/*
 * rio_pack.c — pack 自描述模块
 *
 * 核心特性：pack.bin 自包含虚拟路径 → offset/size 映射表，
 * 客户端只需知道 pack_file 路径（通常是默认的 /remote/pack.bin）。
 * 首次 open 虚拟路径时自动从 pack.bin 头部加载映射，无需 INI、无需 register。
 */

#define _GNU_SOURCE
#include "rio_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 动态注册表 ---- */

#define RIO_PACK_INITIAL_CAP  16

static char              *g_pack_file = NULL;     /* 动态分配，默认 fallback */
static rio_pack_entry_t  *g_entries   = NULL;     /* 动态数组 */
static int                g_count     = 0;
static int                g_cap       = 0;
static int                g_loaded    = 0;        /* 是否已从 pack.bin 尝试加载过 */

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
    if (strlen(vpath) >= sizeof(g_entries[0].vpath)) {
        RIO_LOG("rio_pack_register: vpath too long: %s", vpath);
        return -1;
    }
    /* 查重 — 已存在则更新 */
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].vpath, vpath) == 0) {
            g_entries[i].offset = offset;
            g_entries[i].size   = size;
            return 0;
        }
    }
    if (ensure_cap(g_count + 1) < 0) return -1;
    strncpy(g_entries[g_count].vpath, vpath, sizeof(g_entries[g_count].vpath) - 1);
    g_entries[g_count].vpath[sizeof(g_entries[g_count].vpath) - 1] = '\0';
    g_entries[g_count].offset = offset;
    g_entries[g_count].size   = size;
    g_count++;
    return 0;
}

void rio_pack_clear(void) {
    free(g_entries);
    g_entries = NULL;
    g_count = 0;
    g_cap = 0;
}

const char *rio_pack_pack_file(void) {
    return g_pack_file ? g_pack_file : fallback_pack_file();
}

int rio_pack_is_enabled(void) {
    return g_count > 0;
}

/* ---- 懒加载：从服务端 pack.bin 头部读取映射表 ----
 *
 * 调用时机：第一次 rio_pack_lookup 被调用时（由 libremoteio.c 的 open 劫持触发）。
 * best-effort：失败时仅打日志，g_entries 保持空，后续 open 透传 real_open。
 */
int rio_pack_autoload(void) {
    if (g_loaded) return g_count;   /* 已尝试过（无论成功失败）*/
    g_loaded = 1;

    const char *pf = rio_pack_pack_file();
    RIO_LOG("rio_pack_autoload: connecting to server, reading %s header...", pf);

    if (rio_connect() < 0) {
        RIO_LOG("rio_pack_autoload: connect failed, skip autoload");
        return -1;
    }

    remote_handle_t rh = 0;
    if (rio_do_open(pf, O_RDONLY, 0, &rh) < 0) {
        RIO_LOG("rio_pack_autoload: open %s failed", pf);
        return -1;
    }

    /* 读头部 */
    rio_pack_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    ssize_t nh = rio_do_read(rh, &hdr, sizeof(hdr));
    if (nh < (ssize_t)sizeof(hdr)) {
        RIO_LOG("rio_pack_autoload: header read short (%ld < %zu)", (long)nh, sizeof(hdr));
        rio_do_close(rh);
        return -1;
    }

    /* 校验 magic */
    if (memcmp(hdr.magic, RIO_PACK_MAGIC, RIO_PACK_MAGIC_LEN) != 0) {
        RIO_LOG("rio_pack_autoload: bad magic (expected '%s', got '%.8s')",
                RIO_PACK_MAGIC, hdr.magic);
        rio_do_close(rh);
        return -1;
    }
    if (hdr.version != RIO_PACK_VERSION) {
        RIO_LOG("rio_pack_autoload: version mismatch (got %u, want %u)",
                hdr.version, RIO_PACK_VERSION);
        rio_do_close(rh);
        return -1;
    }
    if (hdr.n_entries == 0) {
        RIO_LOG("rio_pack_autoload: n_entries=0");
        rio_do_close(rh);
        return -1;
    }

    RIO_LOG("rio_pack_autoload: magic=%.8s version=%u n_entries=%u",
            hdr.magic, hdr.version, hdr.n_entries);

    /* 读 entries 数组 */
    uint32_t n = hdr.n_entries;
    rio_pack_entry_t *tmp = (rio_pack_entry_t *)calloc(n, sizeof(*tmp));
    if (!tmp) { rio_do_close(rh); return -1; }

    uint32_t need = n * sizeof(*tmp);
    uint8_t *buf = (uint8_t *)malloc(need);
    if (!buf) { free(tmp); rio_do_close(rh); return -1; }

    /* 头部 256B 之后紧跟 entries[] — 跳过头部已读过的，seek 到 256 */
    rio_do_lseek(rh, (off_t)sizeof(rio_pack_hdr_t), SEEK_SET);
    ssize_t ne = rio_do_read(rh, buf, need);
    if (ne < (ssize_t)need) {
        RIO_LOG("rio_pack_autoload: entries read short (%ld < %u)", (long)ne, need);
        free(buf); free(tmp); rio_do_close(rh);
        return -1;
    }

    rio_do_close(rh);

    memcpy(tmp, buf, need);
    free(buf);

    /* 填充全局表 */
    rio_pack_clear();
    if (ensure_cap((int)n) < 0) { free(tmp); return -1; }
    memcpy(g_entries, tmp, n * sizeof(*tmp));
    g_count = (int)n;
    free(tmp);

    RIO_LOG("rio_pack_autoload: loaded %d entries from pack.bin header", g_count);
    for (int i = 0; i < g_count; i++) {
        RIO_LOG("  [%d] %s  offset=%lu  size=%lu",
                i, g_entries[i].vpath,
                (unsigned long)g_entries[i].offset,
                (unsigned long)g_entries[i].size);
    }
    return g_count;
}

const rio_pack_entry_t *rio_pack_lookup(const char *vpath) {
    if (!vpath) return NULL;
    if (!g_loaded) {
        rio_pack_autoload();   /* 首次调用 — 触发懒加载 */
    }
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].vpath, vpath) == 0)
            return &g_entries[i];
    }
    return NULL;
}

void rio_pack_init(void) {
    /* 不在 constructor 里自动连服务器（太早、可能没启动），
     * 懒加载交给第一次 rio_pack_lookup 触发。 */
    RIO_LOG("rio_pack: init (pack_file=%s, autoload on first open)", rio_pack_pack_file());
}
