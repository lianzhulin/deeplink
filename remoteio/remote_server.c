/*
 * remote_server.c — 远程文件服务端
 *
 * 监听 TCP 端口（默认 RIO_REMOTE_PORT=9527），按照 remoteio.h 里的协议
 * 处理 open/read/write/close/lseek 请求，把结果在服务器本地文件系统上执行。
 *
 * 为了简单，单线程迭代处理，但多客户端连接会串行 —— 对 demo 足够。
 * 如需并发，把 process_client 扔进 pthread 即可。
 */

#define _GNU_SOURCE
#include "remoteio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>

#define MAX_REMOTE_FDS 4096
static int g_handles[MAX_REMOTE_FDS];   /* 服务端 fd 表：0 = 空闲 */

/*
 * 服务端本地根目录 —— 客户端的 /remote/xxx 会映射到这个目录下的 xxx。
 * 通过 -r 指定，默认 ./rio_root。
 */
static const char *g_root = "./rio_root";

/* 把客户端发来的虚拟路径转换成服务端本地真实路径。
 * 例如: /remote/foo/bar.txt -> ./rio_root/foo/bar.txt
 * 如果路径不以 RIO_REMOTE_PREFIX 开头，则原样返回（允许绝对路径直接访问）。
 * 返回的是新分配的字符串，调用方负责 free。 */
static char *map_path(const char *virt) {
    if (!virt) return NULL;
    static const char *prefix = RIO_REMOTE_PREFIX;   /* "/remote/" */
    size_t pl = strlen(prefix);
    const char *rel;
    if (strncmp(virt, prefix, pl) == 0) {
        rel = virt + pl;   /* 去掉前缀 */
    } else {
        rel = virt;        /* 直接用 */
    }
    /* 拼接 g_root + "/" + rel */
    size_t rl = strlen(g_root);
    size_t rl2 = strlen(rel);
    char *out = (char *)malloc(rl + 1 + rl2 + 1);
    if (!out) return NULL;
    snprintf(out, rl + 1 + rl2 + 1, "%s/%s", g_root, rel);
    return out;
}

static int alloc_handle(void) {
    for (int i = 1; i < MAX_REMOTE_FDS; i++) {
        if (g_handles[i] == 0) return i;
    }
    return -1;
}

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

static void handle_open(int cli, const uint8_t *body, uint32_t len) {
    uint32_t off = 0;
    uint32_t plen; memcpy(&plen, body + off, 4); off += 4;
    if (plen > len - 8) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    char *virt = (char *)malloc(plen);
    memcpy(virt, body + off, plen); off += plen;
    uint32_t flags; memcpy(&flags, body + off, 4); off += 4;
    uint32_t mode;  memcpy(&mode,  body + off, 4); off += 4;

    /* 虚拟路径 → 服务端本地真实路径 */
    char *real = map_path(virt);
    if (!real) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); free(virt); return; }

    int lh = open(real, (int)flags, (mode_t)mode);
    if (lh < 0) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); free(virt); free(real); return; }
    int rh = alloc_handle();
    if (rh < 0) { close(lh); send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); free(virt); free(real); return; }
    g_handles[rh] = lh;
    printf("[srv] open  %s -> %s (rh=%d lh=%d)\n", virt, real, rh, lh); fflush(stdout);
    send_resp(cli, OP_ACK, (uint64_t)rh, NULL, 0);
    free(virt); free(real);
}

static void handle_openat(int cli, const uint8_t *body, uint32_t len) {
    uint32_t off = 0;
    int32_t dirfd; memcpy(&dirfd, body + off, 4); off += 4;
    uint32_t plen; memcpy(&plen, body + off, 4); off += 4;
    char *virt = (char *)malloc(plen);
    memcpy(virt, body + off, plen); off += plen;
    uint32_t flags; memcpy(&flags, body + off, 4); off += 4;
    uint32_t mode;  memcpy(&mode,  body + off, 4); off += 4;

    char *real = map_path(virt);
    if (!real) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); free(virt); return; }

    int real_dirfd = (dirfd >= 0 && dirfd < MAX_REMOTE_FDS && g_handles[dirfd] != 0)
                     ? g_handles[dirfd] : (int)dirfd;
    int lh = openat(real_dirfd, real, (int)flags, (mode_t)mode);
    if (lh < 0) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); free(virt); free(real); return; }
    int rh = alloc_handle();
    if (rh < 0) { close(lh); send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); free(virt); free(real); return; }
    g_handles[rh] = lh;
    printf("[srv] openat dirfd=%d %s -> %s (rh=%d lh=%d)\n", real_dirfd, virt, real, rh, lh); fflush(stdout);
    send_resp(cli, OP_ACK, (uint64_t)rh, NULL, 0);
    free(virt); free(real);
}

