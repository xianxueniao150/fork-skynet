#include "skynet.h"

#include "atomic.h"
#include "socket_poll.h"
#include "socket_server.h"
#include "spinlock.h"

#include <assert.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_INFO 128
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16
#define MAX_EVENT 64
#define MIN_READ_BUFFER 64
#define SOCKET_TYPE_INVALID 0         // 初始状态
#define SOCKET_TYPE_RESERVE 1         // 保留状态
#define SOCKET_TYPE_PLISTEN 2         // 监听前状态
#define SOCKET_TYPE_LISTEN 3          // 监听中状态
#define SOCKET_TYPE_CONNECTING 4      // 连接中状态
#define SOCKET_TYPE_CONNECTED 5       // 已连接状态
#define SOCKET_TYPE_HALFCLOSE_READ 6  // 半关闭剩下读
#define SOCKET_TYPE_HALFCLOSE_WRITE 7 // 半关闭剩下写
#define SOCKET_TYPE_PACCEPT 8         // 已接收，但是未添加到epoll（需要start）
#define SOCKET_TYPE_BIND 9            // 绑定系统fd(stdin、stdout、stderr)

#define MAX_SOCKET (1 << MAX_SOCKET_P)

#define PRIORITY_HIGH 0
#define PRIORITY_LOW 1

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)
#define ID_TAG16(id) ((id >> MAX_SOCKET_P) & 0xffff)

#define PROTOCOL_TCP 0
#define PROTOCOL_UDP 1
#define PROTOCOL_UDPv6 2
#define PROTOCOL_UNKNOWN 255

#define UDP_ADDRESS_SIZE 19 // ipv6 128bit + port 16bit + 1 byte type

#define MAX_UDP_PACKAGE 65535

// EAGAIN and EWOULDBLOCK may be not the same value.
#if (EAGAIN != EWOULDBLOCK)
#define AGAIN_WOULDBLOCK \
    EAGAIN:              \
    case EWOULDBLOCK
#else
#define AGAIN_WOULDBLOCK EAGAIN
#endif

#define WARNING_SIZE (1024 * 1024)

#define USEROBJECT ((size_t)(-1))

struct write_buffer {
    struct write_buffer *next;
    const void *buffer; //发送的应用层内存块
    char *ptr;          //记录下次发送的起点
    size_t sz;          //表示这个buffer未发送的字节还有多少个(初始化时，就是buffer的完整大小，每发出n个字节，它就减去n)
    bool userobject;
};

struct write_buffer_udp {
    struct write_buffer buffer;
    uint8_t udp_address[UDP_ADDRESS_SIZE];
};

struct wb_list {
    struct write_buffer *head;
    struct write_buffer *tail;
};

struct socket_stat {
    uint64_t rtime; // 最近一次读取时间
    uint64_t wtime; // 最近一次写入时间
    uint64_t read;  // 已读取的总数据长度
    uint64_t write; // 已写入的总数据长度
};

struct socket {
    uintptr_t opaque;        // 与之关联的服务id
    struct wb_list high;     // 高优先级写入队列
    struct wb_list low;      // 低优先级写入队列
    int64_t wb_size;         // 待发送字节数
    struct socket_stat stat; // 读写统计信息(读写的字节总数、最后读写时间)
    ATOM_ULONG sending;      // 是否有worker线程正在通过管道，发送数据包给socket线程的标记
    int fd;                  // 套接字
    int id;                  // 自增，范围在 0x01 ~ 0x7fffffff,由 skynet 分配的用来标识 socket 结构体变量的唯一标识符。之所以在有了 fd 以后还需要 id 是因为内核可能会重用 fd，并不能用 fd 来做唯一标识
    ATOM_INT type;           // 当前连接状态
    uint8_t protocol;        // 连接协议
    bool reading;            // fd 的 read 监听标记
    bool writing;            // fd 的 write 监听标记
    bool closing;            // fd 的 close 标记
    ATOM_INT udpconnecting;  // 正在连接中的 udp 数量，发起连接时累加，连接成功后递减
    int64_t warn_size;       // 报警阈值,如果超过了这个值，会输出一条警告信息。阈值每次触发以后会变为原来的两倍。
    union {
        int size;                              // tcp使用size表示每次读取数据的字节数,会被初始化 64，并且会根据每次从 fd 中读取数据的情况增加或者减少
        uint8_t udp_address[UDP_ADDRESS_SIZE]; // udp使用 udp_address表示远端地址信息
    } p;
    struct spinlock dw_lock; // 避免worker线程 和socket线程，同时对同一个socket实例的send buffer进行操作

    int dw_offset;         // 已经直接写入的数据大小
    const void *dw_buffer; // 在worker线程中，如果不能将buffer一次写完，skynet会将这块buffer的地址放在dw_buffer中
    size_t dw_size;        //总数据大小
};

struct socket_server {
    volatile uint64_t time; // 当前时间值，每隔2.5毫秒更新一次，由 timer 线程更新
    int reserve_fd;         // for EMFILE
    int recvctrl_fd;        // 接收命令的管道套接字
    int sendctrl_fd;        // 发送命令的管道套接字
    int checkctrl;          // 是否需要检查内部命令的标识
    poll_fd event_fd;
    ATOM_INT alloc_id; // socket id分配器
    int event_n;       // 一次捕获到事件总数，该值小于等于 MAX_EVENT
    int event_index;   // 已处理过的事件索引
    struct socket_object_interface soi;
    struct event ev[MAX_EVENT];     //当epoll实例中的interest list中，有fd的IO准备就绪时，响应的事件信息，就会被填写到这个结构中
    struct socket slot[MAX_SOCKET]; //在skynet中，每个系统socket被创建出来的时候，都会有一个skynet的 socket实例与之对应，skynet最多可以支持(2^16)个socket
    char buffer[MAX_INFO];          // 临时存放需要转发给其他服务的消息数据
    uint8_t udpbuffer[MAX_UDP_PACKAGE];
    fd_set rfds; //select使用
};

struct request_open {
    int id;
    int port;
    uintptr_t opaque;
    char host[1];
};

struct request_send {
    int id;
    size_t sz;
    const void *buffer;
};

struct request_send_udp {
    struct request_send send;
    uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_setudp {
    int id;
    uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_close {
    int id;           // socket slot的id
    int shutdown;     // 是否强制关闭，如果是，则无视发送缓存是否清空，直接强制关闭
    uintptr_t opaque; // 和socket实例关联的服务地址
};

struct request_listen {
    int id;
    int fd;
    uintptr_t opaque;
    char host[1];
};

struct request_bind {
    int id;
    int fd;
    uintptr_t opaque;
};

struct request_resumepause {
    int id;
    uintptr_t opaque;
};

struct request_setopt {
    int id;
    int what;
    int value;
};

struct request_udp {
    int id;
    int fd;
    int family;
    uintptr_t opaque;
};

/*
	The first byte is TYPE

	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
	Q query info
 */

struct request_package {
    uint8_t header[8]; // 6 bytes dummy
    union {
        char buffer[256];
        struct request_open open;
        struct request_send send;
        struct request_send_udp send_udp;
        struct request_close close;
        struct request_listen listen;
        struct request_bind bind;
        struct request_resumepause resumepause;
        struct request_setopt setopt;
        struct request_udp udp;
        struct request_setudp set_udp;
    } u;
    uint8_t dummy[256];
};

union sockaddr_all {
    struct sockaddr s;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
};

struct send_object {
    const void *buffer;
    size_t sz;
    void (*free_func)(void *);
};

#define MALLOC skynet_malloc
#define FREE skynet_free

struct socket_lock {
    struct spinlock *lock;
    int count;
};

static inline void
socket_lock_init(struct socket *s, struct socket_lock *sl) {
    sl->lock = &s->dw_lock;
    sl->count = 0;
}

static inline void
socket_lock(struct socket_lock *sl) {
    if (sl->count == 0) {
        spinlock_lock(sl->lock);
    }
    ++sl->count;
}

static inline int
socket_trylock(struct socket_lock *sl) {
    if (sl->count == 0) {
        if (!spinlock_trylock(sl->lock))
            return 0; // lock failed
    }
    ++sl->count;
    return 1;
}

static inline void
socket_unlock(struct socket_lock *sl) {
    --sl->count;
    if (sl->count <= 0) {
        assert(sl->count == 0);
        spinlock_unlock(sl->lock);
    }
}

static inline int
socket_invalid(struct socket *s, int id) {
    return (s->id != id || ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID);
}

static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, const void *object, size_t sz) {
    if (sz == USEROBJECT) {
        so->buffer = ss->soi.buffer(object);
        so->sz = ss->soi.size(object);
        so->free_func = ss->soi.free;
        return true;
    } else {
        so->buffer = object;
        so->sz = sz;
        so->free_func = FREE;
        return false;
    }
}

static void
dummy_free(void *ptr) {
    (void)ptr;
}

static inline void
send_object_init_from_sendbuffer(struct socket_server *ss, struct send_object *so, struct socket_sendbuffer *buf) {
    switch (buf->type) {
    case SOCKET_BUFFER_MEMORY:
        send_object_init(ss, so, buf->buffer, buf->sz);
        break;
    case SOCKET_BUFFER_OBJECT:
        send_object_init(ss, so, buf->buffer, USEROBJECT);
        break;
    case SOCKET_BUFFER_RAWPOINTER:
        so->buffer = buf->buffer;
        so->sz = buf->sz;
        so->free_func = dummy_free;
        break;
    default:
        // never get here
        so->buffer = NULL;
        so->sz = 0;
        so->free_func = NULL;
        break;
    }
}

static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
    if (wb->userobject) {
        ss->soi.free((void *)wb->buffer);
    } else {
        FREE((void *)wb->buffer);
    }
    FREE(wb);
}

