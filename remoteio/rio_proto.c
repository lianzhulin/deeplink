/*
 * 网络协议层 — TCP 客户端
 *
 * 使用单连接 + 全局互斥，请求-响应严格串行。
 * 断线自动重连一次，再失败返回 -1。
 */

#define _GNU_SOURCE
#include "remoteio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static int g_sock = -1;
static pthread_mutex_t g_net_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- 基础读写 ---------- */
static ssize_t rio_send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, p + sent, n - sent, 0);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return (ssize_t)sent;
}

static ssize_t rio_recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* ---------- 连接 ---------- */

/*
 * 内部版本（假设 g_net_lock 已由调用者持有）。
 * 避免 rio_call() 内调 rio_connect() 时双重 lock 死锁。
 */
static int rio_connect_locked(void) {
    if (g_sock >= 0) return 0;

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", RIO_REMOTE_PORT);

    if (getaddrinfo(RIO_REMOTE_HOST, portstr, &hints, &res) != 0) {
        RIO_LOG("getaddrinfo failed: %s", gai_strerror(errno));
        return -1;
    }

    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        int s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) {
            g_sock = s;
            break;
        }
        close(s);
    }
    freeaddrinfo(res);

    if (g_sock < 0) {
        RIO_LOG("connect %s:%d failed: %s", RIO_REMOTE_HOST, RIO_REMOTE_PORT, strerror(errno));
        return -1;
    }
    RIO_LOG("connected to %s:%d", RIO_REMOTE_HOST, RIO_REMOTE_PORT);
    return 0;
}

int rio_connect(void) {
    pthread_mutex_lock(&g_net_lock);
    int rc = rio_connect_locked();
    pthread_mutex_unlock(&g_net_lock);
    return rc;
}

void rio_disconnect(void) {
    pthread_mutex_lock(&g_net_lock);
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
    pthread_mutex_unlock(&g_net_lock);
}

/* 响应 body 内的 ret_val 固定 8B，带数据时追加在后面 */

/* 发送请求 + 解析响应头。out_body_cap 是缓冲区容量；如果响应带 body（如 read），存进 out_body */
static int rio_call(uint32_t opcode, const void *req_body, uint32_t req_len,
                    void *out_body, uint32_t out_body_cap,
                    uint64_t *ret_val_out, uint32_t *resp_body_len) {
    pthread_mutex_lock(&g_net_lock);

    /* 确保连接 —— 调 locked 版本，避免双重 lock */
    if (g_sock < 0) { rio_connect_locked(); }
    if (g_sock < 0) { pthread_mutex_unlock(&g_net_lock); return -1; }

    /* 发请求 */
    rio_hdr_t hdr = { .magic = RIO_MAGIC, .opcode = opcode, .body_len = req_len };
    RIO_LOG("rio_call send: op=%u body_len=%u sock=%d", opcode, req_len, g_sock);
    if (rio_send_all(g_sock, &hdr, sizeof(hdr)) < 0 ||
        (req_len && rio_send_all(g_sock, req_body, req_len) < 0)) {
        /* 断线，标记让下次重连 */
        close(g_sock); g_sock = -1;
        pthread_mutex_unlock(&g_net_lock);
        return -1;
    }

    /* 收响应头 */
    rio_hdr_t rhdr;
    if (rio_recv_all(g_sock, &rhdr, sizeof(rhdr)) < 0) {
        RIO_LOG("rio_call recv hdr failed");
        close(g_sock); g_sock = -1;
        pthread_mutex_unlock(&g_net_lock);
        return -1;
    }
    RIO_LOG("rio_call resp: magic=%#x op=%u body_len=%u (expect magic=%#x)",
            rhdr.magic, rhdr.opcode, rhdr.body_len, RIO_MAGIC);
    if (rhdr.magic != RIO_MAGIC) {
        close(g_sock); g_sock = -1;
        pthread_mutex_unlock(&g_net_lock);
        return -1;
    }

    int err = (rhdr.opcode == OP_ERR);

    /* 收响应 body 里的 ret_val (固定 8B) */
    uint64_t rv = 0;
    if (rhdr.body_len >= 8) {
        if (rio_recv_all(g_sock, &rv, 8) < 0) {
            close(g_sock); g_sock = -1;
            pthread_mutex_unlock(&g_net_lock);
            return -1;
        }
    }

    /* 读请求的特殊处理：响应剩余 body 是数据 */
    if (rhdr.body_len > 8 && out_body && out_body_cap > 0) {
        uint32_t datalen = rhdr.body_len - 8;
        uint32_t to_copy = datalen < out_body_cap ? datalen : out_body_cap;
        if (rio_recv_all(g_sock, out_body, to_copy) < 0) {
            close(g_sock); g_sock = -1;
            pthread_mutex_unlock(&g_net_lock);
            return -1;
        }
        if (resp_body_len) *resp_body_len = datalen;
        /* 多余的数据直接丢 */
        if (to_copy < datalen) {
            uint8_t junk[256];
            uint32_t discard = datalen - to_copy;
            while (discard > 0) {
                uint32_t n = discard < sizeof(junk) ? discard : sizeof(junk);
                rio_recv_all(g_sock, junk, n);
                discard -= n;
            }
        }
    }

    pthread_mutex_unlock(&g_net_lock);

    if (ret_val_out) *ret_val_out = rv;
    return err ? -1 : 0;
}

