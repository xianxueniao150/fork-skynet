#include "skynet.h"

#include "atomic.h"
#include "skynet_env.h"
#include "skynet_handle.h"
#include "skynet_harbor.h"
#include "skynet_imp.h"
#include "skynet_log.h"
#include "skynet_module.h"
#include "skynet_monitor.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_timer.h"
#include "spinlock.h"

#include <pthread.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef CALLING_CHECK

#define CHECKCALLING_BEGIN(ctx)               \
    if (!(spinlock_trylock(&ctx->calling))) { \
        assert(0);                            \
    }
#define CHECKCALLING_END(ctx) spinlock_unlock(&ctx->calling);
#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
#define CHECKCALLING_DESTROY(ctx) spinlock_destroy(&ctx->calling);
#define CHECKCALLING_DECL struct spinlock calling;

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DESTROY(ctx)
#define CHECKCALLING_DECL

#endif

struct skynet_context {
    void *instance;              // 由指定module的create函数，创建的数据实例指针，同一类服务可能有多个实例，因此每个服务都应该有自己的数据
    struct skynet_module *mod;   // 引用服务module的指针，方便后面对create、init、signal和release函数进行调用
    void *cb_ud;                 // 调用callback函数时，回传给callback的userdata，一般是instance指针
    skynet_cb cb;                // 服务的消息回调函数，一般在skynet_module的init函数里指定
    struct message_queue *queue; // 服务专属的次级消息队列指针
    ATOM_POINTER logfile;        // 文件指针是个原子指针
    uint64_t cpu_cost;           // in microsec,消耗的总 cpu 时间
    uint64_t cpu_start;          // in microsec,本次消息处理的起始时间点
    char result[32];             // 操作skynet_context的返回值，会写到这里
    uint32_t handle;             // 标识唯一context的服务id
    int session_id;              // 在发出请求后，收到对方的返回消息时，通过session_id来匹配一个返回，对应哪个请求
    ATOM_INT ref;                // 引用计数变量，当为0时，表示内存可以被释放
    int message_count;           // 处理过的消息总数
    bool init;                   // 初始化完成标记
    bool endless;                // 死循环标志
    bool profile;                // 是否开启了 profile

    CHECKCALLING_DECL
};

struct skynet_node {
    ATOM_INT total;
    int init;
    uint32_t monitor_exit;
    pthread_key_t handle_key;
    bool profile; // default is on
};

static struct skynet_node G_NODE;

int skynet_context_total() { return ATOM_LOAD(&G_NODE.total); }

static void context_inc() { ATOM_FINC(&G_NODE.total); }

static void context_dec() { ATOM_FDEC(&G_NODE.total); }

uint32_t skynet_current_handle(void) {
    if (G_NODE.init) {
        void *handle = pthread_getspecific(G_NODE.handle_key);
        return (uint32_t)(uintptr_t)handle;
    } else {
        uint32_t v = (uint32_t)(-THREAD_MAIN);
        return v;
    }
}

