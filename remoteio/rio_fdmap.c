/*
 * rio_fdmap.c — 假 fd 映射表（共享内存 pack 模式）
 *
 * 假 fd → (mmap 基址 + 虚拟偏移 + 虚拟长度 + 当前位置)
 *
 * read/write 直接对 mmap 区域 memcpy，lseek 只更新 local_pos。
 * 没有远程 handle、没有远程 close —— 所有 fd 关闭只是释放槽位。
 */

#define _GNU_SOURCE
#include "remoteio.h"
#include <string.h>
#include <pthread.h>

static rio_slot_t  g_slots[RIO_MAX_REMOTE_FDS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int rio_map_alloc(uint8_t *shm_base, size_t shm_size,
                  uint64_t virtual_offset, uint64_t virtual_size) {
    pthread_mutex_lock(&g_lock);
    int idx = -1;
    for (int i = 0; i < RIO_MAX_REMOTE_FDS; i++) {
        if (!g_slots[i].used) {
            g_slots[i].used        = 1;
            g_slots[i].shm_base    = shm_base;
            g_slots[i].shm_size    = shm_size;
            g_slots[i].virtual_off = virtual_offset;
            g_slots[i].virtual_sz  = virtual_size;
            g_slots[i].local_pos   = 0;
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

const rio_slot_t *rio_map_slot(int fd) {
    if (!rio_map_is_remote(fd)) return NULL;
    return &g_slots[fd - RIO_FAKE_FD_BASE];
}

void rio_map_set_pos(int fd, uint64_t pos) {
    if (!rio_map_is_remote(fd)) return;
    pthread_mutex_lock(&g_lock);
    g_slots[fd - RIO_FAKE_FD_BASE].local_pos = pos;
    pthread_mutex_unlock(&g_lock);
}

uint64_t rio_map_get_pos(int fd) {
    if (!rio_map_is_remote(fd)) return 0;
    return g_slots[fd - RIO_FAKE_FD_BASE].local_pos;
}

void rio_map_free(int fd) {
    if (!rio_map_is_remote(fd)) return;
    pthread_mutex_lock(&g_lock);
    int idx = fd - RIO_FAKE_FD_BASE;
    g_slots[idx].used        = 0;
    g_slots[idx].shm_base    = NULL;
    g_slots[idx].shm_size    = 0;
    g_slots[idx].virtual_off = 0;
    g_slots[idx].virtual_sz  = 0;
    g_slots[idx].local_pos   = 0;
    pthread_mutex_unlock(&g_lock);
}
