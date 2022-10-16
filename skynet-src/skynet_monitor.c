#include "skynet.h"

#include "atomic.h"
#include "skynet.h"
#include "skynet_monitor.h"
#include "skynet_server.h"

#include <stdlib.h>
#include <string.h>

//每次当 worker 线程开始处理一条消息的时候，它会修改自己对应的 skynet_monitor 中变量的值。其中 version 会被一直累加，并且将 source 和 destination 设为当前处理的消息的源地址和目标地址
struct skynet_monitor {
    ATOM_INT version;     // 原子 version
    int check_version;    // 上次检查时的 version
    uint32_t source;      // 当前处理的消息的源 handle
    uint32_t destination; // 当前处理的消息的目标 handle
};

struct skynet_monitor *
skynet_monitor_new() {
    struct skynet_monitor *ret = skynet_malloc(sizeof(*ret));
    memset(ret, 0, sizeof(*ret));
    return ret;
}

void skynet_monitor_delete(struct skynet_monitor *sm) {
    skynet_free(sm);
}

void skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
    sm->source = source;
    sm->destination = destination;
    ATOM_FINC(&sm->version);
}

void skynet_monitor_check(struct skynet_monitor *sm) {
    // 如果相等，说明还在处理上次检查的那条消息，给出警告
    if (sm->version == sm->check_version) {
        if (sm->destination) {
            // 设置 endless 标签
            skynet_context_endless(sm->destination);
            skynet_error(NULL, "A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source, sm->destination, sm->version);
        }
    } else { // 如果不相等，保存本次检查的 version
        sm->check_version = sm->version;
    }
}
