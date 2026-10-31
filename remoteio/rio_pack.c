/*
 * rio_pack.c — pack 自描述模块（按需拉取 + 版本化）
 *
 * 核心设计：
 *   1) autoload 只拉 meta（~3KB header + entries + versions[]）
 *   2) 每段 body 按需拉（第一次读某段才拉，之后版本检测）
 *   3) 写穿透：本地 dirty + close/flush 时 OP_WRITE_SEG 写回服务端
 *   4) 服务端维护每段 version，客户端每次 read 前检查
 *
 * 网络开销极小：
 *   - 首次: ~3KB meta
 *   - 每段 read: OP_SEG_VERSION (8B ret_val) + OP_SEG_READ (seg_size) if version changed
 *   - write: 本地 memcpy，close/flush 才穿透
 */

#define _GNU_SOURCE
#include "rio_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ---- 动态注册表 ---- */

#define RIO_PACK_INITIAL_CAP  16

static rio_pack_entry_t  *g_entries   = NULL;
static int                g_count     = 0;   /* == n_entries from pack header */
static int                g_cap       = 0;
static int                g_loaded    = 0;

/* 客户端本地 mmap 基址和大小 */
static uint8_t           *g_shm_base  = NULL;
static size_t             g_shm_size  = 0;

/* 每段状态数组 —— 长度 = g_count */
static uint8_t          *g_seg_loaded = NULL;   /* 0/1 是否已拉取 body 到本地 shm */
static uint8_t          *g_seg_dirty  = NULL;   /* 0/1 本地是否有未穿透的修改 */
static uint32_t         *g_seg_versions = NULL; /* 客户端上次看到的服务端 version */

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

static int alloc_seg_state(int n) {
    if (g_seg_loaded) free(g_seg_loaded);
    if (g_seg_dirty)  free(g_seg_dirty);
    if (g_seg_versions) free(g_seg_versions);
    g_seg_loaded = (uint8_t *)calloc((size_t)n, 1);
    g_seg_dirty  = (uint8_t *)calloc((size_t)n, 1);
    g_seg_versions = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    if (!g_seg_loaded || !g_seg_dirty || !g_seg_versions) {
        free(g_seg_loaded); free(g_seg_dirty); free(g_seg_versions);
        g_seg_loaded = g_seg_dirty = NULL; g_seg_versions = NULL;
        return -1;
    }
    return 0;
}

/* ---- 对外 API ---- */

void rio_pack_clear(void) {
    free(g_entries);
    g_entries = NULL;
    g_count = 0;
    g_cap = 0;
    free(g_seg_loaded);    g_seg_loaded = NULL;
    free(g_seg_dirty);     g_seg_dirty = NULL;
    free(g_seg_versions);  g_seg_versions = NULL;
}

int rio_pack_is_enabled(void) {
    return g_count > 0;
}

uint8_t *rio_pack_shm_base(void) { return g_shm_base; }
size_t   rio_pack_shm_size(void) { return g_shm_size; }