static void
socket_keepalive(int fd) {
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive, sizeof(keepalive));
}

//循环查找 slot 数组中的 socket 实例，同时自增 alloc_id 并通过 HASH_ID 计算得到 slot 数组下标，直到找到一个处于空闲状态的的 socket 实例。
static int reserve_id(struct socket_server *ss) {
    int i;
    for (i = 0; i < MAX_SOCKET; i++) {
        int id = ATOM_FINC(&(ss->alloc_id)) + 1;
        if (id < 0) {
            id = ATOM_FAND(&(ss->alloc_id), 0x7fffffff) & 0x7fffffff;
        }
        struct socket *s = &ss->slot[HASH_ID(id)];
        int type_invalid = ATOM_LOAD(&s->type);
        if (type_invalid == SOCKET_TYPE_INVALID) {
            if (ATOM_CAS(&s->type, type_invalid, SOCKET_TYPE_RESERVE)) {
                s->id = id;
                s->protocol = PROTOCOL_UNKNOWN;
                // socket_server_udp_connect may inc s->udpconncting directly (from other thread, before new_fd),
                // so reset it to 0 here rather than in new_fd.
                ATOM_INIT(&s->udpconnecting, 0);
                s->fd = -1;
                return id;
            } else {
                // retry
                --i;
            }
        }
    }
    return -1;
}

static inline void
clear_wb_list(struct wb_list *list) {
    list->head = NULL;
    list->tail = NULL;
}

struct socket_server *
socket_server_create(uint64_t time) {
    int i;
    int fd[2];
    //skynet 使用的是 epoll 默认的水平触发模式
    poll_fd efd = sp_create();
    if (sp_invalid(efd)) {
        skynet_error(NULL, "socket-server: create event pool failed.");
        return NULL;
    }
    if (pipe(fd)) {
        sp_release(efd);
        skynet_error(NULL, "socket-server: create socket pair failed.");
        return NULL;
    }
    if (sp_add(efd, fd[0], NULL)) { // add recvctrl_fd to event poll
        skynet_error(NULL, "socket-server: can't add server fd to event pool.");
        close(fd[0]);
        close(fd[1]);
        sp_release(efd);
        return NULL;
    }

    struct socket_server *ss = MALLOC(sizeof(*ss));
    ss->time = time;
    ss->event_fd = efd;
    ss->recvctrl_fd = fd[0];
    ss->sendctrl_fd = fd[1];
    ss->checkctrl = 1;
    ss->reserve_fd = dup(1); // reserve an extra fd for EMFILE

    for (i = 0; i < MAX_SOCKET; i++) {
        struct socket *s = &ss->slot[i];
        ATOM_INIT(&s->type, SOCKET_TYPE_INVALID);
        clear_wb_list(&s->high);
        clear_wb_list(&s->low);
        spinlock_init(&s->dw_lock);
    }
    ATOM_INIT(&ss->alloc_id, 0);
    ss->event_n = 0;
    ss->event_index = 0;
    memset(&ss->soi, 0, sizeof(ss->soi));
    FD_ZERO(&ss->rfds);
    assert(ss->recvctrl_fd < FD_SETSIZE);

    return ss;
}

void socket_server_updatetime(struct socket_server *ss, uint64_t time) {
    ss->time = time;
}

static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
    struct write_buffer *wb = list->head;
    while (wb) {
        struct write_buffer *tmp = wb;
        wb = wb->next;
        write_buffer_free(ss, tmp);
    }
    list->head = NULL;
    list->tail = NULL;
}

static void
free_buffer(struct socket_server *ss, struct socket_sendbuffer *buf) {
    void *buffer = (void *)buf->buffer;
    switch (buf->type) {
    case SOCKET_BUFFER_MEMORY:
        FREE(buffer);
        break;
    case SOCKET_BUFFER_OBJECT:
        ss->soi.free(buffer);
        break;
    case SOCKET_BUFFER_RAWPOINTER:
        break;
    }
}

static const void *
clone_buffer(struct socket_sendbuffer *buf, size_t *sz) {
    switch (buf->type) {
    case SOCKET_BUFFER_MEMORY:
        *sz = buf->sz;
        return buf->buffer;
    case SOCKET_BUFFER_OBJECT:
        *sz = USEROBJECT;
        return buf->buffer;
    case SOCKET_BUFFER_RAWPOINTER:
        // It's a raw pointer, we need make a copy
        *sz = buf->sz;
        void *tmp = MALLOC(*sz);
        memcpy(tmp, buf->buffer, *sz);
        return tmp;
    }
    // never get here
    *sz = 0;
    return NULL;
}

static void
force_close(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    result->id = s->id;
    result->ud = 0;
    result->data = NULL;
    result->opaque = s->opaque;
    uint8_t type = ATOM_LOAD(&s->type);
    if (type == SOCKET_TYPE_INVALID) {
        return;
    }
    assert(type != SOCKET_TYPE_RESERVE);
    free_wb_list(ss, &s->high);
    free_wb_list(ss, &s->low);
    sp_del(ss->event_fd, s->fd);
    socket_lock(l);
    if (type != SOCKET_TYPE_BIND) {
        if (close(s->fd) < 0) {
            perror("close socket:");
        }
    }
    ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
    if (s->dw_buffer) {
        struct socket_sendbuffer tmp;
        tmp.buffer = s->dw_buffer;
        tmp.sz = s->dw_size;
        tmp.id = s->id;
        tmp.type = (tmp.sz == USEROBJECT) ? SOCKET_BUFFER_OBJECT : SOCKET_BUFFER_MEMORY;
        free_buffer(ss, &tmp);
        s->dw_buffer = NULL;
    }
    socket_unlock(l);
}

void socket_server_release(struct socket_server *ss) {
    int i;
    struct socket_message dummy;
    for (i = 0; i < MAX_SOCKET; i++) {
        struct socket *s = &ss->slot[i];
        struct socket_lock l;
        socket_lock_init(s, &l);
        if (ATOM_LOAD(&s->type) != SOCKET_TYPE_RESERVE) {
            force_close(ss, s, &l, &dummy);
        }
        spinlock_destroy(&s->dw_lock);
    }
    close(ss->sendctrl_fd);
    close(ss->recvctrl_fd);
    sp_release(ss->event_fd);
    if (ss->reserve_fd >= 0)
        close(ss->reserve_fd);
    FREE(ss);
}

static inline void
check_wb_list(struct wb_list *s) {
    assert(s->head == NULL);
    assert(s->tail == NULL);
}

static inline int
enable_write(struct socket_server *ss, struct socket *s, bool enable) {
    if (s->writing != enable) {
        s->writing = enable;
        return sp_enable(ss->event_fd, s->fd, s, s->reading, enable);
    }
    return 0;
}

//打开或关闭监听读事件
static inline int
enable_read(struct socket_server *ss, struct socket *s, bool enable) {
    if (s->reading != enable) {
        s->reading = enable;
        return sp_enable(ss->event_fd, s->fd, s, enable, s->writing);
    }
    return 0;
}