static void id_to_hex(char *str, uint32_t id) {
    int i;
    static char hex[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                           '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    str[0] = ':';
    for (i = 0; i < 8; i++) {
        str[i + 1] = hex[(id >> ((7 - i) * 4)) & 0xf];
    }
    str[9] = '\0';
}

struct drop_t {
    uint32_t handle;
};

static void drop_message(struct skynet_message *msg, void *ud) {
    struct drop_t *d = ud;
    skynet_free(msg->data);
    uint32_t source = d->handle;
    assert(source);
    // report error to the message source
    skynet_send(NULL, source, msg->source, PTYPE_ERROR, 0, NULL, 0);
}

struct skynet_context *
skynet_context_new(const char *name, const char *param) {
    struct skynet_module *mod = skynet_module_query(name);

    if (mod == NULL)
        return NULL;

    //调用crate方法
    void *inst = skynet_module_instance_create(mod);
    if (inst == NULL)
        return NULL;
    //创建一个服务实例即skynet_context对象
    struct skynet_context *ctx = skynet_malloc(sizeof(*ctx));
    CHECKCALLING_INIT(ctx)

    ctx->mod = mod;
    ctx->instance = inst;
    ATOM_INIT(&ctx->ref, 2);
    ctx->cb = NULL;
    ctx->cb_ud = NULL;
    ctx->session_id = 0;
    ATOM_INIT(&ctx->logfile, (uintptr_t)NULL);

    ctx->init = false;
    ctx->endless = false;

    ctx->cpu_cost = 0;
    ctx->cpu_start = 0;
    ctx->message_count = 0;
    ctx->profile = G_NODE.profile;
    // Should set to 0 first to avoid skynet_handle_retireall get an uninitialized
    // handle
    ctx->handle = 0;
    //将新创建的服务实例（skynet_context对象）注册到全局的服务列表中
    ctx->handle = skynet_handle_register(ctx);
    struct message_queue *queue = ctx->queue = skynet_mq_create(ctx->handle);
    // init function maybe use ctx->handle, so it must init at last
    context_inc();

    CHECKCALLING_BEGIN(ctx)
    //调用init方法,init会注册callback函数
    int r = skynet_module_instance_init(mod, inst, ctx, param);
    CHECKCALLING_END(ctx)
    if (r == 0) {
        struct skynet_context *ret = skynet_context_release(ctx);
        if (ret) {
            ctx->init = true;
        }
        //将该服务实例（skynet_context实例）的次级消息队列，插入到全局消息队列中
        skynet_globalmq_push(queue);
        if (ret) {
            skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
        }
        return ret;
    } else {
        skynet_error(ctx, "FAILED launch %s", name);
        uint32_t handle = ctx->handle;
        skynet_context_release(ctx);
        skynet_handle_retire(handle);
        struct drop_t d = {handle};
        skynet_mq_release(queue, drop_message, &d);
        return NULL;
    }
}

int skynet_context_newsession(struct skynet_context *ctx) {
    // session always be a positive number
    int session = ++ctx->session_id;
    if (session <= 0) {
        ctx->session_id = 1;
        return 1;
    }
    return session;
}

void skynet_context_grab(struct skynet_context *ctx) { ATOM_FINC(&ctx->ref); } //增加服务的引用计数

void skynet_context_reserve(struct skynet_context *ctx) {
    skynet_context_grab(ctx);
    // don't count the context reserved, because skynet abort (the worker threads
    // terminate) only when the total context is 0 . the reserved context will be
    // release at last.
    context_dec();
}

static void delete_context(struct skynet_context *ctx) {
    FILE *f = (FILE *)ATOM_LOAD(&ctx->logfile);
    if (f) {
        fclose(f);
    }
    skynet_module_instance_release(ctx->mod, ctx->instance);
    skynet_mq_mark_release(ctx->queue);
    CHECKCALLING_DESTROY(ctx)
    skynet_free(ctx);
    context_dec();
}

struct skynet_context *skynet_context_release(struct skynet_context *ctx) { //减少服务的引用计数
    if (ATOM_FDEC(&ctx->ref) == 1) {
        delete_context(ctx);
        return NULL;
    }
    return ctx;
}

int skynet_context_push(uint32_t handle, struct skynet_message *message) {
    struct skynet_context *ctx = skynet_handle_grab(handle);
    if (ctx == NULL) {
        return -1;
    }
    skynet_mq_push(ctx->queue, message);
    skynet_context_release(ctx);

    return 0;
}

void skynet_context_endless(uint32_t handle) {
    struct skynet_context *ctx = skynet_handle_grab(handle);
    if (ctx == NULL) {
        return;
    }
    ctx->endless = true;
    skynet_context_release(ctx);
}

int skynet_isremote(struct skynet_context *ctx, uint32_t handle, int *harbor) {
    int ret = skynet_harbor_message_isremote(handle);
    if (harbor) {
        *harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
    }
    return ret;
}

static void dispatch_message(struct skynet_context *ctx,
                             struct skynet_message *msg) {
    assert(ctx->init);
    CHECKCALLING_BEGIN(ctx)
    pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));
    int type = msg->sz >> MESSAGE_TYPE_SHIFT;
    size_t sz = msg->sz & MESSAGE_TYPE_MASK;
    FILE *f = (FILE *)ATOM_LOAD(&ctx->logfile);
    if (f) {
        skynet_log_output(f, msg->source, type, msg->session, msg->data, sz);
    }
    ++ctx->message_count;
    int reserve_msg;
    if (ctx->profile) {
        ctx->cpu_start = skynet_thread_time();
        reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source,
                              msg->data, sz);
        uint64_t cost_time = skynet_thread_time() - ctx->cpu_start;
        ctx->cpu_cost += cost_time;
    } else {
        //调用回调函数
        reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source,
                              msg->data, sz);
    }
    if (!reserve_msg) {
        skynet_free(msg->data);
    }
    CHECKCALLING_END(ctx)
}

