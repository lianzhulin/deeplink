/*
 * remote_server.c — 共享内存 pack 模式专用服务端
 *
 * 核心职责：
 *   1) 启动时创建 16MB POSIX 共享内存段 (RIO_PACK_SHM_NAME)
 *      → ftruncate(16MB) → mmap
 *      → 写入默认 rio_pack_hdr_t + rio_pack_entry_t[] + 测试 body
 *   2) 监听 TCP (默认 9527)，处理 OP_SHM_OPEN 请求
 *      → 让客户端确认 shm 存在后也能自己 shm_open + mmap
 *
 * 注意：
 *   - 所有虚拟文件 I/O 都是客户端本地 memcpy —— 这里没有 open/read/write 处理！
 *   - 服务端创建的 shm 段是全局可见的 POSIX shm，客户端直接 shm_open(RIO_PACK_SHM_NAME)
 *   - 可选：gen_pack.py 也可以在运行前直接写 shm，server 启动时检查 magic 决定是否重写
 */

#define _GNU_SOURCE
#include "remoteio.h"
#include "rio_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>

/* ================================================================
 *  共享内存 pack 初始化
 * ================================================================ */

static void *g_shm_base = NULL;
static size_t g_shm_size = RIO_PACK_SHM_SIZE;

/* 每段 version —— 任何人修改某段 body 后 +1（服务端权威版本）*/
static uint32_t *g_seg_versions = NULL;
static uint32_t  g_n_segs_srv  = 0;

/*
 * 在 shm 里写入默认的测试 pack 数据（和 test_pack_client.c / gen_pack.py 预期一致）。
 * 如果 shm 已经有正确的 magic（说明 gen_pack.py 写过），就跳过。
 */
static void write_default_pack(void *base) {
    uint8_t *p = (uint8_t *)base;
    rio_pack_hdr_t *hdr = (rio_pack_hdr_t *)p;

    /* 已经初始化好就跳过 —— 让 gen_pack.py 或之前写入的数据保留 */
    if (memcmp(hdr->magic, RIO_PACK_MAGIC, RIO_PACK_MAGIC_LEN) == 0 &&
        hdr->version == RIO_PACK_VERSION &&
        hdr->n_entries > 0 && hdr->n_entries <= 1024) {
        printf("[srv] shm 已有有效 pack 头部 (magic=%.8s version=%u n=%u)，跳过默认初始化\n",
               hdr->magic, hdr->version, hdr->n_entries);
        return;
    }

    /* --- segment 定义（和 gen_pack.py 精确对齐） --- */
    /* body 起始 = 256 + 3*144 = 688 */
    uint32_t n_segs = 3;
    uint64_t body_off = sizeof(rio_pack_hdr_t) + n_segs * sizeof(rio_pack_entry_t);

    struct { const char *vpath; uint64_t size; const char *content; } segs[] = {
        { "/virtual/config.txt",   64,  "=== config.txt: hello pack ===\n" },
        { "/virtual/header.bin",   128, "HEADER_BIN_MAGIC" "\x00\x01\x02\x03\xde\xad\xbe\xef" },
        { "/virtual/records.log",  256, "records line 1\nrecords line 2\nrecords line 3\n" },
    };

    /* --- 写 header --- */
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, RIO_PACK_MAGIC, RIO_PACK_MAGIC_LEN);
    hdr->version    = RIO_PACK_VERSION;
    hdr->n_entries  = n_segs;
    hdr->reserved   = 0;

    /* --- 写 entries --- */
    rio_pack_entry_t *entries = (rio_pack_entry_t *)(p + sizeof(*hdr));
    uint64_t cur_off = body_off;
    for (uint32_t i = 0; i < n_segs; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        snprintf(entries[i].vpath, sizeof(entries[i].vpath), "%s", segs[i].vpath);
        entries[i].offset = cur_off;
        entries[i].size   = segs[i].size;
        cur_off += segs[i].size;
    }

    /* --- 写 body --- */
    memset(p + body_off, 0, g_shm_size - body_off);
    cur_off = body_off;
    for (uint32_t i = 0; i < n_segs; i++) {
        uint64_t content_len = strlen(segs[i].content);
        memcpy(p + cur_off, segs[i].content, content_len);
        /* 剩余用 'X' 填充 */
        memset(p + cur_off + content_len, 'X', segs[i].size - content_len);
        printf("  [%u] %-24s offset=%lu  size=%lu  content=%lu + pad=%lu\n",
               i, segs[i].vpath,
               (unsigned long)entries[i].offset,
               (unsigned long)entries[i].size,
               (unsigned long)content_len,
               (unsigned long)(segs[i].size - content_len));
        cur_off += segs[i].size;
    }

    printf("[srv] 默认 pack 初始化完成 (body @ %lu, used %lu / %lu bytes)\n",
           (unsigned long)body_off,
           (unsigned long)cur_off,
           (unsigned long)g_shm_size);

    /* 初始化 seg_versions 数组 —— 和 n_entries 一致 */
    g_n_segs_srv = n_segs;
    if (g_seg_versions) free(g_seg_versions);
    g_seg_versions = (uint32_t *)calloc(n_segs, sizeof(uint32_t));
}