//如果传入的reading为false，那么epoll将不会监听读事件
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool reading) {
    struct socket *s = &ss->slot[HASH_ID(id)];
    assert(ATOM_LOAD(&s->type) == SOCKET_TYPE_RESERVE);

    if (sp_add(ss->event_fd, fd, s)) {
        ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
        return NULL;
    }

    s->id = id;
    s->fd = fd;
    s->reading = true;
    s->writing = false;
    s->closing = false;
    ATOM_INIT(&s->sending, ID_TAG16(id) << 16 | 0);
    s->protocol = protocol;
    s->p.size = MIN_READ_BUFFER;
    s->opaque = opaque;
    s->wb_size = 0;
    s->warn_size = 0;
    check_wb_list(&s->high);
    check_wb_list(&s->low);
    s->dw_buffer = NULL;
    s->dw_size = 0;
    memset(&s->stat, 0, sizeof(s->stat));
    if (enable_read(ss, s, reading)) {
        ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
        return NULL;
    }
    return s;
}

static inline void
stat_read(struct socket_server *ss, struct socket *s, int n) {
    s->stat.read += n;
    s->stat.rtime = ss->time;
}

static inline void
stat_write(struct socket_server *ss, struct socket *s, int n) {
    s->stat.write += n;
    s->stat.wtime = ss->time;
}

// return -1 when connecting
static int
open_socket(struct socket_server *ss, struct request_open *request, struct socket_message *result) {
    int id = request->id;
    result->opaque = request->opaque;
    result->id = id;
    result->ud = 0;
    result->data = NULL;
    struct socket *ns;
    int status;
    struct addrinfo ai_hints;
    struct addrinfo *ai_list = NULL;
    struct addrinfo *ai_ptr = NULL;
    char port[16];
    sprintf(port, "%d", request->port);
    memset(&ai_hints, 0, sizeof(ai_hints));
    ai_hints.ai_family = AF_UNSPEC;
    ai_hints.ai_socktype = SOCK_STREAM;
    ai_hints.ai_protocol = IPPROTO_TCP;

    status = getaddrinfo(request->host, port, &ai_hints, &ai_list);
    if (status != 0) {
        result->data = (void *)gai_strerror(status);
        goto _failed_getaddrinfo;
    }
    int sock = -1;
    for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
        sock = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
        if (sock < 0) {
            continue;
        }
        socket_keepalive(sock);
        sp_nonblocking(sock);
        status = connect(sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
        if (status != 0 && errno != EINPROGRESS) {
            close(sock);
            sock = -1;
            continue;
        }
        break;
    }

    if (sock < 0) {
        result->data = strerror(errno);
        goto _failed;
    }

    ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);
    if (ns == NULL) {
        result->data = "reach skynet socket number limit";
        goto _failed;
    }

    if (status == 0) {
        ATOM_STORE(&ns->type, SOCKET_TYPE_CONNECTED);
        struct sockaddr *addr = ai_ptr->ai_addr;
        void *sin_addr = (ai_ptr->ai_family == AF_INET) ? (void *)&((struct sockaddr_in *)addr)->sin_addr : (void *)&((struct sockaddr_in6 *)addr)->sin6_addr;
        if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
            result->data = ss->buffer;
        }
        freeaddrinfo(ai_list);
        return SOCKET_OPEN;
    } else {
        if (enable_write(ss, ns, true)) {
            result->data = "enable write failed";
            goto _failed;
        }
        ATOM_STORE(&ns->type, SOCKET_TYPE_CONNECTING);
    }

    freeaddrinfo(ai_list);
    return -1;
_failed:
    if (sock >= 0)
        close(sock);
    freeaddrinfo(ai_list);
_failed_getaddrinfo:
    ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
    return SOCKET_ERR;
}

static int
report_error(struct socket *s, struct socket_message *result, const char *err) {
    result->id = s->id;
    result->ud = 0;
    result->opaque = s->opaque;
    result->data = (char *)err;
    return SOCKET_ERR;
}

static int
close_write(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    if (s->closing) {
        force_close(ss, s, l, result);
        return SOCKET_RST;
    } else {
        int t = ATOM_LOAD(&s->type);
        if (t == SOCKET_TYPE_HALFCLOSE_READ) {
            // recv 0 before, ignore the error and close fd
            force_close(ss, s, l, result);
            return SOCKET_RST;
        }
        if (t == SOCKET_TYPE_HALFCLOSE_WRITE) {
            // already raise SOCKET_ERR
            return SOCKET_RST;
        }
        ATOM_STORE(&s->type, SOCKET_TYPE_HALFCLOSE_WRITE);
        shutdown(s->fd, SHUT_WR);
        enable_write(ss, s, false);
        return report_error(s, result, strerror(errno));
    }
}

static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l, struct socket_message *result) {
    while (list->head) {
        struct write_buffer *tmp = list->head;
        for (;;) {
            ssize_t sz = write(s->fd, tmp->ptr, tmp->sz);
            if (sz < 0) {
                switch (errno) {
                case EINTR:
                    continue;
                case AGAIN_WOULDBLOCK:
                    return -1;
                }
                return close_write(ss, s, l, result);
            }
            stat_write(ss, s, (int)sz);
            s->wb_size -= sz;
            if (sz != tmp->sz) {
                tmp->ptr += sz;
                tmp->sz -= sz;
                return -1;
            }
            break;
        }
        list->head = tmp->next;
        write_buffer_free(ss, tmp);
    }
    list->tail = NULL;

    return -1;
}

static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
    int type = (uint8_t)udp_address[0];
    if (type != s->protocol)
        return 0;
    uint16_t port = 0;
    memcpy(&port, udp_address + 1, sizeof(uint16_t));
    switch (s->protocol) {
    case PROTOCOL_UDP:
        memset(&sa->v4, 0, sizeof(sa->v4));
        sa->s.sa_family = AF_INET;
        sa->v4.sin_port = port;
        memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr)); // ipv4 address is 32 bits
        return sizeof(sa->v4);
    case PROTOCOL_UDPv6:
        memset(&sa->v6, 0, sizeof(sa->v6));
        sa->s.sa_family = AF_INET6;
        sa->v6.sin6_port = port;
        memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr)); // ipv6 address is 128 bits
        return sizeof(sa->v6);
    }
    return 0;
}

static void
drop_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct write_buffer *tmp) {
    s->wb_size -= tmp->sz;
    list->head = tmp->next;
    if (list->head == NULL)
        list->tail = NULL;
    write_buffer_free(ss, tmp);
}

static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
    while (list->head) {
        struct write_buffer *tmp = list->head;
        struct write_buffer_udp *udp = (struct write_buffer_udp *)tmp;
        union sockaddr_all sa;
        socklen_t sasz = udp_socket_address(s, udp->udp_address, &sa);
        if (sasz == 0) {
            skynet_error(NULL, "socket-server : udp (%d) type mismatch.", s->id);
            drop_udp(ss, s, list, tmp);
            return -1;
        }
        int err = sendto(s->fd, tmp->ptr, tmp->sz, 0, &sa.s, sasz);
        if (err < 0) {
            switch (errno) {
            case EINTR:
            case AGAIN_WOULDBLOCK:
                return -1;
            }
            skynet_error(NULL, "socket-server : udp (%d) sendto error %s.", s->id, strerror(errno));
            drop_udp(ss, s, list, tmp);
            return -1;
        }
        stat_write(ss, s, tmp->sz);
        s->wb_size -= tmp->sz;
        list->head = tmp->next;
        write_buffer_free(ss, tmp);
    }
    list->tail = NULL;

    return -1;
}

static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l, struct socket_message *result) {
    if (s->protocol == PROTOCOL_TCP) {
        return send_list_tcp(ss, s, list, l, result);
    } else {
        return send_list_udp(ss, s, list, result);
    }
}

static inline int
list_uncomplete(struct wb_list *s) {
    struct write_buffer *wb = s->head;
    if (wb == NULL)
        return 0;

    return (void *)wb->ptr != wb->buffer;
}

static void
raise_uncomplete(struct socket *s) {
    struct wb_list *low = &s->low;
    struct write_buffer *tmp = low->head;
    low->head = tmp->next;
    if (low->head == NULL) {
        low->tail = NULL;
    }

    // move head of low list (tmp) to the empty high list
    struct wb_list *high = &s->high;
    assert(high->head == NULL);

    tmp->next = NULL;
    high->head = high->tail = tmp;
}