void skynet_context_dispatchall(struct skynet_context *ctx) {
    // for skynet_error
    struct skynet_message msg;
    struct message_queue *q = ctx->queue;
    while (!skynet_mq_pop(q, &msg)) {
        dispatch_message(ctx, &msg);
    }
}

struct message_queue *skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
    if (q == NULL) {
        q = skynet_globalmq_pop();
        if (q == NULL)
            return NULL;
    }

    //根据次级消息的handle，找出其所属的服务（一个skynet_context实例）指针，从次级消息队列中，pop出n条消息（受weight值影响），并且将其作为参数，传给skynet_context的cb函数，并调用它
    uint32_t handle = skynet_mq_handle(q);

    struct skynet_context *ctx = skynet_handle_grab(handle);
    if (ctx == NULL) { // 如果拿不到对应的服务，说明这消息队列被废弃了
        struct drop_t d = {handle};
        skynet_mq_release(q, drop_message, &d);
        return skynet_globalmq_pop();
    }

    int i, n = 1;
    struct skynet_message msg;

    for (i = 0; i < n; i++) {
        if (skynet_mq_pop(q, &msg)) { // 拿不到消息，直接返回
            skynet_context_release(ctx);
            return skynet_globalmq_pop();
        } else if (i == 0 && weight >= 0) {
            n = skynet_mq_length(q);
            n >>= weight; //消息队列的长度mq_length向右移动weight（权重值）位，就是该线程一次消费次级消息队列的消息数
        }
        int overload = skynet_mq_overload(q);
        if (overload) {
            skynet_error(ctx, "May overload, message queue length = %d", overload);
        }

        skynet_monitor_trigger(sm, msg.source, handle); // 更新 monitor 的记录和计数

        if (ctx->cb == NULL) {
            skynet_free(msg.data);
        } else {
            dispatch_message(ctx, &msg);
        }

        skynet_monitor_trigger(sm, 0, 0);// 更新 monitor 的记录和计数
    }

    //当完成callback函数调用时，就从global_mq中再pop一个次级消息队列中，供下一次使用，并将本次使用的次级消息队列push回global_mq的尾部
    assert(q == ctx->queue);
    struct message_queue *nq = skynet_globalmq_pop();
    if (nq) {
        // If global mq is not empty , push q back, and return next queue (nq)
        // Else (global mq is empty or block, don't push q back, and return q again
        // (for next dispatch)
        skynet_globalmq_push(q);
        q = nq;
    }
    skynet_context_release(ctx);

    return q;
}

static void copy_name(char name[GLOBALNAME_LENGTH], const char *addr) {
    int i;
    for (i = 0; i < GLOBALNAME_LENGTH && addr[i]; i++) {
        name[i] = addr[i];
    }
    for (; i < GLOBALNAME_LENGTH; i++) {
        name[i] = '\0';
    }
}

uint32_t skynet_queryname(struct skynet_context *context, const char *name) {
    switch (name[0]) {
    case ':':
        return strtoul(name + 1, NULL, 16);
    case '.':
        return skynet_handle_findname(name + 1);
    }
    skynet_error(context, "Don't support query global name %s", name);
    return 0;
}

static void handle_exit(struct skynet_context *context, uint32_t handle) {
    if (handle == 0) {
        handle = context->handle;
        skynet_error(context, "KILL self");
    } else {
        skynet_error(context, "KILL :%0x", handle);
    }
    if (G_NODE.monitor_exit) {
        skynet_send(context, handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);
    }
    skynet_handle_retire(handle);
}

// skynet command

struct command_func {
    const char *name;
    const char *(*func)(struct skynet_context *context, const char *param);
};

static const char *cmd_timeout(struct skynet_context *context,
                               const char *param) {
    char *session_ptr = NULL;
    int ti = strtol(param, &session_ptr, 10);
    int session = skynet_context_newsession(context);
    skynet_timeout(context->handle, ti, session);
    sprintf(context->result, "%d", session);
    return context->result;
}