/* ---------- 各操作的便捷封装 ---------- */
int rio_do_open(const char *path, int flags, mode_t mode, remote_handle_t *out) {
    uint32_t plen = (uint32_t)strlen(path) + 1;
    uint32_t body_len = 4 + plen + 4 + 4;
    uint8_t *body = (uint8_t *)malloc(body_len);
    if (!body) return -1;
    uint32_t off = 0;
    memcpy(body + off, &plen, 4); off += 4;
    memcpy(body + off, path, plen); off += plen;
    uint32_t f = (uint32_t)flags;  memcpy(body + off, &f, 4); off += 4;
    uint32_t m = (uint32_t)mode;   memcpy(body + off, &m, 4);
    uint64_t rv = 0;
    int rc = rio_call(OP_OPEN, body, body_len, NULL, 0, &rv, NULL);
    free(body);
    if (rc < 0 || rv == (uint64_t)(uint32_t)-1) return -1;
    if (out) *out = (remote_handle_t)rv;
    return 0;
}

int rio_do_openat(int dirfd, const char *path, int flags, mode_t mode, remote_handle_t *out) {
    uint32_t plen = (uint32_t)strlen(path) + 1;
    uint32_t body_len = 4 + 4 + plen + 4 + 4;
    uint8_t *body = (uint8_t *)malloc(body_len);
    if (!body) return -1;
    uint32_t off = 0;
    int32_t d = dirfd; memcpy(body + off, &d, 4); off += 4;
    memcpy(body + off, &plen, 4); off += 4;
    memcpy(body + off, path, plen); off += plen;
    uint32_t f = (uint32_t)flags;  memcpy(body + off, &f, 4); off += 4;
    uint32_t m = (uint32_t)mode;   memcpy(body + off, &m, 4);
    uint64_t rv = 0;
    int rc = rio_call(OP_OPENAT, body, body_len, NULL, 0, &rv, NULL);
    free(body);
    if (rc < 0 || rv == (uint64_t)(uint32_t)-1) return -1;
    if (out) *out = (remote_handle_t)rv;
    return 0;
}

ssize_t rio_do_read(remote_handle_t rh, void *buf, size_t count) {
    uint32_t body[2] = { rh, (uint32_t)count };
    uint64_t rv = 0;
    uint32_t datalen = 0;
    int rc = rio_call(OP_READ, body, sizeof(body), buf, (uint32_t)count, &rv, &datalen);
    if (rc < 0) return -1;
    /* ret_val 就是真正读到的字节数（或 -1=错误） */
    return (ssize_t)(int64_t)rv;
}

ssize_t rio_do_write(remote_handle_t rh, const void *buf, size_t count) {
    uint32_t body_len = 4 + 4 + (uint32_t)count;
    uint8_t *body = (uint8_t *)malloc(body_len);
    if (!body) return -1;
    uint32_t off = 0;
    uint32_t rh32 = rh; memcpy(body + off, &rh32, 4); off += 4;
    uint32_t c32  = (uint32_t)count; memcpy(body + off, &c32, 4); off += 4;
    if (count) memcpy(body + off, buf, count);
    uint64_t rv = 0;
    int rc = rio_call(OP_WRITE, body, body_len, NULL, 0, &rv, NULL);
    free(body);
    if (rc < 0) return -1;
    return (ssize_t)(int64_t)rv;
}

