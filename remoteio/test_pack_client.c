/*
 * test_pack_client.c — pack 多虚拟文件 → 单远程大文件 测试
 *
 * gen_pack.py 生成的 pack.bin 段内容（精确对齐）：
 *   seg1 config.txt  offset=0,  size=64: "=== config.txt: hello pack ===\n" (31) + 33*X
 *   seg2 header.bin  offset=64, size=128: "HEADER_BIN_MAGIC\x00\x01\x02\x03\xde\xad\xbe\xef" (24) + 104*H
 *   seg3 records.log offset=192,size=256: "records line 1\nrecords line 2\nrecords line 3\n" (45) + 211*R
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int tp = 0, tf = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL: %s  (%s)\n", msg, strerror(errno)); tf++; } \
    else         { fprintf(stderr, "  PASS: %s\n", msg); tp++; } \
} while (0)

#define V_CONFIG_TXT  "/virtual/config.txt"
#define V_HEADER_BIN  "/virtual/header.bin"
#define V_RECORDS_LOG "/virtual/records.log"

#define SEG1_ORIG     "=== config.txt: hello pack ===\n"   /* 31 bytes */
#define SEG2_ORIG     "HEADER_BIN_MAGIC"                   /* 16 bytes */
#define SEG3_ORIG     "records line 1\nrecords line 2\nrecords line 3\n" /* 45 bytes */