static const char *cmd_reg(struct skynet_context *context, const char *param) {
    if (param == NULL || param[0] == '\0') {
        sprintf(context->result, ":%x", context->handle);
        return context->result;
    } else if (param[0] == '.') {
        return skynet_handle_namehandle(context->handle, param + 1);
    } else {
        skynet_error(context, "Can't register global name %s in C", param);
        return NULL;
    }
}

static const char *cmd_query(struct skynet_context *context,
                             const char *param) {
    if (param[0] == '.') {
        uint32_t handle = skynet_handle_findname(param + 1);
        if (handle) {
            sprintf(context->result, ":%x", handle);
            return context->result;
        }
    }
    return NULL;
}

static const char *cmd_name(struct skynet_context *context, const char *param) {
    int size = strlen(param);
    char name[size + 1];
    char handle[size + 1];
    sscanf(param, "%s %s", name, handle);
    if (handle[0] != ':') {
        return NULL;
    }
    uint32_t handle_id = strtoul(handle + 1, NULL, 16);
    if (handle_id == 0) {
        return NULL;
    }
    if (name[0] == '.') {
        return skynet_handle_namehandle(handle_id, name + 1);
    } else {
        skynet_error(context, "Can't set global name %s in C", name);
    }
    return NULL;
}

static const char *cmd_exit(struct skynet_context *context, const char *param) {
    handle_exit(context, 0);
    return NULL;
}

static uint32_t tohandle(struct skynet_context *context, const char *param) {
    uint32_t handle = 0;
    if (param[0] == ':') {
        handle = strtoul(param + 1, NULL, 16);
    } else if (param[0] == '.') {
        handle = skynet_handle_findname(param + 1);
    } else {
        skynet_error(context, "Can't convert %s to handle", param);
    }

    return handle;
}

static const char *cmd_kill(struct skynet_context *context, const char *param) {
    uint32_t handle = tohandle(context, param);
    if (handle) {
        handle_exit(context, handle);
    }
    return NULL;
}

static const char *cmd_launch(struct skynet_context *context,
                              const char *param) {
    size_t sz = strlen(param);
    char tmp[sz + 1];
    strcpy(tmp, param);
    char *args = tmp;
    char *mod = strsep(&args, " \t\r\n");
    // printf("cmd_launch,args:%s,mod:%s\n", args, mod); //cmd_launch,args:service_mgr,mod:snlua
    args = strsep(&args, "\r\n");
    struct skynet_context *inst = skynet_context_new(mod, args);
    if (inst == NULL) {
        return NULL;
    } else {
        id_to_hex(context->result, inst->handle);
        return context->result;
    }
}

static const char *cmd_getenv(struct skynet_context *context,
                              const char *param) {
    return skynet_getenv(param);
}

static const char *cmd_setenv(struct skynet_context *context,
                              const char *param) {
    size_t sz = strlen(param);
    char key[sz + 1];
    int i;
    for (i = 0; param[i] != ' ' && param[i]; i++) {
        key[i] = param[i];
    }
    if (param[i] == '\0')
        return NULL;

    key[i] = '\0';
    param += i + 1;

    skynet_setenv(key, param);
    return NULL;
}

static const char *cmd_starttime(struct skynet_context *context,
                                 const char *param) {
    uint32_t sec = skynet_starttime();
    sprintf(context->result, "%u", sec);
    return context->result;
}

static const char *cmd_abort(struct skynet_context *context,
                             const char *param) {
    skynet_handle_retireall();
    return NULL;
}

static const char *cmd_monitor(struct skynet_context *context,
                               const char *param) {
    uint32_t handle = 0;
    if (param == NULL || param[0] == '\0') {
        if (G_NODE.monitor_exit) {
            // return current monitor serivce
            sprintf(context->result, ":%x", G_NODE.monitor_exit);
            return context->result;
        }
        return NULL;
    } else {
        handle = tohandle(context, param);
    }
    G_NODE.monitor_exit = handle;
    return NULL;
}

