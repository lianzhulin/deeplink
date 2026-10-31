/*
 * rio_pack.c —— pack 映射运行时注册表
 *
 * 动态数组实现，不含任何硬编码的虚拟路径或 offset/size。
 * 所有业务映射由外部通过 rio_pack_register() 在运行时注入。
 */

#define _GNU_SOURCE
#include "rio_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 动态注册表 ---- */

#define RIO_PACK_INITIAL_CAP  16

static char              *g_pack_file = NULL;     /* 动态分配，默认用 RIO_PACK_FILE */
static rio_pack_entry_t  *g_entries   = NULL;     /* 动态数组 */
static int                g_count     = 0;
static int                g_cap       = 0;

/* ---- 内部 helper ---- */

static const char *fallback_pack_file(void) {
    return RIO_PACK_FILE;
}

static int ensure_cap(int need) {
    if (g_cap >= need) return 0;
    int new_cap = g_cap ? g_cap : RIO_PACK_INITIAL_CAP;
    while (new_cap < need) new_cap *= 2;
    rio_pack_entry_t *p = realloc(g_entries, sizeof(*p) * new_cap);
    if (!p) return -1;
    g_entries = p;
    g_cap = new_cap;
    return 0;
}

/* ---- 对外 API ---- */

void rio_pack_set_pack_file(const char *pack_file) {
    if (!pack_file) return;
    free(g_pack_file);
    g_pack_file = strdup(pack_file);
}

int rio_pack_register(const char *vpath, uint64_t offset, uint64_t size) {
    if (!vpath || !*vpath) return -1;
    /* 查重 */
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].vpath, vpath) == 0) {
            /* 更新已有条目 */
            g_entries[i].offset = offset;
            g_entries[i].size   = size;
            return 0;
        }
    }
    if (ensure_cap(g_count + 1) < 0) return -1;
    char *dup = strdup(vpath);
    if (!dup) return -1;
    g_entries[g_count].vpath = dup;
    g_entries[g_count].offset = offset;
    g_entries[g_count].size   = size;
    g_count++;
    return 0;
}

void rio_pack_clear(void) {
    for (int i = 0; i < g_count; i++) {
        free((void *)g_entries[i].vpath);
    }
    free(g_entries);
    g_entries = NULL;
    g_count = 0;
    g_cap = 0;
}

const rio_pack_entry_t *rio_pack_lookup(const char *vpath) {
    if (!vpath) return NULL;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].vpath, vpath) == 0)
            return &g_entries[i];
    }
    return NULL;
}

const char *rio_pack_pack_file(void) {
    return g_pack_file ? g_pack_file : fallback_pack_file();
}

int rio_pack_is_enabled(void) {
    return g_count > 0;
}

void rio_pack_init(void) {
    /* constructor 里调一次 —— 留空，避免在构造函数里做 malloc */
    RIO_LOG("rio_pack: init (entries will be registered at runtime)");
}