/* ---- 懒加载（只拉 meta，不拉 body）---- */
int rio_pack_autoload(void) {
    if (g_loaded) return g_count;
    g_loaded = 1;

    RIO_LOG("rio_pack_autoload: OP_PACK_META 拉 meta...");

    /* 先拉 meta：[4B n][hdr][entries][4B*n versions] */
    /* 先分配足够大的 buffer（header=256, entry=144, 假设最多1024段）*/
    uint32_t meta_cap = 4 + sizeof(rio_pack_hdr_t) +
                        1024 * (uint32_t)(sizeof(rio_pack_entry_t) + sizeof(uint32_t));
    uint8_t *meta_buf = (uint8_t *)malloc(meta_cap);
    if (!meta_buf) { RIO_LOG("meta malloc fail"); return -1; }

    uint32_t meta_len = 0;
    if (rio_do_pack_meta(meta_buf, meta_cap, &meta_len) < 0) {
        RIO_LOG("rio_do_pack_meta 失败");
        free(meta_buf);
        return -1;
    }

    /* 解析 meta */
    uint32_t off = 0;
    uint32_t n = 0;
    memcpy(&n, meta_buf + off, 4); off += 4;
    if (n == 0 || n > 1024) {
        RIO_LOG("meta: n_entries=%u 不合理", n);
        free(meta_buf); return -1;
    }

    /* 校验 header */
    const rio_pack_hdr_t *hdr = (const rio_pack_hdr_t *)(meta_buf + off);
    off += sizeof(rio_pack_hdr_t);
    if (memcmp(hdr->magic, RIO_PACK_MAGIC, RIO_PACK_MAGIC_LEN) != 0) {
        RIO_LOG("meta: bad magic '%.8s'", hdr->magic);
        free(meta_buf); return -1;
    }
    if (hdr->version != RIO_PACK_VERSION) {
        RIO_LOG("meta: version mismatch %u vs %u", hdr->version, RIO_PACK_VERSION);
        free(meta_buf); return -1;
    }
    if (hdr->n_entries != n) {
        RIO_LOG("meta: n mismatch (header=%u, meta=%u)", hdr->n_entries, n);
        free(meta_buf); return -1;
    }

    /* 拷贝 entries */
    rio_pack_clear();
    if (ensure_cap((int)n) < 0) { free(meta_buf); return -1; }
    memcpy(g_entries, meta_buf + off, sizeof(rio_pack_entry_t) * n);
    off += sizeof(rio_pack_entry_t) * n;
    g_count = (int)n;

    /* 拷贝初始 versions */
    if (alloc_seg_state((int)n) < 0) { free(meta_buf); rio_pack_clear(); return -1; }
    memcpy(g_seg_versions, meta_buf + off, sizeof(uint32_t) * n);
    off += sizeof(uint32_t) * n;

    RIO_LOG("rio_pack_autoload: meta 解析完成, n_segs=%u", n);
    for (uint32_t i = 0; i < n; i++) {
        RIO_LOG("  [%u] %-28s off=%lu  sz=%lu  v=%u",
                i, g_entries[i].vpath,
                (unsigned long)g_entries[i].offset,
                (unsigned long)g_entries[i].size,
                g_seg_versions[i]);
    }

    /* 先把 hdr 存本地（hdr 指向 meta_buf，后面要 free meta_buf）*/
    rio_pack_hdr_t hdr_copy;
    memcpy(&hdr_copy, hdr, sizeof(rio_pack_hdr_t));
    free(meta_buf);

    /* 客户端本地创建 16MB shm —— 只填 header + entries（body 区域先空着）*/
    int fd = shm_open(RIO_PACK_SHM_NAME, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        RIO_LOG("rio_pack_autoload: shm_open(%s) failed: %s",
                RIO_PACK_SHM_NAME, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, RIO_PACK_SHM_SIZE) < 0) {
        RIO_LOG("rio_pack_autoload: ftruncate failed: %s", strerror(errno));
        close(fd); return -1;
    }

    uint8_t *base = (uint8_t *)mmap(NULL, RIO_PACK_SHM_SIZE,
                                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        RIO_LOG("rio_pack_autoload: mmap failed: %s", strerror(errno));
        return -1;
    }

    g_shm_base = base;
    g_shm_size = RIO_PACK_SHM_SIZE;

    /* 清零 + 把 header + entries 写入本地 shm（body 区域保持 0 —— 每次 ensure_seg 从服务端拉）*/
    memset(base, 0, RIO_PACK_SHM_SIZE);
    memcpy(base, &hdr_copy, sizeof(rio_pack_hdr_t));
    memcpy(base + sizeof(rio_pack_hdr_t), g_entries, sizeof(rio_pack_entry_t) * n);

    RIO_LOG("rio_pack_autoload: 本地 shm mmap %s at %p size=%zu  "
            "(meta 已填, body 按需拉)",
            RIO_PACK_SHM_NAME, (void *)base, RIO_PACK_SHM_SIZE);

    return g_count;
}

const rio_pack_entry_t *rio_pack_lookup(const char *vpath) {
    if (!vpath) return NULL;
    if (!g_loaded) { rio_pack_autoload(); }
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].vpath, vpath) == 0)
            return &g_entries[i];
    }
    return NULL;
}

int rio_pack_find_seg_idx(const char *vpath) {
    if (!vpath) return -1;
    if (!g_loaded) { rio_pack_autoload(); }
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].vpath, vpath) == 0)
            return i;
    }
    return -1;
}

/* ---- 按需拉取 ----
 *
 * rio_pack_ensure_seg(seg_idx):
 *   1) OP_SEG_VERSION 快速查询服务端当前 version
 *   2) 如果本地没加载 或者 version 变了 → OP_SEG_READ 拉完整 body
 *   3) 把拉到的 body memcpy 到本地 shm 对应位置
 */
