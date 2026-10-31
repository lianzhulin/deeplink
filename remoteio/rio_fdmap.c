/*
 * fd 映射管理：假 fd ↔ 远程 handle
 *
 * 策略：
 *   - 假 fd 区间 [RIO_FAKE_FD_BASE, RIO_FAKE_FD_BASE + RIO_MAX_REMOTE_FDS)
 *   - 用数组 + 位 used 管理，O(1) 分配和查找
 *   - 简单起见用数组扫描找第一个空槽，RIO_MAX_REMOTE_FDS=4096 下完全够用
 */

#define _GNU_SOURCE
#include "remoteio.h"
#include <string.h>
#include <pthread.h>

static rio_slot_t  g_slots[RIO_MAX_REMOTE_FDS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int rio_map_alloc(remote_handle_t rh) {
    pthread_mutex_lock(&g_lock);
    int idx = -1;
    for (int i = 0; i < RIO_MAX_REMOTE_FDS; i++) {
        if (!g_slots[i].used) {
            g_slots[i].used = 1;
            g_slots[i].rh   = rh;
            idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (idx < 0) return -1;      /* 槽位耗尽 */
    return RIO_FAKE_FD_BASE + idx;
}

int rio_map_is_remote(int fd) {
    return fd >= RIO_FAKE_FD_BASE &&
           fd <  RIO_FAKE_FD_BASE + RIO_MAX_REMOTE_FDS &&
           g_slots[fd - RIO_FAKE_FD_BASE].used;
}

remote_handle_t rio_map_to_handle(int fd) {
    int idx = fd - RIO_FAKE_FD_BASE;
    return g_slots[idx].rh;
}

void rio_map_free(int fd) {
    if (!rio_map_is_remote(fd)) return;
    pthread_mutex_lock(&g_lock);
    int idx = fd - RIO_FAKE_FD_BASE;
    g_slots[idx].used = 0;
    g_slots[idx].rh   = 0;
    pthread_mutex_unlock(&g_lock);
}