int rio_do_close(remote_handle_t rh) {
    uint32_t rh32 = rh;
    uint64_t rv = 0;
    int rc = rio_call(OP_CLOSE, &rh32, 4, NULL, 0, &rv, NULL);
    if (rc < 0) return -1;
    return (int)(int64_t)rv;
}

off_t rio_do_lseek(remote_handle_t rh, off_t offset, int whence) {
    uint8_t body[4 + 8 + 4];
    uint32_t rh32 = rh;  memcpy(body + 0, &rh32, 4);
    int64_t off64 = offset; memcpy(body + 4, &off64, 8);
    int32_t w = whence;  memcpy(body + 12, &w, 4);
    uint64_t rv = 0;
    int rc = rio_call(OP_LSEEK, body, sizeof(body), NULL, 0, &rv, NULL);
    if (rc < 0) return (off_t)-1;
    return (off_t)(int64_t)rv;
}

/* ---------- 按需拉取 + 版本化 新协议 ---------- */

/*
 * OP_PACK_META: 请求空 body → 服务端返回 header+entries+versions[] 打包在响应 body 里
 */
int rio_do_pack_meta(uint8_t *out_meta_buf, uint32_t out_meta_cap, uint32_t *out_meta_len) {
    uint64_t rv = 0;
    uint32_t got = 0;
    int rc = rio_call(OP_PACK_META, NULL, 0, out_meta_buf, out_meta_cap, &rv, &got);
    if (rc < 0) return -1;
    if (out_meta_len) *out_meta_len = got;
    RIO_LOG("rio_do_pack_meta: got %u bytes", got);
    return 0;
}

/*
 * OP_SEG_VERSION: 只查询某段 version（不拉 body，极轻量）
 * body: [4B seg_idx]
 * 响应 body: [8B version]
 */
int rio_do_seg_version(uint32_t seg_idx, uint32_t *out_version) {
    uint32_t body = seg_idx;
    uint64_t rv = 0;
    int rc = rio_call(OP_SEG_VERSION, &body, sizeof(body), NULL, 0, &rv, NULL);
    if (rc < 0) return -1;
    if (out_version) *out_version = (uint32_t)rv;
    return 0;
}

/*
 * OP_SEG_READ: 拉某段完整 body
 * body: [4B seg_idx]
 * 响应: [8B version][seg_size 字节 body]
 * ret_val 里是 version，额外 body 是 seg 数据
 */
int rio_do_seg_read(uint32_t seg_idx, uint32_t seg_size,
                    uint8_t *out_buf, uint32_t out_cap, uint32_t *out_version) {
    uint32_t body = seg_idx;
    uint64_t rv = 0;
    uint32_t got = 0;
    int rc = rio_call(OP_SEG_READ, &body, sizeof(body), out_buf, out_cap, &rv, &got);
    if (rc < 0) return -1;
    /* got 应该等于 seg_size（响应 body 就是 seg 数据本身）*/
    if (got != seg_size) {
        RIO_LOG("rio_do_seg_read(seg=%u): got %u want %u", seg_idx, got, seg_size);
        return -1;
    }
    if (out_version) *out_version = (uint32_t)rv;
    RIO_LOG("rio_do_seg_read(seg=%u): %u bytes version=%u", seg_idx, got, (uint32_t)rv);
    return 0;
}

/*
 * OP_WRITE_SEG: 穿透写某段到服务端（服务端 version++）
 * body: [4B seg_idx][4B data_len][data...]
 */
int rio_do_write_seg(uint32_t seg_idx, const uint8_t *data, uint32_t data_len) {
    uint32_t body_len = 4 + 4 + data_len;
    uint8_t *body = (uint8_t *)malloc(body_len);
    if (!body) return -1;
    uint32_t off = 0;
    uint32_t s32 = seg_idx;    memcpy(body + off, &s32, 4); off += 4;
    uint32_t d32 = data_len;   memcpy(body + off, &d32, 4); off += 4;
    if (data_len) memcpy(body + off, data, data_len);

    uint64_t rv = 0;
    int rc = rio_call(OP_WRITE_SEG, body, body_len, NULL, 0, &rv, NULL);
    free(body);
    if (rc < 0) return -1;
    /* ret_val 应该是 0 成功 */
    return (int)rv;
}