/*
 * 初始化 16MB POSIX shm：
 *   shm_open(name, O_RDWR | O_CREAT | O_TRUNC, 0644)
 *   ftruncate(fd, size)
 *   mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)
 */
static int init_shm(const char *shm_name, size_t shm_size, int truncate_existing) {
    /* 先尝试不带 O_TRUNC 打开已有的（看 magic） */
    int fd = shm_open(shm_name, O_RDWR, 0644);
    if (fd >= 0) {
        /* 已存在 —— mmap 看 magic */
        void *base = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) {
            close(fd);
            fprintf(stderr, "[srv] mmap existing shm failed: %s\n", strerror(errno));
            return -1;
        }
        rio_pack_hdr_t *hdr = (rio_pack_hdr_t *)base;
        int has_valid = (memcmp(hdr->magic, RIO_PACK_MAGIC, RIO_PACK_MAGIC_LEN) == 0 &&
                         hdr->version == RIO_PACK_VERSION);
        munmap(base, shm_size);
        close(fd);

        if (has_valid && !truncate_existing) {
            printf("[srv] shm %s 已存在且有有效 magic，跳过 truncate\n", shm_name);
        } else {
            /* 删掉重建 */
            shm_unlink(shm_name);
            fd = -1;
        }
    }

    if (fd < 0) {
        fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd < 0 && errno == EEXIST) {
            /* 竞争：别人刚创建了，再不带 O_EXCL 打开 */
            fd = shm_open(shm_name, O_RDWR, 0644);
        }
        if (fd < 0) {
            fprintf(stderr, "[srv] shm_open(%s) failed: %s\n", shm_name, strerror(errno));
            return -1;
        }
        if (ftruncate(fd, (off_t)shm_size) < 0) {
            fprintf(stderr, "[srv] ftruncate(%s, %zu) failed: %s\n",
                    shm_name, shm_size, strerror(errno));
            close(fd);
            shm_unlink(shm_name);
            return -1;
        }
    }

    void *base = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        fprintf(stderr, "[srv] mmap(%s) failed: %s\n", shm_name, strerror(errno));
        return -1;
    }

    g_shm_base = base;
    g_shm_size = shm_size;

    /* 清空 shm 前几 KB（header + entries 区域）以确保写入干净 */
    memset(base, 0, 4096);

    /* 写默认 pack 数据（如果还没写过）*/
    write_default_pack(base);

    /* 兜底：如果 write_default_pack 跳过了（已有有效 magic），
     * seg_versions 还没初始化 —— 从 header 里读 n_entries 来补 */
    if (g_seg_versions == NULL) {
        const rio_pack_hdr_t *hdr_chk = (const rio_pack_hdr_t *)base;
        uint32_t n = hdr_chk->n_entries;
        g_seg_versions = (uint32_t *)calloc(n, sizeof(uint32_t));
        g_n_segs_srv = n;
        printf("[srv] (兜底) 从已有 shm header 初始化 seg_versions n=%u\n", n);
    }

    printf("[srv] shm %s ready  base=%p  size=%zu  n_segs=%u\n",
           shm_name, base, shm_size, g_n_segs_srv);
    return 0;
}