static inline int
send_buffer_empty(struct socket *s) {
    return (s->high.head == NULL && s->low.head == NULL);
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
static int
send_buffer_(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    assert(!list_uncomplete(&s->low));
    // step 1
    int ret = send_list(ss, s, &s->high, l, result);
    if (ret != -1) {
        if (ret == SOCKET_ERR) {
            // HALFCLOSE_WRITE
            return SOCKET_ERR;
        }
        // SOCKET_RST (ignore)
        return -1;
    }
    if (s->high.head == NULL) {
        // step 2
        if (s->low.head != NULL) {
            int ret = send_list(ss, s, &s->low, l, result);
            if (ret != -1) {
                if (ret == SOCKET_ERR) {
                    // HALFCLOSE_WRITE
                    return SOCKET_ERR;
                }
                // SOCKET_RST (ignore)
                return -1;
            }
            // step 3
            if (list_uncomplete(&s->low)) { //如果低优先级队列某个节点只写入了一部分数据
                raise_uncomplete(s);        //将低优先级队列该节点移到高优先级
                return -1;
            }
            if (s->low.head)
                return -1;
        }
        // step 4
        assert(send_buffer_empty(s) && s->wb_size == 0);

        if (s->closing) {
            // finish writing
            force_close(ss, s, l, result);
            return -1;
        }

        int err = enable_write(ss, s, false); //移除EPOLLOUT监听

        if (err) {
            return report_error(s, result, "disable write failed");
        }

        if (s->warn_size > 0) {
            s->warn_size = 0;
            result->opaque = s->opaque;
            result->id = s->id;
            result->ud = 0;
            result->data = NULL;
            return SOCKET_WARNING;
        }
    }

    return -1;
}

static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    if (!socket_trylock(l))
        return -1;      // blocked by direct write, send later.
    if (s->dw_buffer) { //如果工作线程写了一部分的话，就把剩下的加入到高优先级队列中
        // add direct write buffer before high.head
        struct write_buffer *buf = MALLOC(sizeof(*buf));
        struct send_object so;
        buf->userobject = send_object_init(ss, &so, (void *)s->dw_buffer, s->dw_size);
        buf->ptr = (char *)so.buffer + s->dw_offset;
        buf->sz = so.sz - s->dw_offset;
        buf->buffer = (void *)s->dw_buffer;
        s->wb_size += buf->sz;
        if (s->high.head == NULL) {
            s->high.head = s->high.tail = buf;
            buf->next = NULL;
        } else {
            buf->next = s->high.head;
            s->high.head = buf;
        }
        s->dw_buffer = NULL;
    }
    int r = send_buffer_(ss, s, l, result);
    socket_unlock(l);

    return r;
}

static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send *request, int size) {
    struct write_buffer *buf = MALLOC(size);
    struct send_object so;
    buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
    buf->ptr = (char *)so.buffer;
    buf->sz = so.sz;
    buf->buffer = request->buffer;
    buf->next = NULL;
    if (s->head == NULL) {
        s->head = s->tail = buf;
    } else {
        assert(s->tail != NULL);
        assert(s->tail->next == NULL);
        s->tail->next = buf;
        s->tail = buf;
    }
    return buf;
}

static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send *request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
    struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;
    struct write_buffer_udp *buf = (struct write_buffer_udp *)append_sendbuffer_(ss, wl, request, sizeof(*buf));
    memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);
    s->wb_size += buf->buffer.sz;
}

static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send *request) {
    struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, sizeof(*buf));
    s->wb_size += buf->sz;
}

static inline void
append_sendbuffer_low(struct socket_server *ss, struct socket *s, struct request_send *request) {
    struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, sizeof(*buf));
    s->wb_size += buf->sz;
}

static int
trigger_write(struct socket_server *ss, struct request_send *request, struct socket_message *result) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id))
        return -1;
    if (enable_write(ss, s, true)) {
        return report_error(s, result, "enable write failed");
    }
    return -1;
}

/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */
static int
send_socket(struct socket_server *ss, struct request_send *request, struct socket_message *result, int priority, const uint8_t *udp_address) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    struct send_object so;
    send_object_init(ss, &so, request->buffer, request->sz);
    uint8_t type = ATOM_LOAD(&s->type);
    if (type == SOCKET_TYPE_INVALID || s->id != id || type == SOCKET_TYPE_HALFCLOSE_WRITE || type == SOCKET_TYPE_PACCEPT || s->closing) {
        so.free_func((void *)request->buffer);
        return -1;
    }
    if (type == SOCKET_TYPE_PLISTEN || type == SOCKET_TYPE_LISTEN) {
        skynet_error(NULL, "socket-server: write to listen fd %d.", id);
        so.free_func((void *)request->buffer);
        return -1;
    }
    //如果队列为空的话，直接写入高优先级队列，并触发写事件，否则写入对应的优先级队列，不触发写事件
    if (send_buffer_empty(s)) {
        if (s->protocol == PROTOCOL_TCP) {
            append_sendbuffer(ss, s, request); // add to high priority list, even priority == PRIORITY_LOW
        } else {
            // udp
            if (udp_address == NULL) {
                udp_address = s->p.udp_address;
            }
            union sockaddr_all sa;
            socklen_t sasz = udp_socket_address(s, udp_address, &sa);
            if (sasz == 0) {
                // udp type mismatch, just drop it.
                skynet_error(NULL, "socket-server: udp socket (%d) type mismatch.", id);
                so.free_func((void *)request->buffer);
                return -1;
            }
            int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
            if (n != so.sz) {
                append_sendbuffer_udp(ss, s, priority, request, udp_address);
            } else {
                stat_write(ss, s, n);
                so.free_func((void *)request->buffer);
                return -1;
            }
        }
        if (enable_write(ss, s, true)) {
            return report_error(s, result, "enable write failed");
        }
    } else {
        if (s->protocol == PROTOCOL_TCP) {
            if (priority == PRIORITY_LOW) {
                append_sendbuffer_low(ss, s, request);
            } else {
                append_sendbuffer(ss, s, request);
            }
        } else {
            if (udp_address == NULL) {
                udp_address = s->p.udp_address;
            }
            append_sendbuffer_udp(ss, s, priority, request, udp_address);
        }
    }
    if (s->wb_size >= WARNING_SIZE && s->wb_size >= s->warn_size) {
        s->warn_size = s->warn_size == 0 ? WARNING_SIZE * 2 : s->warn_size * 2;
        result->opaque = s->opaque;
        result->id = s->id;
        result->ud = s->wb_size % 1024 == 0 ? s->wb_size / 1024 : s->wb_size / 1024 + 1;
        result->data = NULL;
        return SOCKET_WARNING;
    }
    return -1;
}

static int listen_socket(struct socket_server *ss, struct request_listen *request, struct socket_message *result) {
    int id = request->id;
    int listen_fd = request->fd;
    struct socket *s = new_fd(ss, id, listen_fd, PROTOCOL_TCP, request->opaque, false);
    if (s == NULL) {
        goto _failed;
    }
    ATOM_STORE(&s->type, SOCKET_TYPE_PLISTEN); //上面new_fd传入的最后一个参数为false，所以这时还不会监听连接事件，所以是一个中间状态SOCKET_TYPE_PLISTEN,当gate服务调用socketdriver.start时才会开始监听，这样做的目的是为了让服务能够完成初始化或者是必要的准备工作以后，再接收来自外部的网络请求。
    result->opaque = request->opaque;
    result->id = id;
    result->ud = 0;
    result->data = "listen";

    union sockaddr_all u;
    socklen_t slen = sizeof(u);
    if (getsockname(listen_fd, &u.s, &slen) == 0) {
        void *sin_addr = (u.s.sa_family == AF_INET) ? (void *)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
        if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer)) == 0) {
            result->data = strerror(errno);
            return SOCKET_ERR;
        }
        int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
        result->data = ss->buffer;
        result->ud = sin_port;
    } else {
        result->data = strerror(errno);
        return SOCKET_ERR;
    }

    return SOCKET_OPEN;
_failed:
    close(listen_fd);
    result->opaque = request->opaque;
    result->id = id;
    result->ud = 0;
    result->data = "reach skynet socket number limit";
    ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

    return SOCKET_ERR;
}

static inline int
nomore_sending_data(struct socket *s) {
    return (send_buffer_empty(s) && s->dw_buffer == NULL && (ATOM_LOAD(&s->sending) & 0xffff) == 0) || (ATOM_LOAD(&s->type) == SOCKET_TYPE_HALFCLOSE_WRITE);
}

static void
close_read(struct socket_server *ss, struct socket *s, struct socket_message *result) {
    // Don't read socket later
    ATOM_STORE(&s->type, SOCKET_TYPE_HALFCLOSE_READ);
    enable_read(ss, s, false);
    shutdown(s->fd, SHUT_RD);
    result->id = s->id;
    result->ud = 0;
    result->data = NULL;
    result->opaque = s->opaque;
}

static inline int
halfclose_read(struct socket *s) {
    return ATOM_LOAD(&s->type) == SOCKET_TYPE_HALFCLOSE_READ;
}