static void test_read_three_files(void) {
    printf("\n=== 1. 并发 open + read 三个虚拟文件 ===\n");

    int fd_c = open(V_CONFIG_TXT, O_RDONLY);
    int fd_h = open(V_HEADER_BIN, O_RDONLY);
    int fd_r = open(V_RECORDS_LOG, O_RDONLY);
    CHECK(fd_c >= 0, "open config.txt");
    CHECK(fd_h >= 0, "open header.bin");
    CHECK(fd_r >= 0, "open records.log");

    char buf[512];

    memset(buf, 0, sizeof(buf));
    ssize_t nc = read(fd_c, buf, sizeof(buf));   /* 读整个 seg1 (最多 64) */
    CHECK(nc == 64, "read config.txt → 64 bytes (seg1 全段)");
    CHECK(memcmp(buf, SEG1_ORIG, strlen(SEG1_ORIG)) == 0, "config.txt seg1 原始内容正确");
    /* seg1 结尾是 \n，后面跟 'X' 填充 */
    CHECK(buf[strlen(SEG1_ORIG)-1] == '\n' && buf[strlen(SEG1_ORIG)] == 'X',
          "config.txt 内容 + 填充 'X' 正确");

    memset(buf, 0, sizeof(buf));
    ssize_t nh = read(fd_h, buf, sizeof(buf));   /* 读整个 seg2 (最多 128) */
    CHECK(nh == 128, "read header.bin → 128 bytes (seg2 全段)");
    CHECK(memcmp(buf, SEG2_ORIG, strlen(SEG2_ORIG)) == 0, "header.bin seg2 原始内容正确");

    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(fd_r, buf, sizeof(buf));   /* 读整个 seg3 (最多 256) */
    CHECK(nr == 256, "read records.log → 256 bytes (seg3 全段)");
    CHECK(memcmp(buf, SEG3_ORIG, strlen(SEG3_ORIG)) == 0, "records.log seg3 原始内容正确");

    /* 关闭前再读一次头部，确认互不干扰 */
    lseek(fd_c, 0, SEEK_SET); lseek(fd_h, 0, SEEK_SET); lseek(fd_r, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    read(fd_c, buf, 10);
    CHECK(strncmp(buf, "=== config", 10) == 0, "交叉读 seg1 没被污染");
    memset(buf, 0, sizeof(buf));
    read(fd_h, buf, 10);
    CHECK(strncmp(buf, "HEADER_BIN", 10) == 0, "交叉读 seg2 没被污染");
    memset(buf, 0, sizeof(buf));
    read(fd_r, buf, 10);
    CHECK(strncmp(buf, "records li", 10) == 0, "交叉读 seg3 没被污染");

    close(fd_c); close(fd_h); close(fd_r);
}

static void test_lseek_independent(void) {
    printf("\n=== 2. lseek 独立定位 + read ===\n");

    int fd_c = open(V_CONFIG_TXT, O_RDONLY);
    int fd_r = open(V_RECORDS_LOG, O_RDONLY);
    CHECK(fd_c >= 0 && fd_r >= 0, "open 成功");

    /* seg1: === config.txt: hello pack ===\nXXX...
     *       0123456789...     1111111111222
     *       0         1         2         3 */

    /* 在 config.txt 里从 offset=16 (hello 的 'h') 读 12 bytes → "hello pack ==" */
    off_t rc = lseek(fd_c, 16, SEEK_SET);
    CHECK(rc == 16, "lseek config.txt → 16");
    char buf[32]; memset(buf, 0, sizeof(buf));
    ssize_t nc = read(fd_c, buf, 12);
    CHECK(nc == 12 && memcmp(buf, "hello pack ==", 12) == 0,
          "config.txt lseek+read 正确 (offset 16 读 12)");

    /* records.log 独立 */
    /* seg3: records line 1\nrecords line 2\nrecords line 3\nRRR...
     *       15 chars per line  */
    /* lseek 到 offset=15 跳过第一行，读 15 应该是 "records line 2\n" */
    off_t rr = lseek(fd_r, 15, SEEK_SET);
    CHECK(rr == 15, "lseek records.log → 15");
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(fd_r, buf, 15);
    CHECK(nr == 15 && memcmp(buf, "records line 2\n", 15) == 0,
          "records.log lseek+read 独立 (offset 15 读 15)");

    /* 现在 config.txt pack_pos 应该是 16+12=28，records.log 应该是 15+15=30
     * 但它们各自独立 track —— lseek+read 互不影响 */
    off_t pc = lseek(fd_c, 0, SEEK_CUR);
    off_t pr = lseek(fd_r, 0, SEEK_CUR);
    CHECK(pc == 28, "config.txt pack_pos == 28 (16+12)");
    CHECK(pr == 30, "records.log pack_pos == 30 (15+15)");

    close(fd_c); close(fd_r);
}

static void test_boundary_eof(void) {
    printf("\n=== 3. pack_size 边界 EOF 测试 ===\n");

    int fd = open(V_CONFIG_TXT, O_RDONLY);  /* size=64 */
    CHECK(fd >= 0, "open config.txt");

    off_t r = lseek(fd, 64, SEEK_SET);
    CHECK(r == 64, "lseek → pack_size=64");
    char buf[16];
    ssize_t n = read(fd, buf, sizeof(buf));
    CHECK(n == 0, "read 到 pack_size 边界返回 0 (EOF)");

    lseek(fd, 60, SEEK_SET);
    n = read(fd, buf, 16);
    CHECK(n == 4, "read 跨 pack_size 边界被 clamp (64-60=4)");

    close(fd);
}

static void test_write_pack_segment(void) {
    printf("\n=== 4. write pack 段 + 重新打开读出 ===\n");

    int fd = open(V_HEADER_BIN, O_RDWR);
    CHECK(fd >= 0, "open header.bin 可读写");

    /* seg2 原始: "HEADER_BIN_MAGIC\x00\x01\x02\x03\xde\xad\xbe\xef" + H*104 */
    /* offset 0~15 = HEADER_BIN_MAGIC */
    /* offset 16~23 = \x00\x01\x02\x03\xde\xad\xbe\xef */

    const char *tag = "PACK_WRITE_TEST_V2";   /* 18 bytes */
    off_t r = lseek(fd, 10, SEEK_SET);
    CHECK(r == 10, "lseek → 10");
    ssize_t w = write(fd, tag, strlen(tag));
    CHECK(w == (ssize_t)strlen(tag), "write 18 bytes → header.bin offset=10");
    close(fd);

    fd = open(V_HEADER_BIN, O_RDONLY);
    char buf[128]; memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, 128);
    CHECK(n == 128, "reopen 读 128 bytes");
    /* offset 0~9 原始: "HEADER_BIN" */
    CHECK(strncmp(buf, "HEADER_BIN", 10) == 0, "offset 0~9 未被覆盖");
    /* offset 10+ 被我们覆盖 */
    CHECK(strncmp(buf + 10, "PACK_WRITE_TEST_V2", 18) == 0,
          "offset 10+ 写入持久化成功");
    close(fd);

    /* 确认 config.txt 未被影响 */
    int fd2 = open(V_CONFIG_TXT, O_RDONLY);
    char buf2[64]; memset(buf2, 0, sizeof(buf2));
    read(fd2, buf2, 31);
    CHECK(memcmp(buf2, SEG1_ORIG, 31) == 0,
          "写入 header.bin 不影响 config.txt (seg1 原始内容完整)");
    close(fd2);
}