/* ================================================================
 *  TCP 服务端（只处理 OP_SHM_OPEN）
 * ================================================================ */

static ssize_t recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf; size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return got;
}

static ssize_t send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf; size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, p + sent, n - sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return sent;
}

static int send_resp(int cli, uint32_t opcode, uint64_t ret_val,
                     const void *extra, uint32_t extra_len) {
    uint32_t body_len = 8 + extra_len;
    rio_hdr_t hdr = { .magic = RIO_MAGIC, .opcode = opcode, .body_len = body_len };
    if (send_all(cli, &hdr, sizeof(hdr)) < 0) return -1;
    if (send_all(cli, &ret_val, 8) < 0) return -1;
    if (extra_len && send_all(cli, extra, extra_len) < 0) return -1;
    return 0;
}

/* ---- OP_PACK_META：客户端拉 pack 元数据（3KB 级别）----
 * 响应 body: [4B n_entries][rio_pack_hdr_t][rio_pack_entry_t*n][4B*n versions]
 * 只包含 header+entries+版本数组 —— 不含 16MB body */
static void handle_pack_meta(int cli) {
    uint32_t n = g_n_segs_srv;
    const rio_pack_hdr_t *hdr = (const rio_pack_hdr_t *)g_shm_base;
    const rio_pack_entry_t *entries = (const rio_pack_entry_t *)(g_shm_base + sizeof(rio_pack_hdr_t));

    uint32_t meta_len = 4 + sizeof(rio_pack_hdr_t) +
                        sizeof(rio_pack_entry_t) * n + sizeof(uint32_t) * n;
    uint8_t *meta = (uint8_t *)malloc(meta_len);
    if (!meta) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }

    uint32_t off = 0;
    memcpy(meta + off, &n, 4);                           off += 4;
    memcpy(meta + off, hdr, sizeof(rio_pack_hdr_t));     off += sizeof(rio_pack_hdr_t);
    memcpy(meta + off, entries, sizeof(rio_pack_entry_t) * n); off += sizeof(rio_pack_entry_t) * n;
    memcpy(meta + off, g_seg_versions, sizeof(uint32_t) * n);

    /* ret_val 填 n 方便客户端校验 */
    send_resp(cli, OP_ACK, (uint64_t)n, meta, meta_len);
    printf("[srv] OP_PACK_META → n=%u  meta_len=%u\n", n, meta_len);
    free(meta);
}

/* ---- OP_SEG_VERSION: 只查某段 version（不拉 body）---- */
static void handle_seg_version(int cli, const uint8_t *body, uint32_t len) {
    if (len < 4) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    uint32_t idx; memcpy(&idx, body, 4);
    if (idx >= g_n_segs_srv) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    send_resp(cli, OP_ACK, (uint64_t)g_seg_versions[idx], NULL, 0);
}

/* ---- OP_SEG_READ: 拉某段完整 body ----
 * ret_val = version，extra body = seg 内容（从 g_shm_base + entry.offset）*/
static void handle_seg_read(int cli, const uint8_t *body, uint32_t len) {
    if (len < 4) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    uint32_t idx; memcpy(&idx, body, 4);
    if (idx >= g_n_segs_srv) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }

    const rio_pack_entry_t *entry = (const rio_pack_entry_t *)(g_shm_base + sizeof(rio_pack_hdr_t)) + idx;
    uint64_t off = entry->offset;
    uint32_t sz  = (uint32_t)entry->size;

    send_resp(cli, OP_ACK, (uint64_t)g_seg_versions[idx],
              g_shm_base + off, sz);
    printf("[srv] OP_SEG_READ seg[%u] %s → %u bytes version=%u\n",
           idx, entry->vpath, sz, g_seg_versions[idx]);
}

/* ---- OP_WRITE_SEG: 客户端穿透写某段 ----
 * body: [4B seg_idx][4B data_len][data...]
 * 服务端: memcpy(g_shm_base + entry.offset, data, data_len) + version++ */