// SOCKET_CLOSE can be raised (only once) in one of two conditions.
// See https://github.com/cloudwu/skynet/issues/1346 for more discussion.
// 1. close socket by self, See close_socket()
// 2. recv 0 or eof event (close socket by remote), See forward_message_tcp()
// It's able to write data after SOCKET_CLOSE (In condition 2), but if remote is closed, SOCKET_ERR may raised.
static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        // The socket is closed, ignore
        return -1;
    }
    struct socket_lock l;
    socket_lock_init(s, &l);

    int shutdown_read = halfclose_read(s);

    if (request->shutdown || nomore_sending_data(s)) { //如果强制关闭或者发送缓存为空，则直接关闭
        // If socket is SOCKET_TYPE_HALFCLOSE_READ, Do not raise SOCKET_CLOSE again.
        int r = shutdown_read ? -1 : SOCKET_CLOSE;
        force_close(ss, s, &l, result);
        return r;
    }
    //否则设置个标记位，后面先让数据都发送出去
    s->closing = true;
    if (!shutdown_read) {
        // don't read socket after socket.close()
        close_read(ss, s, result);
        return SOCKET_CLOSE;
    }
    // recv 0 before (socket is SOCKET_TYPE_HALFCLOSE_READ) and waiting for sending data out.
    return -1;
}

static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
    int id = request->id;
    result->id = id;
    result->opaque = request->opaque;
    result->ud = 0;
    struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
    if (s == NULL) {
        result->data = "reach skynet socket number limit";
        return SOCKET_ERR;
    }
    sp_nonblocking(request->fd);
    ATOM_STORE(&s->type, SOCKET_TYPE_BIND);
    result->data = "binding";
    return SOCKET_OPEN;
}

static int
resume_socket(struct socket_server *ss, struct request_resumepause *request, struct socket_message *result) {
    int id = request->id;
    result->id = id;
    result->opaque = request->opaque;
    result->ud = 0;
    result->data = NULL;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        result->data = "invalid socket";
        return SOCKET_ERR;
    }
    if (halfclose_read(s)) {
        // The closing socket may be in transit, so raise an error. See https://github.com/cloudwu/skynet/issues/1374
        result->data = "socket closed";
        return SOCKET_ERR;
    }
    struct socket_lock l;
    socket_lock_init(s, &l);
    if (enable_read(ss, s, true)) {
        result->data = "enable read failed";
        return SOCKET_ERR;
    }
    uint8_t type = ATOM_LOAD(&s->type);
    if (type == SOCKET_TYPE_PACCEPT || type == SOCKET_TYPE_PLISTEN) {
        ATOM_STORE(&s->type, (type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN);
        s->opaque = request->opaque;
        result->data = "start";
        return SOCKET_OPEN;
    } else if (type == SOCKET_TYPE_CONNECTED) {
        // todo: maybe we should send a message SOCKET_TRANSFER to s->opaque
        s->opaque = request->opaque;
        result->data = "transfer";
        return SOCKET_OPEN;
    }
    // if s->type == SOCKET_TYPE_HALFCLOSE_WRITE , SOCKET_CLOSE message will send later
    return -1;
}

static int
pause_socket(struct socket_server *ss, struct request_resumepause *request, struct socket_message *result) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        return -1;
    }
    if (enable_read(ss, s, false)) {
        return report_error(s, result, "enable read failed");
    }
    return -1;
}

static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        return;
    }
    int v = request->value;
    setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
}

static void block_readpipe(int pipefd, void *buffer, int sz) {
    for (;;) {
        int n = read(pipefd, buffer, sz);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            skynet_error(NULL, "socket-server : read pipe error %s.", strerror(errno));
            return;
        }
        // must atomic read from a pipe
        assert(n == sz);
        return;
    }
}

static int
has_cmd(struct socket_server *ss) {
    struct timeval tv = {0, 0};
    int retval;

    FD_SET(ss->recvctrl_fd, &ss->rfds);

    retval = select(ss->recvctrl_fd + 1, &ss->rfds, NULL, NULL, &tv);
    if (retval == 1) {
        return 1;
    }
    return 0;
}

static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
    int id = udp->id;
    int protocol;
    if (udp->family == AF_INET6) {
        protocol = PROTOCOL_UDPv6;
    } else {
        protocol = PROTOCOL_UDP;
    }
    struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
    if (ns == NULL) {
        close(udp->fd);
        ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
        return;
    }
    ATOM_STORE(&ns->type, SOCKET_TYPE_CONNECTED);
    memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}

static int
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        return -1;
    }
    int type = request->address[0];
    if (type != s->protocol) {
        // protocol mismatch
        return report_error(s, result, "protocol mismatch");
    }
    if (type == PROTOCOL_UDP) {
        memcpy(s->p.udp_address, request->address, 1 + 2 + 4); // 1 type, 2 port, 4 ipv4
    } else {
        memcpy(s->p.udp_address, request->address, 1 + 2 + 16); // 1 type, 2 port, 16 ipv6
    }
    ATOM_FDEC(&s->udpconnecting);
    return -1;
}

static inline void
inc_sending_ref(struct socket *s, int id) {
    if (s->protocol != PROTOCOL_TCP)
        return;
    for (;;) {
        unsigned long sending = ATOM_LOAD(&s->sending);
        if ((sending >> 16) == ID_TAG16(id)) {
            if ((sending & 0xffff) == 0xffff) {
                // s->sending may overflow (rarely), so busy waiting here for socket thread dec it. see issue #794
                continue;
            }
            // inc sending only matching the same socket id
            if (ATOM_CAS_ULONG(&s->sending, sending, sending + 1))
                return;
            // atom inc failed, retry
        } else {
            // socket id changed, just return
            return;
        }
    }
}

static inline void
dec_sending_ref(struct socket_server *ss, int id) {
    struct socket *s = &ss->slot[HASH_ID(id)];
    // Notice: udp may inc sending while type == SOCKET_TYPE_RESERVE
    if (s->id == id && s->protocol == PROTOCOL_TCP) {
        assert((ATOM_LOAD(&s->sending) & 0xffff) != 0);
        ATOM_FDEC(&s->sending);
    }
}

// return type
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
    int fd = ss->recvctrl_fd;
    // the length of message is one byte, so 256 buffer size is enough.
    uint8_t buffer[256];
    uint8_t header[2];
    block_readpipe(fd, header, sizeof(header));
    int type = header[0];
    int len = header[1];
    block_readpipe(fd, buffer, len);
    // ctrl command only exist in local fd, so don't worry about endian.
    switch (type) {
    case 'R':
        return resume_socket(ss, (struct request_resumepause *)buffer, result);
    case 'S':
        return pause_socket(ss, (struct request_resumepause *)buffer, result);
    case 'B':
        return bind_socket(ss, (struct request_bind *)buffer, result);
    case 'L':
        return listen_socket(ss, (struct request_listen *)buffer, result);
    case 'K':
        return close_socket(ss, (struct request_close *)buffer, result);
    case 'O':
        return open_socket(ss, (struct request_open *)buffer, result);
    case 'X': //Exit
        result->opaque = 0;
        result->id = 0;
        result->ud = 0;
        result->data = NULL;
        return SOCKET_EXIT;
    case 'W':
        return trigger_write(ss, (struct request_send *)buffer, result); //直接触发写事件,不往写入队列里移了
    case 'D':                                                            //Send package (high)
    case 'P': {                                                          //Send package (low)
        int priority = (type == 'D') ? PRIORITY_HIGH : PRIORITY_LOW;
        struct request_send *request = (struct request_send *)buffer;
        int ret = send_socket(ss, request, result, priority, NULL); //加入到对应的写入队列
        dec_sending_ref(ss, request->id);
        return ret;
    }
    case 'A': { //Send UDP package
        struct request_send_udp *rsu = (struct request_send_udp *)buffer;
        return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
    }
    case 'C':
        return set_udp_address(ss, (struct request_setudp *)buffer, result);
    case 'T':
        setopt_socket(ss, (struct request_setopt *)buffer);
        return -1;
    case 'U':
        add_udp_socket(ss, (struct request_udp *)buffer);
        return -1;
    default:
        skynet_error(NULL, "socket-server: Unknown ctrl %c.", type);
        return -1;
    };

    return -1;
}