static void handle_read(int cli, const uint8_t *body, uint32_t len) {
    uint32_t rh, count;
    memcpy(&rh, body + 0, 4);
    memcpy(&count, body + 4, 4);
    if (rh == 0 || rh >= MAX_REMOTE_FDS || g_handles[rh] == 0) {
        send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return;
    }
    uint8_t *buf = (uint8_t *)malloc(count > 0 ? count : 1);
    ssize_t n = read(g_handles[rh], buf, count);
    if (n < 0) { free(buf); send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    uint64_t ret = (uint64_t)(int64_t)n;
    send_resp(cli, OP_READ, ret, buf, (uint32_t)n);
    free(buf);
}

static void handle_write(int cli, const uint8_t *body, uint32_t len) {
    uint32_t rh, count;
    memcpy(&rh, body + 0, 4);
    memcpy(&count, body + 4, 4);
    if (rh == 0 || rh >= MAX_REMOTE_FDS || g_handles[rh] == 0) {
        send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return;
    }
    if (4 + 4 + count > len) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    const uint8_t *data = body + 8;
    ssize_t n = write(g_handles[rh], data, count);
    if (n < 0) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    send_resp(cli, OP_ACK, (uint64_t)(int64_t)n, NULL, 0);
}

static void handle_close(int cli, const uint8_t *body) {
    uint32_t rh; memcpy(&rh, body, 4);
    if (rh == 0 || rh >= MAX_REMOTE_FDS || g_handles[rh] == 0) {
        send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return;
    }
    int rc = close(g_handles[rh]);
    g_handles[rh] = 0;
    printf("[srv] close rh=%d lh=%d\n", rh, rc >= 0 ? rc : -1); fflush(stdout);
    send_resp(cli, rc == 0 ? OP_ACK : OP_ERR, (uint64_t)rc, NULL, 0);
}

static void handle_lseek(int cli, const uint8_t *body) {
    uint32_t rh;     memcpy(&rh,     body + 0,  4);
    int64_t  offset; memcpy(&offset, body + 4,  8);
    int32_t  whence; memcpy(&whence, body + 12, 4);
    if (rh == 0 || rh >= MAX_REMOTE_FDS || g_handles[rh] == 0) {
        send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return;
    }
    off_t r = lseek(g_handles[rh], (off_t)offset, (int)whence);
    if (r == (off_t)-1) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); return; }
    send_resp(cli, OP_ACK, (uint64_t)(int64_t)r, NULL, 0);
}

/* ---------- 主循环 ---------- */
static void serve_client(int cli) {
    rio_hdr_t hdr;
    while (1) {
        if (recv_all(cli, &hdr, sizeof(hdr)) < 0) break;
        if (hdr.magic != RIO_MAGIC) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); break; }
        uint8_t *body = NULL;
        if (hdr.body_len) {
            body = (uint8_t *)malloc(hdr.body_len);
            if (!body) { send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); break; }
            if (recv_all(cli, body, hdr.body_len) < 0) { free(body); break; }
        }
        switch (hdr.opcode) {
            case OP_OPEN:   handle_open(cli, body, hdr.body_len); break;
            case OP_OPENAT: handle_openat(cli, body, hdr.body_len); break;
            case OP_READ:   handle_read(cli, body, hdr.body_len); break;
            case OP_WRITE:  handle_write(cli, body, hdr.body_len); break;
            case OP_CLOSE:  handle_close(cli, body); break;
            case OP_LSEEK:  handle_lseek(cli, body); break;
            default: send_resp(cli, OP_ERR, (uint64_t)-1, NULL, 0); break;
        }
        free(body);
    }
}

int main(int argc, char **argv) {
    int port = RIO_REMOTE_PORT;
    int opt;
    while ((opt = getopt(argc, argv, "p:r:h")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'r': g_root = optarg; break;
            case 'h':
                fprintf(stderr, "usage: %s [-p port] [-r root_dir]\n", argv[0]);
                fprintf(stderr, "  -p port    监听端口 (default %d)\n", RIO_REMOTE_PORT);
                fprintf(stderr, "  -r root    虚拟路径 /remote/* 映射到的本地根目录 (default ./rio_root)\n");
                return 0;
        }
    }

    memset(g_handles, 0, sizeof(g_handles));
    signal(SIGPIPE, SIG_IGN);

    /* 确保 root 目录存在 */
    if (mkdir(g_root, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "[srv] mkdir %s: %s\n", g_root, strerror(errno));
        return 1;
    }

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_addr   = { .s_addr = htonl(INADDR_ANY) },
                                .sin_port   = htons((uint16_t)port) };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(ls, 16) < 0) { perror("listen"); return 1; }
    printf("[srv] listening on 0.0.0.0:%d  root=%s\n", port, g_root); fflush(stdout);

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