static void handle_write_seg(int cli, const uint8_t *body, uint32_t len) {
    if (len < 8) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    uint32_t idx, dlen;
    uint32_t off = 0;
    memcpy(&idx, body + off, 4); off += 4;
    memcpy(&dlen, body + off, 4); off += 4;
    if (idx >= g_n_segs_srv) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    if (off + dlen > len) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }

    const rio_pack_entry_t *entry = (const rio_pack_entry_t *)(g_shm_base + sizeof(rio_pack_hdr_t)) + idx;
    uint64_t seg_off = entry->offset;
    uint32_t seg_sz  = (uint32_t)entry->size;

    /* 安全：最多写 seg_sz 字节 */
    uint32_t n = dlen < seg_sz ? dlen : seg_sz;
    memcpy(g_shm_base + seg_off, body + off, n);
    g_seg_versions[idx]++;  /* 修改后 version++ —— 客户端下次 OP_SEG_VERSION 就能发现 */

    send_resp(cli, OP_ACK, 0, NULL, 0);
    printf("[srv] OP_WRITE_SEG seg[%u] %s ← %u bytes  version→%u\n",
           idx, entry->vpath, n, g_seg_versions[idx]);
}

static void serve_client(int cli) {
    rio_hdr_t hdr;
    while (1) {
        if (recv_all(cli, &hdr, sizeof(hdr)) < 0) break;
        if (hdr.magic != RIO_MAGIC) {
            send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0);
            break;
        }
        uint8_t *body = NULL;
        if (hdr.body_len) {
            body = (uint8_t *)malloc(hdr.body_len);
            if (!body) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); break; }
            if (recv_all(cli, body, hdr.body_len) < 0) { free(body); break; }
        }
        switch (hdr.opcode) {
            case OP_PACK_META:   handle_pack_meta(cli); break;
            case OP_SEG_VERSION: handle_seg_version(cli, body, hdr.body_len); break;
            case OP_SEG_READ:    handle_seg_read(cli, body, hdr.body_len); break;
            case OP_WRITE_SEG:   handle_write_seg(cli, body, hdr.body_len); break;
            default:
                fprintf(stderr, "[srv] unknown opcode %u\n", hdr.opcode);
                send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0);
                break;
        }
        free(body);
    }
}

/* ================================================================
 *  main
 * ================================================================ */

int main(int argc, char **argv) {
    int port = RIO_REMOTE_PORT;
    int truncate_existing = 1;  /* 默认总是重建 shm（确保干净） */
    int opt;
    while ((opt = getopt(argc, argv, "p:ht")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 't': truncate_existing = 0; break;  /* 保留现有 shm（gen_pack.py 写过的） */
            case 'h':
                fprintf(stderr, "usage: %s [-p port] [-t] [-h]\n", argv[0]);
                fprintf(stderr, "  -p port    监听端口 (default %d)\n", RIO_REMOTE_PORT);
                fprintf(stderr, "  -t         保留已有 shm（不 truncate，让 gen_pack.py 写过的数据保留）\n");
                fprintf(stderr, "  -h         帮助\n");
                return 0;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    /* --- 初始化 shm --- */
    printf("[srv] 初始化 %s (%zu MB) 共享内存段...\n",
           RIO_PACK_SHM_NAME_SRV, (size_t)RIO_PACK_SHM_SIZE / (1024 * 1024));
    if (init_shm(RIO_PACK_SHM_NAME_SRV, RIO_PACK_SHM_SIZE, truncate_existing) < 0) {
        fprintf(stderr, "[srv] shm 初始化失败，退出\n");
        return 1;
    }

    /* --- TCP 监听 --- */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr   = { .s_addr = htonl(INADDR_ANY) },
        .sin_port   = htons((uint16_t)port)
    };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ls, 16) < 0) { perror("listen"); return 1; }
    printf("[srv] listening on 0.0.0.0:%d  (shm mode, pack in %s)\n",
           port, RIO_PACK_SHM_NAME_SRV);
    fflush(stdout);

    while (1) {
        int cli = accept(ls, NULL, NULL);
        if (cli < 0) { perror("accept"); continue; }
        printf("[srv] client connected (fd=%d)\n", cli); fflush(stdout);
        serve_client(cli);
        printf("[srv] client disconnected\n"); fflush(stdout);
        close(cli);
    }
    return 0;
}