// return -1 (ignore) when error
static int forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    int sz = s->p.size;
    char *buffer = MALLOC(sz);
    int n = (int)read(s->fd, buffer, sz);
    if (n < 0) {
        FREE(buffer);
        switch (errno) {
        case EINTR:
        case AGAIN_WOULDBLOCK:
            break;
        default:
            return report_error(s, result, strerror(errno));
        }
        return -1;
    }
    if (n == 0) {
        FREE(buffer);
        if (s->closing) {
            // Rare case : if s->closing is true, reading event is disable, and SOCKET_CLOSE is raised.
            if (nomore_sending_data(s)) {
                force_close(ss, s, l, result);
            }
            return -1;
        }
        int t = ATOM_LOAD(&s->type);
        if (t == SOCKET_TYPE_HALFCLOSE_READ) {
            // Rare case : Already shutdown read.
            return -1;
        }
        if (t == SOCKET_TYPE_HALFCLOSE_WRITE) {
            // Remote shutdown read (write error) before.
            force_close(ss, s, l, result);
        } else {
            close_read(ss, s, result);
        }
        return SOCKET_CLOSE;
    }

    if (halfclose_read(s)) {
        // discard recv data (Rare case : if socket is HALFCLOSE_READ, reading event is disable.)
        FREE(buffer);
        return -1;
    }

    stat_read(ss, s, n);

    result->opaque = s->opaque;
    result->id = s->id;
    result->ud = n;
    result->data = buffer;

    //关于 TCP 连接每次读取数据的容量(即socket结构体的p.size字段)大小有以下规则：每个 socket 的初始的读取容量为 64 字节，在接下来的读取中，若读取到的数据大小等于读取容量，则将读取容量扩大一倍；若读取到的数据大小小于容量的一半且容量大小大于初始容量，则将容量缩小一倍。
    if (n == sz) {
        s->p.size *= 2;
        return SOCKET_MORE;
    } else if (sz > MIN_READ_BUFFER && n * 2 < sz) {
        s->p.size /= 2;
    }

    return SOCKET_DATA;
}

static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t *udp_address) {
    int addrsz = 1;
    udp_address[0] = (uint8_t)protocol;
    if (protocol == PROTOCOL_UDP) {
        memcpy(udp_address + addrsz, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
        addrsz += sizeof(sa->v4.sin_port);
        memcpy(udp_address + addrsz, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
        addrsz += sizeof(sa->v4.sin_addr);
    } else {
        memcpy(udp_address + addrsz, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
        addrsz += sizeof(sa->v6.sin6_port);
        memcpy(udp_address + addrsz, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
        addrsz += sizeof(sa->v6.sin6_addr);
    }
    return addrsz;
}

static int
forward_message_udp(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    union sockaddr_all sa;
    socklen_t slen = sizeof(sa);
    int n = recvfrom(s->fd, ss->udpbuffer, MAX_UDP_PACKAGE, 0, &sa.s, &slen);
    if (n < 0) {
        switch (errno) {
        case EINTR:
        case AGAIN_WOULDBLOCK:
            return -1;
        }
        int error = errno;
        // close when error
        force_close(ss, s, l, result);
        result->data = strerror(error);
        return SOCKET_ERR;
    }
    stat_read(ss, s, n);

    uint8_t *data;
    if (slen == sizeof(sa.v4)) {
        if (s->protocol != PROTOCOL_UDP)
            return -1;
        data = MALLOC(n + 1 + 2 + 4);
        gen_udp_address(PROTOCOL_UDP, &sa, data + n);
    } else {
        if (s->protocol != PROTOCOL_UDPv6)
            return -1;
        data = MALLOC(n + 1 + 2 + 16);
        gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
    }
    memcpy(data, ss->udpbuffer, n);

    result->opaque = s->opaque;
    result->id = s->id;
    result->ud = n;
    result->data = (char *)data;

    return SOCKET_UDP;
}

static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    int error;
    socklen_t len = sizeof(error);
    int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);
    if (code < 0 || error) {
        error = code < 0 ? errno : error;
        force_close(ss, s, l, result);
        result->data = strerror(error);
        return SOCKET_ERR;
    } else {
        ATOM_STORE(&s->type, SOCKET_TYPE_CONNECTED);
        result->opaque = s->opaque;
        result->id = s->id;
        result->ud = 0;
        if (nomore_sending_data(s)) {
            if (enable_write(ss, s, false)) {
                force_close(ss, s, l, result);
                result->data = "disable write failed";
                return SOCKET_ERR;
            }
        }
        union sockaddr_all u;
        socklen_t slen = sizeof(u);
        if (getpeername(s->fd, &u.s, &slen) == 0) {
            void *sin_addr = (u.s.sa_family == AF_INET) ? (void *)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
            if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
                result->data = ss->buffer;
                return SOCKET_OPEN;
            }
        }
        result->data = NULL;
        return SOCKET_OPEN;
    }
}

static int
getname(union sockaddr_all *u, char *buffer, size_t sz) {
    char tmp[INET6_ADDRSTRLEN];
    void *sin_addr = (u->s.sa_family == AF_INET) ? (void *)&u->v4.sin_addr : (void *)&u->v6.sin6_addr;
    if (inet_ntop(u->s.sa_family, sin_addr, tmp, sizeof(tmp))) {
        int sin_port = ntohs((u->s.sa_family == AF_INET) ? u->v4.sin_port : u->v6.sin6_port);
        snprintf(buffer, sz, "%s:%d", tmp, sin_port);
        return 1;
    } else {
        buffer[0] = '\0';
        return 0;
    }
}

// return 0 when failed, or -1 when file limit
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
    union sockaddr_all u;
    socklen_t len = sizeof(u);
    int client_fd = accept(s->fd, &u.s, &len);
    if (client_fd < 0) {
        if (errno == EMFILE || errno == ENFILE) {
            result->opaque = s->opaque;
            result->id = s->id;
            result->ud = 0;
            result->data = strerror(errno);

            // See https://stackoverflow.com/questions/47179793/how-to-gracefully-handle-accept-giving-emfile-and-close-the-connection
            if (ss->reserve_fd >= 0) {
                close(ss->reserve_fd);
                client_fd = accept(s->fd, &u.s, &len);
                if (client_fd >= 0) {
                    close(client_fd);
                }
                ss->reserve_fd = dup(1);
            }
            return -1;
        } else {
            return 0;
        }
    }
    int id = reserve_id(ss);
    if (id < 0) {
        close(client_fd);
        return 0;
    }
    socket_keepalive(client_fd);
    sp_nonblocking(client_fd);
    struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
    if (ns == NULL) {
        close(client_fd);
        return 0;
    }
    // accept new one connection
    stat_read(ss, s, 1);

    ATOM_STORE(&ns->type, SOCKET_TYPE_PACCEPT);
    result->opaque = s->opaque;
    result->id = s->id;
    result->ud = id;
    result->data = NULL;

    if (getname(&u, ss->buffer, sizeof(ss->buffer))) {
        result->data = ss->buffer;
    }

    return 1;
}

static inline void
clear_closed_event(struct socket_server *ss, struct socket_message *result, int type) {
    if (type == SOCKET_CLOSE || type == SOCKET_ERR) {
        int id = result->id;
        int i;
        for (i = ss->event_index; i < ss->event_n; i++) {
            struct event *e = &ss->ev[i];
            struct socket *s = e->s;
            if (s) {
                if (socket_invalid(s, id) && s->id == id) {
                    e->s = NULL;
                    break;
                }
            }
        }
    }
}

