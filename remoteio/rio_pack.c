/*
 * rio_pack.c — pack 自描述模块（共享内存模式）
 *
 * 服务端启动时创建 16MB shm 段 (RIO_PACK_SHM_NAME)，头部写入 rio_pack_hdr_t + entries[]。
 * 客户端首次 open() 虚拟路径时：
 *   1) rio_do_shm_open_req() 让服务端确认/初始化 shm
 *   2) 本地 shm_open() + mmap() 拿到 shm_base
 *   3) 从 shm_base 读 header + entries，填充 g_entries[]
 *
 * 之后所有虚拟文件 I/O 都是本地内存 memcpy —— 零网络、零磁盘。
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
static int                g_count     = 0;
static int                g_cap       = 0;
static int                g_loaded    = 0;

/* 客户端本地 mmap 基址和大小（autoload 后导出）*/
static uint8_t           *g_shm_base  = NULL;
static size_t             g_shm_size  = 0;

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

void rio_pack_clear(void) {
    free(g_entries);
    g_entries = NULL;
    g_count = 0;
    g_cap = 0;
}

int rio_pack_is_enabled(void) {
    return g_count > 0;
}

/* 返回客户端本地 shm mmap 基址 —— libremoteio.c 的 rio_map_alloc 需要 */
uint8_t *rio_pack_shm_base(void) { return g_shm_base; }
size_t   rio_pack_shm_size(void) { return g_shm_size; }

/* ---- 懒加载 ----
 *
 * 由 rio_pack_lookup 首次调用触发。流程：
 *   1) 发 OP_SHM_OPEN 给服务端 → 让服务端创建/确认 shm 段
 *   2) 客户端自己 shm_open(O_RDWR) + mmap
 *   3) memcpy 读 header → 校验 magic/version
 *   4) memcpy 读 entries → 填充 g_entries[]
 */
int rio_pack_autoload(void) {
    if (g_loaded) return g_count;
    g_loaded = 1;

    RIO_LOG("rio_pack_autoload: requesting shm init from server...");
    if (rio_do_shm_open_req(RIO_PACK_SHM_NAME) < 0) {
        RIO_LOG("rio_pack_autoload: server shm_open failed");
        return -1;
    }

    /* 客户端 mmap 同一段 shm */
    int fd = shm_open(RIO_PACK_SHM_NAME, O_RDWR, 0);
    if (fd < 0) {
        RIO_LOG("rio_pack_autoload: shm_open(%s) failed: %s",
                RIO_PACK_SHM_NAME, strerror(errno));
        return -1;
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
    RIO_LOG("rio_pack_autoload: mmap %s at %p size=%zu",
            RIO_PACK_SHM_NAME, (void *)base, RIO_PACK_SHM_SIZE);

    /* 从 shm 基址读 header */
    const rio_pack_hdr_t *hdr = (const rio_pack_hdr_t *)base;
    if (memcmp(hdr->magic, RIO_PACK_MAGIC, RIO_PACK_MAGIC_LEN) != 0) {
        RIO_LOG("rio_pack_autoload: bad magic (expected '%s', got '%.8s')",
                RIO_PACK_MAGIC, hdr->magic);
        return -1;
    }
    if (hdr->version != RIO_PACK_VERSION) {
        RIO_LOG("rio_pack_autoload: version mismatch (got %u, want %u)",
                hdr->version, RIO_PACK_VERSION);
        return -1;
    }
    if (hdr->n_entries == 0 || hdr->n_entries > 1024) {
        RIO_LOG("rio_pack_autoload: n_entries=%u looks wrong", hdr->n_entries);
        return -1;
    }
    RIO_LOG("rio_pack_autoload: magic=%.8s version=%u n_entries=%u",
            hdr->magic, hdr->version, hdr->n_entries);

    /* 填充 g_entries[] —— 直接从 shm 里 memcpy */
    uint32_t n = hdr->n_entries;
    rio_pack_clear();
    if (ensure_cap((int)n) < 0) return -1;

    const rio_pack_entry_t *e_src = (const rio_pack_entry_t *)(base + sizeof(rio_pack_hdr_t));
    for (uint32_t i = 0; i < n; i++) {
        memcpy(&g_entries[i], &e_src[i], sizeof(rio_pack_entry_t));
        g_count++;
        RIO_LOG("  [%u] %s  offset=%lu  size=%lu",
                i, g_entries[i].vpath,
                (unsigned long)g_entries[i].offset,
                (unsigned long)g_entries[i].size);
    }
    RIO_LOG("rio_pack_autoload: loaded %d entries (SHM mode)", g_count);
    return g_count;
}

const rio_pack_entry_t *rio_pack_lookup(const char *vpath) {
    if (!vpath) return NULL;
    if (!g_loaded) {
        rio_pack_autoload();
    }
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].vpath, vpath) == 0)
            return &g_entries[i];
    }
    return NULL;
}

/* shm 模式下这些函数都是 stub —— pack 文件路径已不适用，
 * 因为 pack 内容直接存在服务端初始化的 16MB POSIX 共享内存里 */
void rio_pack_set_pack_file(const char *pack_file) {
    (void)pack_file;
    /* no-op — shm 模式下 pack 路径由 RIO_PACK_SHM_NAME 固定 */
}

const char *rio_pack_pack_file(void) {
    return "/dev/shm" RIO_PACK_SHM_NAME;
}

int rio_pack_register(const char *vpath, uint64_t offset, uint64_t size) {
    (void)vpath; (void)offset; (void)size;
    /* shm 模式下不支持 register —— pack header 是 self-describing */
    return -1;
}

void rio_pack_init(void) {
    RIO_LOG("rio_pack: init (shm_name=%s, size=%dMB, autoload on first open)",
            RIO_PACK_SHM_NAME, RIO_PACK_SHM_SIZE / (1024 * 1024));
}