static const char *cmd_stat(struct skynet_context *context, const char *param) {
    if (strcmp(param, "mqlen") == 0) {
        int len = skynet_mq_length(context->queue);
        sprintf(context->result, "%d", len);
    } else if (strcmp(param, "endless") == 0) {
        if (context->endless) {
            strcpy(context->result, "1");
            context->endless = false;
        } else {
            strcpy(context->result, "0");
        }
    } else if (strcmp(param, "cpu") == 0) {
        double t = (double)context->cpu_cost / 1000000.0; // microsec
        sprintf(context->result, "%lf", t);
    } else if (strcmp(param, "time") == 0) {
        if (context->profile) {
            uint64_t ti = skynet_thread_time() - context->cpu_start;
            double t = (double)ti / 1000000.0; // microsec
            sprintf(context->result, "%lf", t);
        } else {
            strcpy(context->result, "0");
        }
    } else if (strcmp(param, "message") == 0) {
        sprintf(context->result, "%d", context->message_count);
    } else {
        context->result[0] = '\0';
    }
    return context->result;
}

static const char *cmd_logon(struct skynet_context *context,
                             const char *param) {
    uint32_t handle = tohandle(context, param);
    if (handle == 0)
        return NULL;
    struct skynet_context *ctx = skynet_handle_grab(handle);
    if (ctx == NULL)
        return NULL;
    FILE *f = NULL;
    FILE *lastf = (FILE *)ATOM_LOAD(&ctx->logfile);
    if (lastf == NULL) {
        f = skynet_log_open(context, handle);
        if (f) {
            if (!ATOM_CAS_POINTER(&ctx->logfile, 0, (uintptr_t)f)) {
                // logfile opens in other thread, close this one.
                fclose(f);
            }
        }
    }
    skynet_context_release(ctx);
    return NULL;
}

static const char *cmd_logoff(struct skynet_context *context,
                              const char *param) {
    uint32_t handle = tohandle(context, param);
    if (handle == 0)
        return NULL;
    struct skynet_context *ctx = skynet_handle_grab(handle);
    if (ctx == NULL)
        return NULL;
    FILE *f = (FILE *)ATOM_LOAD(&ctx->logfile);
    if (f) {
        // logfile may close in other thread
        if (ATOM_CAS_POINTER(&ctx->logfile, (uintptr_t)f, (uintptr_t)NULL)) {
            skynet_log_close(context, f, handle);
        }
    }
    skynet_context_release(ctx);
    return NULL;
}

static const char *cmd_signal(struct skynet_context *context,
                              const char *param) {
    uint32_t handle = tohandle(context, param);
    if (handle == 0)
        return NULL;
    struct skynet_context *ctx = skynet_handle_grab(handle);
    if (ctx == NULL)
        return NULL;
    param = strchr(param, ' ');
    int sig = 0;
    if (param) {
        sig = strtol(param, NULL, 0);
    }
    // NOTICE: the signal function should be thread safe.
    skynet_module_instance_signal(ctx->mod, ctx->instance, sig);

    skynet_context_release(ctx);
    return NULL;
}

static struct command_func cmd_funcs[] = {
    {"TIMEOUT", cmd_timeout},
    {"REG", cmd_reg},
    {"QUERY", cmd_query},
    {"NAME", cmd_name},
    {"EXIT", cmd_exit},
    {"KILL", cmd_kill},
    {"LAUNCH", cmd_launch},
    {"GETENV", cmd_getenv},
    {"SETENV", cmd_setenv},
    {"STARTTIME", cmd_starttime},
    {"ABORT", cmd_abort},
    {"MONITOR", cmd_monitor},
    {"STAT", cmd_stat},
    {"LOGON", cmd_logon},
    {"LOGOFF", cmd_logoff},
    {"SIGNAL", cmd_signal},
    {NULL, NULL},
};

const char *skynet_command(struct skynet_context *context, const char *cmd,
                           const char *param) {
    struct command_func *method = &cmd_funcs[0];
    while (method->name) {
        if (strcmp(cmd, method->name) == 0) {
            return method->func(context, param);
        }
        ++method;
    }

    return NULL;
}

static void _filter_args(struct skynet_context *context, int type, int *session,
                         void **data, size_t *sz) {
    int needcopy = !(type & PTYPE_TAG_DONTCOPY);
    int allocsession = type & PTYPE_TAG_ALLOCSESSION;
    type &= 0xff;

    if (allocsession) {
        assert(*session == 0);
        *session = skynet_context_newsession(context);
    }

    if (needcopy && *data) {
        char *msg = skynet_malloc(*sz + 1);
        memcpy(msg, *data, *sz);
        msg[*sz] = '\0';
        *data = msg;
    }

    *sz |= (size_t)type << MESSAGE_TYPE_SHIFT;
}