// return type
int socket_server_poll(struct socket_server *ss, struct socket_message *result, int *more) {
    for (;;) {
        if (ss->checkctrl) {
            if (has_cmd(ss)) {                   //检查管道里面是否有命令(select检查)
                int type = ctrl_cmd(ss, result); //读取并处理管道里面命令
                if (type != -1) {
                    clear_closed_event(ss, result, type);
                    return type;
                } else
                    continue;
            } else {
                ss->checkctrl = 0;
            }
        }
        if (ss->event_index == ss->event_n) {
            // 事件wait
            ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
            ss->checkctrl = 1; //首先会把检查控制命令的变量 checkctrl 置为 1
            if (more) {
                *more = 0;
            }
            ss->event_index = 0;
            if (ss->event_n <= 0) {
                ss->event_n = 0;
                int err = errno;
                if (err != EINTR) {
                    skynet_error(NULL, "socket-server: %s", strerror(err));
                }
                continue;
            }
        }
        struct event *e = &ss->ev[ss->event_index++];
        struct socket *s = e->s;
        if (s == NULL) { //如果碰到的是命令事件，则直接 continue 回到循环的最上面去处理
            // dispatch pipe message at beginning
            continue;
        }
        struct socket_lock l;
        socket_lock_init(s, &l);
        // 事件处理
        switch (ATOM_LOAD(&s->type)) {
        case SOCKET_TYPE_CONNECTING:
            // 主动连接
            return report_connect(ss, s, &l, result);
        case SOCKET_TYPE_LISTEN: { //如果触发网络事件的套接字是 SOCKET_TYPE_LISTEN 状态的话，则说明触发了 accept 事件
            // 有客户端来连接
            int ok = report_accept(ss, s, result);
            if (ok > 0) {
                return SOCKET_ACCEPT;
            }
            if (ok < 0) {
                return SOCKET_ERR;
            }
            // when ok == 0, retry
            break;
        }
        case SOCKET_TYPE_INVALID:
            skynet_error(NULL, "socket-server: invalid socket");
            break;
        default:
            if (e->read) { // 读取处理，socket线程不会对读出来的数据包作任何处理，它的做法是，读取之后立刻转给gate服务。
                int type;
                if (s->protocol == PROTOCOL_TCP) {
                    type = forward_message_tcp(ss, s, &l, result);
                    if (type == SOCKET_MORE) { //如果本次没有读取完套接字中的数据，则会减少 event_index，使下一次循环依然处理本事件
                        --ss->event_index;
                        return SOCKET_DATA;
                    }
                } else {
                    type = forward_message_udp(ss, s, &l, result);
                    if (type == SOCKET_UDP) {
                        // try read again
                        --ss->event_index;
                        return SOCKET_UDP;
                    }
                }
                if (e->write && type != SOCKET_CLOSE && type != SOCKET_ERR) {
                    // Try to dispatch write message next step if write flag set.
                    e->read = false;
                    --ss->event_index;
                }
                if (type == -1)
                    break;
                return type;
            }
            if (e->write) {
                int type = send_buffer(ss, s, &l, result);
                if (type == -1)
                    break;
                return type;
            }
            if (e->error) {
                int error;
                socklen_t len = sizeof(error);
                int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);
                const char *err = NULL;
                if (code < 0) {
                    err = strerror(errno);
                } else if (error != 0) {
                    err = strerror(error);
                } else {
                    err = "Unknown error";
                }
                return report_error(s, result, err);
            }
            if (e->eof) {
                // For epoll (at least), FIN packets are exchanged both ways.
                // See: https://stackoverflow.com/questions/52976152/tcp-when-is-epollhup-generated
                int halfclose = halfclose_read(s);
                force_close(ss, s, &l, result);
                if (!halfclose) {
                    return SOCKET_CLOSE;
                }
            }
            break;
        }
    }
}

static void send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
    request->header[6] = (uint8_t)type;
    request->header[7] = (uint8_t)len; //小于256字节，可以保证原子性
    const char *req = (const char *)request + offsetof(struct request_package, header[6]);
    for (;;) {
        ssize_t n = write(ss->sendctrl_fd, req, len + 2);
        if (n < 0) {
            if (errno != EINTR) {
                skynet_error(NULL, "socket-server : send ctrl command error %s.", strerror(errno));
            }
            continue;
        }
        assert(n == len + 2);
        return;
    }
}

static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
    int len = strlen(addr);
    if (len + sizeof(req->u.open) >= 256) {
        skynet_error(NULL, "socket-server : Invalid addr %s.", addr);
        return -1;
    }
    int id = reserve_id(ss);
    if (id < 0)
        return -1;
    req->u.open.opaque = opaque;
    req->u.open.id = id;
    req->u.open.port = port;
    memcpy(req->u.open.host, addr, len);
    req->u.open.host[len] = '\0';

    return len;
}

int socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char *addr, int port) {
    struct request_package request;
    int len = open_request(ss, &request, opaque, addr, port);
    if (len < 0)
        return -1;
    send_request(ss, &request, 'O', sizeof(request.u.open) + len);
    return request.u.open.id;
}

static inline int
can_direct_write(struct socket *s, int id) {
    return s->id == id && nomore_sending_data(s) && ATOM_LOAD(&s->type) == SOCKET_TYPE_CONNECTED && ATOM_LOAD(&s->udpconnecting) == 0;
}

// return -1 when error, 0 when success
int socket_server_send(struct socket_server *ss, struct socket_sendbuffer *buf) {
    int id = buf->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id) || s->closing) {
        free_buffer(ss, buf);
        return -1;
    }

    struct socket_lock l;
    socket_lock_init(s, &l);

    if (can_direct_write(s, id) && socket_trylock(&l)) {
        // may be we can send directly, double check
        if (can_direct_write(s, id)) {
            // send directly
            struct send_object so;
            send_object_init_from_sendbuffer(ss, &so, buf);
            ssize_t n;
            if (s->protocol == PROTOCOL_TCP) {
                n = write(s->fd, so.buffer, so.sz); //直接在worker线程内发送数据包
            } else {
                union sockaddr_all sa;
                socklen_t sasz = udp_socket_address(s, s->p.udp_address, &sa);
                if (sasz == 0) {
                    skynet_error(NULL, "socket-server : set udp (%d) address first.", id);
                    socket_unlock(&l);
                    so.free_func((void *)buf->buffer);
                    return -1;
                }
                n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
            }
            if (n < 0) {
                // ignore error, let socket thread try again
                n = 0;
            }
            stat_write(ss, s, n);
            if (n == so.sz) {
                // write done
                socket_unlock(&l);
                so.free_func((void *)buf->buffer);
                return 0;
            }
            // write failed, put buffer into s->dw_* , and let socket thread send it. see send_buffer()
            s->dw_buffer = clone_buffer(buf, &s->dw_size);
            s->dw_offset = n;

            socket_unlock(&l);

            struct request_package request;
            request.u.send.id = id;
            request.u.send.sz = 0;
            request.u.send.buffer = NULL;

            // let socket thread enable write event
            send_request(ss, &request, 'W', sizeof(request.u.send));

            return 0;
        }
        socket_unlock(&l);
    }

    inc_sending_ref(s, id);

    struct request_package request;
    request.u.send.id = id;
    request.u.send.buffer = clone_buffer(buf, &request.u.send.sz);

    send_request(ss, &request, 'D', sizeof(request.u.send));
    return 0;
}

// return -1 when error, 0 when success
int socket_server_send_lowpriority(struct socket_server *ss, struct socket_sendbuffer *buf) {
    int id = buf->id;

    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        free_buffer(ss, buf);
        return -1;
    }

    inc_sending_ref(s, id);

    struct request_package request;
    request.u.send.id = id;
    request.u.send.buffer = clone_buffer(buf, &request.u.send.sz);

    send_request(ss, &request, 'P', sizeof(request.u.send));
    return 0;
}

void socket_server_exit(struct socket_server *ss) {
    struct request_package request;
    send_request(ss, &request, 'X', 0);
}

void socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
    struct request_package request;
    request.u.close.id = id;
    request.u.close.shutdown = 0;
    request.u.close.opaque = opaque;
    send_request(ss, &request, 'K', sizeof(request.u.close));
}

void socket_server_shutdown(struct socket_server *ss, uintptr_t opaque, int id) {
    struct request_package request;
    request.u.close.id = id;
    request.u.close.shutdown = 1;
    request.u.close.opaque = opaque;
    send_request(ss, &request, 'K', sizeof(request.u.close));
}

// return -1 means failed
// or return AF_INET or AF_INET6
static int do_bind(const char *host, int port, int protocol, int *family) {
    int fd;
    int status;
    int reuse = 1;
    struct addrinfo ai_hints;
    struct addrinfo *ai_list = NULL;
    char portstr[16];
    if (host == NULL || host[0] == 0) {
        host = "0.0.0.0"; // INADDR_ANY
    }
    sprintf(portstr, "%d", port);
    memset(&ai_hints, 0, sizeof(ai_hints));
    ai_hints.ai_family = AF_UNSPEC;
    if (protocol == IPPROTO_TCP) {
        ai_hints.ai_socktype = SOCK_STREAM;
    } else {
        assert(protocol == IPPROTO_UDP);
        ai_hints.ai_socktype = SOCK_DGRAM;
    }
    ai_hints.ai_protocol = protocol;

    status = getaddrinfo(host, portstr, &ai_hints, &ai_list);
    if (status != 0) {
        return -1;
    }
    *family = ai_list->ai_family;
    fd = socket(*family, ai_list->ai_socktype, 0);
    if (fd < 0) {
        goto _failed_fd;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int)) == -1) {
        goto _failed;
    }
    status = bind(fd, (struct sockaddr *)ai_list->ai_addr, ai_list->ai_addrlen);
    if (status != 0)
        goto _failed;

    freeaddrinfo(ai_list);
    return fd;
_failed:
    close(fd);
_failed_fd:
    freeaddrinfo(ai_list);
    return -1;
}