int rio_pack_ensure_seg(uint32_t seg_idx) {
    if ((int)seg_idx >= g_count || !g_shm_base) {
        RIO_LOG("rio_pack_ensure_seg: seg_idx=%u 超出范围", seg_idx);
        return -1;
    }

    /* 每次都拉最新 body — 保证"他人"修改服务端 body 后客户端下次 read 自动看到。
     * 网络开销 = seg_size 字节（64/128/256/...），远小于 16MB 全量同步。*/
    uint64_t seg_sz = g_entries[seg_idx].size;
    if (seg_sz == 0 || seg_sz > (uint64_t)RIO_PACK_SHM_SIZE) {
        RIO_LOG("rio_pack_ensure_seg(%u): seg_size=%lu 异常",
                seg_idx, (unsigned long)seg_sz);
        return -1;
    }

    uint8_t *tmp = (uint8_t *)malloc((size_t)seg_sz);
    if (!tmp) { RIO_LOG("seg read tmp alloc fail"); return -1; }

    uint32_t got_version = 0;
    if (rio_do_seg_read(seg_idx, (uint32_t)seg_sz, tmp, (uint32_t)seg_sz,
                        &got_version) < 0) {
        RIO_LOG("rio_pack_ensure_seg(%u): OP_SEG_READ 失败", seg_idx);
        free(tmp);
        return -1;
    }

    /* 写入本地 shm 对应位置 */
    uint64_t local_off = g_entries[seg_idx].offset;
    memcpy(g_shm_base + local_off, tmp, (size_t)seg_sz);
    free(tmp);

    g_seg_loaded[seg_idx] = 1;
    g_seg_versions[seg_idx] = got_version;

    RIO_LOG("rio_pack_ensure_seg(%u): %s loaded  (version=%u)",
            seg_idx, g_entries[seg_idx].vpath, got_version);
    return 0;
}

void rio_pack_mark_dirty(uint32_t seg_idx) {
    if ((int)seg_idx >= g_count) return;
    g_seg_dirty[seg_idx] = 1;
}

/* 把指定 seg 完整穿透到服务端 —— write/pwrite 后立刻调
 * 返回 0 成功，-1 失败 */
int rio_pack_push_seg(uint32_t seg_idx) {
    if ((int)seg_idx >= g_count || !g_shm_base) return -1;
    uint64_t off = g_entries[seg_idx].offset;
    uint64_t sz  = g_entries[seg_idx].size;
    int rc = rio_do_write_seg(seg_idx, g_shm_base + off, (uint32_t)sz);
    if (rc == 0) {
        g_seg_dirty[seg_idx] = 0;
        g_seg_versions[seg_idx]++;
        RIO_LOG("rio_pack_push_seg(%u) %s → %lu bytes pushed",
                seg_idx, g_entries[seg_idx].vpath, (unsigned long)sz);
    } else {
        RIO_LOG("rio_pack_push_seg(%u) 失败", seg_idx);
    }
    return rc;
}

int rio_pack_flush_dirty_segs(void) {
    if (!g_seg_dirty || !g_shm_base) return 0;
    int written = 0;
    for (int i = 0; i < g_count; i++) {
        if (!g_seg_dirty[i] || !g_seg_loaded[i]) continue;
        uint64_t sz = g_entries[i].size;
        uint64_t off = g_entries[i].offset;
        if (rio_do_write_seg((uint32_t)i, g_shm_base + off, (uint32_t)sz) == 0) {
            g_seg_dirty[i] = 0;
            written++;
            /* 服务端 version++ —— 我们看不到具体值了，但至少知道自己刚写过 */
            g_seg_versions[i]++;
            RIO_LOG("rio_pack_flush_dirty_segs: seg[%d] %s written",
                    i, g_entries[i].vpath);
        } else {
            RIO_LOG("rio_pack_flush_dirty_segs: seg[%d] write failed", i);
        }
    }
    return written;
}

/* ---- stub 函数 ---- */

void rio_pack_set_pack_file(const char *pack_file) { (void)pack_file; }

const char *rio_pack_pack_file(void) {
    return "/dev/shm" RIO_PACK_SHM_NAME;
}

int rio_pack_register(const char *vpath, uint64_t offset, uint64_t size) {
    (void)vpath; (void)offset; (void)size;
    return -1;
}

void rio_pack_init(void) {
    RIO_LOG("rio_pack: init (shm_name=%s, size=%dMB, 按需拉取+版本化)",
            RIO_PACK_SHM_NAME, RIO_PACK_SHM_SIZE / (1024 * 1024));
}
