/*
 * rio_pack.c —— 多虚拟文件 → 单远程大文件
 *
 * 这里用静态数组声明每个虚拟路径在 pack_file 里的 offset 和 size。
 * 要加新段，直接在 g_entries 里追加一项即可。
 * 数组按 offset 递增排列（方便人类阅读）。
 */

#define _GNU_SOURCE
#include "rio_pack.h"
#include <string.h>

/* ============ 在这里声明所有虚拟文件 → pack 段映射 ============ */

static const rio_pack_entry_t g_entries[] = {
    /* virtual path            offset  size      备注 */
    { "/virtual/config.txt",   0,      64   },   /* seg1 */
    { "/virtual/header.bin",   64,     128  },   /* seg2 */
    { "/virtual/records.log",  192,    256  },   /* seg3 */
};

/* ============================================================ */

static const rio_pack_cfg_t g_cfg = {
    .prefix     = RIO_PACK_PREFIX,
    .pack_file  = RIO_PACK_FILE,
    .entries    = g_entries,
    .n_entries  = (int)(sizeof(g_entries) / sizeof(g_entries[0])),
};

void rio_pack_init(void) {
    /* constructor 里调一次，留空（启用是编译期决定的）*/
    RIO_LOG("rio_pack: pack_file=%s, n_entries=%d (compile-time enabled)",
            g_cfg.pack_file, g_cfg.n_entries);
}

const rio_pack_entry_t *rio_pack_lookup(const char *vpath) {
    if (!vpath) return NULL;
    for (int i = 0; i < g_cfg.n_entries; i++) {
        if (strcmp(g_cfg.entries[i].vpath, vpath) == 0)
            return &g_cfg.entries[i];
    }
    return NULL;
}

const rio_pack_cfg_t *rio_pack_cfg(void) { return &g_cfg; }