static int do_listen(const char *host, int port, int backlog) {
    int family = 0;
    int listen_fd = do_bind(host, port, IPPROTO_TCP, &family);
    if (listen_fd < 0) {
        return -1;
    }
    if (listen(listen_fd, backlog) == -1) {
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

int socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char *addr, int port, int backlog) {
    int fd = do_listen(addr, port, backlog);
    if (fd < 0) {
        return -1;
    }
    struct request_package request;
    int id = reserve_id(ss); //在slot中为其预留了一个socket实例
    if (id < 0) {
        close(fd);
        return id;
    }
    request.u.listen.opaque = opaque;
    request.u.listen.id = id;
    request.u.listen.fd = fd;
    send_request(ss, &request, 'L', sizeof(request.u.listen));
    return id;
}

int socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
    struct request_package request;
    int id = reserve_id(ss);
    if (id < 0)
        return -1;
    request.u.bind.opaque = opaque;
    request.u.bind.id = id;
    request.u.bind.fd = fd;
    send_request(ss, &request, 'B', sizeof(request.u.bind));
    return id;
}

void socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
    struct request_package request;
    request.u.resumepause.id = id;
    request.u.resumepause.opaque = opaque;
    send_request(ss, &request, 'R', sizeof(request.u.resumepause));
}

void socket_server_pause(struct socket_server *ss, uintptr_t opaque, int id) {
    struct request_package request;
    request.u.resumepause.id = id;
    request.u.resumepause.opaque = opaque;
    send_request(ss, &request, 'S', sizeof(request.u.resumepause));
}

void socket_server_nodelay(struct socket_server *ss, int id) {
    struct request_package request;
    request.u.setopt.id = id;
    request.u.setopt.what = TCP_NODELAY;
    request.u.setopt.value = 1;
    send_request(ss, &request, 'T', sizeof(request.u.setopt));
}

void socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
    ss->soi = *soi;
}

// UDP

int socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char *addr, int port) {
    int fd;
    int family;
    if (port != 0 || addr != NULL) {
        // bind
        fd = do_bind(addr, port, IPPROTO_UDP, &family);
        if (fd < 0) {
            return -1;
        }
    } else {
        family = AF_INET;
        fd = socket(family, SOCK_DGRAM, 0);
        if (fd < 0) {
            return -1;
        }
    }
    sp_nonblocking(fd);

    int id = reserve_id(ss);
    if (id < 0) {
        close(fd);
        return -1;
    }
    struct request_package request;
    request.u.udp.id = id;
    request.u.udp.fd = fd;
    request.u.udp.opaque = opaque;
    request.u.udp.family = family;

    send_request(ss, &request, 'U', sizeof(request.u.udp));
    return id;
}

int socket_server_udp_send(struct socket_server *ss, const struct socket_udp_address *addr, struct socket_sendbuffer *buf) {
    int id = buf->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        free_buffer(ss, buf);
        return -1;
    }

    const uint8_t *udp_address = (const uint8_t *)addr;
    int addrsz;
    switch (udp_address[0]) {
    case PROTOCOL_UDP:
        addrsz = 1 + 2 + 4; // 1 type, 2 port, 4 ipv4
        break;
    case PROTOCOL_UDPv6:
        addrsz = 1 + 2 + 16; // 1 type, 2 port, 16 ipv6
        break;
    default:
        free_buffer(ss, buf);
        return -1;
    }

    struct socket_lock l;
    socket_lock_init(s, &l);

    if (can_direct_write(s, id) && socket_trylock(&l)) {
        // may be we can send directly, double check
        if (can_direct_write(s, id)) {
            // send directly
            struct send_object so;
            send_object_init_from_sendbuffer(ss, &so, buf);
            union sockaddr_all sa;
            socklen_t sasz = udp_socket_address(s, udp_address, &sa);
            if (sasz == 0) {
                socket_unlock(&l);
                so.free_func((void *)buf->buffer);
                return -1;
            }
            int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
            if (n >= 0) {
                // sendto succ
                stat_write(ss, s, n);
                socket_unlock(&l);
                so.free_func((void *)buf->buffer);
                return 0;
            }
        }
        socket_unlock(&l);
        // let socket thread try again, udp doesn't care the order
    }

    struct request_package request;
    request.u.send_udp.send.id = id;
    request.u.send_udp.send.buffer = clone_buffer(buf, &request.u.send_udp.send.sz);

    memcpy(request.u.send_udp.address, udp_address, addrsz);

    send_request(ss, &request, 'A', sizeof(request.u.send_udp.send) + addrsz);
    return 0;
}

int socket_server_udp_connect(struct socket_server *ss, int id, const char *addr, int port) {
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        return -1;
    }
    struct socket_lock l;
    socket_lock_init(s, &l);
    socket_lock(&l);
    if (socket_invalid(s, id)) {
        socket_unlock(&l);
        return -1;
    }
    ATOM_FINC(&s->udpconnecting);
    socket_unlock(&l);

    int status;
    struct addrinfo ai_hints;
    struct addrinfo *ai_list = NULL;
    char portstr[16];
    sprintf(portstr, "%d", port);
    memset(&ai_hints, 0, sizeof(ai_hints));
    ai_hints.ai_family = AF_UNSPEC;
    ai_hints.ai_socktype = SOCK_DGRAM;
    ai_hints.ai_protocol = IPPROTO_UDP;

    status = getaddrinfo(addr, portstr, &ai_hints, &ai_list);
    if (status != 0) {
        return -1;
    }
    struct request_package request;
    request.u.set_udp.id = id;
    int protocol;

    if (ai_list->ai_family == AF_INET) {
        protocol = PROTOCOL_UDP;
    } else if (ai_list->ai_family == AF_INET6) {
        protocol = PROTOCOL_UDPv6;
    } else {
        freeaddrinfo(ai_list);
        return -1;
    }

    int addrsz = gen_udp_address(protocol, (union sockaddr_all *)ai_list->ai_addr, request.u.set_udp.address);

    freeaddrinfo(ai_list);

    send_request(ss, &request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) + addrsz);

    return 0;
}

const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
    uint8_t *address = (uint8_t *)(msg->data + msg->ud);
    int type = address[0];
    switch (type) {
    case PROTOCOL_UDP:
        *addrsz = 1 + 2 + 4;
        break;
    case PROTOCOL_UDPv6:
        *addrsz = 1 + 2 + 16;
        break;
    default:
        return NULL;
    }
    return (const struct socket_udp_address *)address;
}

struct socket_info *
socket_info_create(struct socket_info *last) {
    struct socket_info *si = skynet_malloc(sizeof(*si));
    memset(si, 0, sizeof(*si));
    si->next = last;
    return si;
}

void socket_info_release(struct socket_info *si) {
    while (si) {
        struct socket_info *temp = si;
        si = si->next;
        skynet_free(temp);
    }
}

static int
query_info(struct socket *s, struct socket_info *si) {
    union sockaddr_all u;
    socklen_t slen = sizeof(u);
    int closing = 0;
    switch (ATOM_LOAD(&s->type)) {
    case SOCKET_TYPE_BIND:
        si->type = SOCKET_INFO_BIND;
        si->name[0] = '\0';
        break;
    case SOCKET_TYPE_LISTEN:
        si->type = SOCKET_INFO_LISTEN;
        if (getsockname(s->fd, &u.s, &slen) == 0) {
            getname(&u, si->name, sizeof(si->name));
        }
        break;
    case SOCKET_TYPE_HALFCLOSE_READ:
    case SOCKET_TYPE_HALFCLOSE_WRITE:
        closing = 1;
    case SOCKET_TYPE_CONNECTED:
        if (s->protocol == PROTOCOL_TCP) {
            si->type = closing ? SOCKET_INFO_CLOSING : SOCKET_INFO_TCP;
            if (getpeername(s->fd, &u.s, &slen) == 0) {
                getname(&u, si->name, sizeof(si->name));
            }
        } else {
            si->type = SOCKET_INFO_UDP;
            if (udp_socket_address(s, s->p.udp_address, &u)) {
                getname(&u, si->name, sizeof(si->name));
            }
        }
        break;
    default:
        return 0;
    }
    si->id = s->id;
    si->opaque = (uint64_t)s->opaque;
    si->read = s->stat.read;
    si->write = s->stat.write;
    si->rtime = s->stat.rtime;
    si->wtime = s->stat.wtime;
    si->wbuffer = s->wb_size;
    si->reading = s->reading;
    si->writing = s->writing;

    return 1;
}

struct socket_info *
socket_server_info(struct socket_server *ss) {
    int i;
    struct socket_info *si = NULL;
    for (i = 0; i < MAX_SOCKET; i++) {
        struct socket *s = &ss->slot[i];
        int id = s->id;
        struct socket_info temp;
        if (query_info(s, &temp) && s->id == id) {
            // socket_server_info may call in different thread, so check socket id again
            si = socket_info_create(si);
            temp.next = si->next;
            *si = temp;
        }
    }
    return si;
}
