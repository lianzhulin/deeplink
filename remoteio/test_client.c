/*
 * test_client.c — 端到端测试
 *
 * 这个程序做一系列文件操作，同时覆盖：
 *   1. 本地文件（/tmp/remoteio_test_xxx） — 正常走系统
 *   2. 远程文件（/remote/remoteio_test_xxx） — 被 LD_PRELOAD 劫持到网络
 *
 * 对比两边操作结果是否一致。程序本身不包含任何远程逻辑 —— 完全依赖 LD_PRELOAD 层自动区分。
 *
 * 运行：
 *   LD_PRELOAD=./libremoteio.so ./test_client
 *   （先启动 remote_server）
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define LOCAL_PATH  "/tmp/remoteio_test_local.bin"
#define REMOTE_PATH "/remote/remoteio_test_remote.bin"

static int test_passed = 0;
static int test_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL: %s  (%s)\n", msg, strerror(errno)); test_failed++; } \
    else         { fprintf(stderr, "  PASS: %s\n", msg); test_passed++; } \
} while (0)

int main(void) {
    const char *path_local  = LOCAL_PATH;
    const char *path_remote = REMOTE_PATH;

    /* 清理旧文件 */
    unlink(path_local);
    unlink(path_remote);  /* 不存在就忽略 */

    printf("=== 1. open + write + close ===\n");
    int fd_l = open(path_local,  O_CREAT | O_TRUNC | O_WRONLY, 0644);
    int fd_r = open(path_remote, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CHECK(fd_l >= 0,   "local open");
    CHECK(fd_r >= 0,   "remote open (LD_PRELOAD 劫持)");

    const char *hello = "Hello, Remote IO!\n";
    ssize_t hl = write(fd_l, hello, strlen(hello));
    ssize_t hr = write(fd_r, hello, strlen(hello));
    CHECK(hl == (ssize_t)strlen(hello), "local write");
    CHECK(hr == (ssize_t)strlen(hello), "remote write");
    close(fd_l); close(fd_r);

    printf("=== 2. open + read ===\n");
    fd_l = open(path_local,  O_RDONLY);
    fd_r = open(path_remote, O_RDONLY);
    CHECK(fd_l >= 0, "local reopen");
    CHECK(fd_r >= 0, "remote reopen");

    char buf_l[64] = {0}, buf_r[64] = {0};
    ssize_t rl = read(fd_l, buf_l, sizeof(buf_l) - 1);
    ssize_t rr = read(fd_r, buf_r, sizeof(buf_r) - 1);
    CHECK(rl == rr,              "read 长度一致");
    CHECK(strcmp(buf_l, buf_r) == 0, "read 内容一致");
    CHECK(strcmp(buf_l, hello) == 0, "read 内容正确");
    close(fd_l); close(fd_r);

    printf("=== 3. lseek + write ===\n");
    fd_l = open(path_local,  O_RDWR);
    fd_r = open(path_remote, O_RDWR);
    CHECK(fd_l >= 0, "local open rw");
    CHECK(fd_r >= 0, "remote open rw");

    const char *tail = " (appended via lseek)\n";
    off_t ll = lseek(fd_l, 0, SEEK_END);
    off_t lr = lseek(fd_r, 0, SEEK_END);
    CHECK(ll > 0 && lr > 0, "lseek SEEK_END");
    ssize_t wl = write(fd_l, tail, strlen(tail));
    ssize_t wr = write(fd_r, tail, strlen(tail));
    CHECK(wl == (ssize_t)strlen(tail), "local write at end");
    CHECK(wr == (ssize_t)strlen(tail), "remote write at end");
    close(fd_l); close(fd_r);

    printf("=== 4. fopen + fwrite + fread (FILE* 层) ===\n");
    FILE *fl = fopen(path_local,  "rb+");
    FILE *fr = fopen(path_remote, "rb+");
    CHECK(fl != NULL, "local fopen");
    CHECK(fr != NULL, "remote fopen");
    fseek(fl, 0, SEEK_SET);
    fseek(fr, 0, SEEK_SET);
    fread(buf_l, 1, sizeof(buf_l) - 1, fl);
    fread(buf_r, 1, sizeof(buf_r) - 1, fr);
    CHECK(memcmp(buf_l, buf_r, rl + wl) == 0, "FILE* fread 内容一致");
    fclose(fl); fclose(fr);

    printf("=== 5. cleanup ===\n");
    unlink(path_local);
    int ur = unlink(path_remote);
    /* 远端 unlink 不被劫持（没劫持 unlink），这里可能失败 —— 忽略 */
    fprintf(stderr, "  (remote unlink: %s)\n", ur < 0 ? strerror(errno) : "ok");

    printf("\n=== Summary: %d passed, %d failed ===\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