int skynet_send(struct skynet_context *context, uint32_t source,
                uint32_t destination, int type, int session, void *data,
                size_t sz) {
    if ((sz & MESSAGE_TYPE_MASK) != sz) {
        skynet_error(context, "The message to %x is too large", destination);
        if (type & PTYPE_TAG_DONTCOPY) {
            skynet_free(data);
        }
        return -2;
    }
    _filter_args(context, type, &session, (void **)&data, &sz);

    if (source == 0) {
        source = context->handle;
    }

    if (destination == 0) {
        if (data) {
            skynet_error(context, "Destination address can't be 0");
            skynet_free(data);
            return -1;
        }

        return session;
    }
    if (skynet_harbor_message_isremote(destination)) {
        struct remote_message *rmsg = skynet_malloc(sizeof(*rmsg));
        rmsg->destination.handle = destination;
        rmsg->message = data;
        rmsg->sz = sz & MESSAGE_TYPE_MASK;
        rmsg->type = sz >> MESSAGE_TYPE_SHIFT;
        skynet_harbor_send(rmsg, source, session);
    } else {
        struct skynet_message smsg;
        smsg.source = source;
        smsg.session = session;
        smsg.data = data;
        smsg.sz = sz;

        if (skynet_context_push(destination, &smsg)) {
            skynet_free(data);
            return -1;
        }
    }
    return session;
}

int skynet_sendname(struct skynet_context *context, uint32_t source,
                    const char *addr, int type, int session, void *data,
                    size_t sz) {
    if (source == 0) {
        source = context->handle;
    }
    uint32_t des = 0;
    if (addr[0] == ':') {
        des = strtoul(addr + 1, NULL, 16);
    } else if (addr[0] == '.') {
        des = skynet_handle_findname(addr + 1);
        if (des == 0) {
            if (type & PTYPE_TAG_DONTCOPY) {
                skynet_free(data);
            }
            return -1;
        }
    } else {
        if ((sz & MESSAGE_TYPE_MASK) != sz) {
            skynet_error(context, "The message to %s is too large", addr);
            if (type & PTYPE_TAG_DONTCOPY) {
                skynet_free(data);
            }
            return -2;
        }
        _filter_args(context, type, &session, (void **)&data, &sz);

        struct remote_message *rmsg = skynet_malloc(sizeof(*rmsg));
        copy_name(rmsg->destination.name, addr);
        rmsg->destination.handle = 0;
        rmsg->message = data;
        rmsg->sz = sz & MESSAGE_TYPE_MASK;
        rmsg->type = sz >> MESSAGE_TYPE_SHIFT;

        skynet_harbor_send(rmsg, source, session);
        return session;
    }

    return skynet_send(context, source, des, type, session, data, sz);
}

uint32_t skynet_context_handle(struct skynet_context *ctx) {
    return ctx->handle;
}

void skynet_callback(struct skynet_context *context, void *ud, skynet_cb cb) {
    context->cb = cb;
    context->cb_ud = ud;
}

void skynet_context_send(struct skynet_context *ctx, void *msg, size_t sz,
                         uint32_t source, int type, int session) {
    struct skynet_message smsg;
    smsg.source = source;
    smsg.session = session;
    smsg.data = msg;
    smsg.sz = sz | (size_t)type << MESSAGE_TYPE_SHIFT;

    skynet_mq_push(ctx->queue, &smsg);
}

void skynet_globalinit(void) {
    ATOM_INIT(&G_NODE.total, 0);
    G_NODE.monitor_exit = 0;
    G_NODE.init = 1;
    if (pthread_key_create(&G_NODE.handle_key, NULL)) {
        fprintf(stderr, "pthread_key_create failed");
        exit(1);
    }
    // set mainthread's key
    skynet_initthread(THREAD_MAIN);
}

void skynet_globalexit(void) { pthread_key_delete(G_NODE.handle_key); }

void skynet_initthread(int m) {
    uintptr_t v = (uint32_t)(-m);
    pthread_setspecific(G_NODE.handle_key, (void *)v);
}

void skynet_profile_enable(int enable) { G_NODE.profile = (bool)enable; }
