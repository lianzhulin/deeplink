/*
 * rio_fdmap.c — 假 fd 映射表（pack 模式专用）
 *
 * 只有一种模式：pack —— 假 fd → pack_file 的远程 handle + 虚拟 offset/size/pos。
 * open(pack_file) 一次，多个虚拟 fd 共享同一个 rh，各自 track 独立 pack_pos。
 */

#define _GNU_SOURCE
#include "remoteio.h"
#include <string.h>
#include <pthread.h>

static rio_slot_t  g_slots[RIO_MAX_REMOTE_FDS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int rio_map_alloc(remote_handle_t rh, uint64_t offset, uint64_t size) {
    pthread_mutex_lock(&g_lock);
    int idx = -1;
    for (int i = 0; i < RIO_MAX_REMOTE_FDS; i++) {
        if (!g_slots[i].used) {
            g_slots[i].used        = 1;
            g_slots[i].rh          = rh;
            g_slots[i].pack_offset = offset;
            g_slots[i].pack_size   = size;
            g_slots[i].pack_pos    = 0;
            idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (idx < 0) return -1;
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

const rio_slot_t *rio_map_slot(int fd) {
    if (!rio_map_is_remote(fd)) return NULL;
    return &g_slots[fd - RIO_FAKE_FD_BASE];
}

void rio_map_set_pos(int fd, uint64_t pos) {
    if (!rio_map_is_remote(fd)) return;
    pthread_mutex_lock(&g_lock);
    int idx = fd - RIO_FAKE_FD_BASE;
    g_slots[idx].pack_pos = pos;
    pthread_mutex_unlock(&g_lock);
}

uint64_t rio_map_get_pos(int fd) {
    if (!rio_map_is_remote(fd)) return 0;
    return g_slots[fd - RIO_FAKE_FD_BASE].pack_pos;
}

void rio_map_free(int fd) {
    if (!rio_map_is_remote(fd)) return;
    pthread_mutex_lock(&g_lock);
    int idx = fd - RIO_FAKE_FD_BASE;
    g_slots[idx].used        = 0;
    g_slots[idx].rh          = 0;
    g_slots[idx].pack_offset = 0;
    g_slots[idx].pack_size   = 0;
    g_slots[idx].pack_pos    = 0;
    pthread_mutex_unlock(&g_lock);
}
