/*
 * fd 映射管理：假 fd ↔ 远程 handle (+ pack 元数据)
 *
 * 普通远程文件：  open("/remote/xxx")  →  alloc(rh)         → slot.pack=0
 * Pack 虚拟文件：  open("/virtual/xxx") → open(pack_file)   → alloc_pack(rh,offset,size) → slot.pack=1
 */

#define _GNU_SOURCE
#include "remoteio.h"
#include <string.h>
#include <pthread.h>

static rio_slot_t  g_slots[RIO_MAX_REMOTE_FDS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int alloc_slot_locked(remote_handle_t rh,
                             int pack, uint64_t offset, uint64_t size) {
    int idx = -1;
    for (int i = 0; i < RIO_MAX_REMOTE_FDS; i++) {
        if (!g_slots[i].used) {
            g_slots[i].used        = 1;
            g_slots[i].rh          = rh;
            g_slots[i].pack        = pack;
            g_slots[i].pack_offset = offset;
            g_slots[i].pack_size   = size;
            g_slots[i].pack_pos    = 0;
            idx = i;
            break;
        }
    }
    return idx;
}

int rio_map_alloc(remote_handle_t rh) {
    pthread_mutex_lock(&g_lock);
    int idx = alloc_slot_locked(rh, 0, 0, 0);
    pthread_mutex_unlock(&g_lock);
    if (idx < 0) return -1;
    return RIO_FAKE_FD_BASE + idx;
}

int rio_map_alloc_pack(remote_handle_t rh,
                       uint64_t offset, uint64_t size) {
    pthread_mutex_lock(&g_lock);
    int idx = alloc_slot_locked(rh, 1, offset, size);
    pthread_mutex_unlock(&g_lock);
    if (idx < 0) return -1;
    return RIO_FAKE_FD_BASE + idx;
}

int rio_map_is_remote(int fd) {
    return fd >= RIO_FAKE_FD_BASE &&
           fd <  RIO_FAKE_FD_BASE + RIO_MAX_REMOTE_FDS &&
           g_slots[fd - RIO_FAKE_FD_BASE].used;
}

int rio_map_is_pack(int fd) {
    if (!rio_map_is_remote(fd)) return 0;
    return g_slots[fd - RIO_FAKE_FD_BASE].pack;
}

remote_handle_t rio_map_to_handle(int fd) {
    int idx = fd - RIO_FAKE_FD_BASE;
    return g_slots[idx].rh;
}

/* 获取 fake fd 对应的 slot 指针（只读 —— 内部加锁调用） */
const rio_slot_t *rio_map_slot(int fd) {
    if (!rio_map_is_remote(fd)) return NULL;
    return &g_slots[fd - RIO_FAKE_FD_BASE];
}

/* 调整 pack 模式下的虚拟位置（由 rio_proto 在 lseek/read/write 时调用） */
void rio_map_set_pos(int fd, uint64_t pos) {
    if (!rio_map_is_pack(fd)) return;
    pthread_mutex_lock(&g_lock);
    int idx = fd - RIO_FAKE_FD_BASE;
    g_slots[idx].pack_pos = pos;
    pthread_mutex_unlock(&g_lock);
}

uint64_t rio_map_get_pos(int fd) {
    if (!rio_map_is_pack(fd)) return 0;
    return g_slots[fd - RIO_FAKE_FD_BASE].pack_pos;
}

void rio_map_free(int fd) {
    if (!rio_map_is_remote(fd)) return;
    pthread_mutex_lock(&g_lock);
    int idx = fd - RIO_FAKE_FD_BASE;
    g_slots[idx].used        = 0;
    g_slots[idx].rh          = 0;
    g_slots[idx].pack        = 0;
    g_slots[idx].pack_offset = 0;
    g_slots[idx].pack_size   = 0;
    g_slots[idx].pack_pos    = 0;
    pthread_mutex_unlock(&g_lock);
}