static void test_pread_pwrite(void) {
    printf("\n=== 5. pread + pwrite pack 模式 ===\n");

    int fd = open(V_RECORDS_LOG, O_RDWR);
    CHECK(fd >= 0, "open records.log");

    const char *mark = "[MARK]";

    /* pwrite 到 records.log offset=30 —— 直接定位，不影响 pack_pos */
    ssize_t pw = pwrite(fd, mark, strlen(mark), 30);
    CHECK(pw == (ssize_t)strlen(mark), "pwrite offset=30 成功");

    /* pread 从 offset=14 读 30 bytes —— 直接定位 */
    char buf[64]; memset(buf, 0, sizeof(buf));
    ssize_t pr = pread(fd, buf, 30, 14);
    CHECK(pr == 30, "pread offset=14 → 30 bytes");
    /* records.log: 每行 15 chars
     * offset 14 = 第一行最后一个字符 '\n'
     * offset 15~29 = 第二行 "records line 2\n" (15 bytes)
     * offset 30~44 = 第三行，但 offset 30 被 [MARK] 覆盖 */
    /* pread 30 bytes: offset 14~43
     * buf[0] = '\n' (offset 14)
     * buf[1..15] = "records line 2\n" (offset 15~29)
     * buf[16..21] = "[MARK]" (我们写入，offset 30~35)
     * buf[22..29] = 原始第三行剩下的部分 (offset 36~43) */
    CHECK(memcmp(buf + 16, mark, 6) == 0,
          "pread 看到 pwrite 写入的 [MARK]");

    close(fd);
}

static void test_fopen_pack(void) {
    printf("\n=== 6. fopen + fread pack 虚拟路径 (FILE* 层) ===\n");

    FILE *fp = fopen(V_CONFIG_TXT, "rb");
    CHECK(fp != NULL, "fopen /virtual/config.txt");

    /* seg1 原始内容 "=== config.txt: hello pack ===\n" (31 bytes) + 33 X */
    char buf[128]; memset(buf, 0, sizeof(buf));
    size_t n = fread(buf, 1, 64, fp);
    CHECK(n == 64, "fread 64 bytes (seg1 全段)");
    CHECK(memcmp(buf, SEG1_ORIG, strlen(SEG1_ORIG)) == 0,
          "fread seg1 原始内容正确");

    /* fseek 到 16 + fread 12 → "hello pack ==" */
    int r = fseek(fp, 16, SEEK_SET);
    CHECK(r == 0, "fseek → 16");
    memset(buf, 0, sizeof(buf));
    n = fread(buf, 1, 12, fp);
    CHECK(n == 12, "fread 12 bytes");
    CHECK(memcmp(buf, "hello pack ==", 12) == 0, "fseek + fread 内容正确");

    fclose(fp);
}

static void test_independent_pos(void) {
    printf("\n=== 7. 两次 open 同一虚拟文件，pack_pos 独立 ===\n");

    int fd1 = open(V_CONFIG_TXT, O_RDONLY);
    int fd2 = open(V_CONFIG_TXT, O_RDONLY);
    CHECK(fd1 >= 0 && fd2 >= 0, "两次 open 同一虚拟文件");

    char b1[32], b2[32];
    memset(b1, 0, sizeof(b1)); memset(b2, 0, sizeof(b2));
    read(fd1, b1, 10);    /* fd1 pos → 10 */
    read(fd2, b2, 20);    /* fd2 pos → 20  (独立!) */

    off_t p1 = lseek(fd1, 0, SEEK_CUR);
    off_t p2 = lseek(fd2, 0, SEEK_CUR);
    CHECK(p1 == 10, "fd1 pack_pos == 10");
    CHECK(p2 == 20, "fd2 pack_pos == 20 (两者独立)");

    close(fd1); close(fd2);
}

int main(void) {
    printf("RIO_PACK_CONFIG=%s\n", getenv("RIO_PACK_CONFIG") ? getenv("RIO_PACK_CONFIG") : "(未设置)");

    test_read_three_files();
    test_lseek_independent();
    test_boundary_eof();
    test_write_pack_segment();
    test_pread_pwrite();
    test_fopen_pack();
    test_independent_pos();

    printf("\n=== Summary: %d passed, %d failed ===\n", tp, tf);
    return tf == 0 ? 0 : 1;
}
