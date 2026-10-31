/*
 * libremoteio.c — LD_PRELOAD 劫持主模块（pack 模式专用）
 *
 * 劫持清单：
 *   系统调用层： open, openat, open64, openat64
 *               read, write, close, lseek, lseek64, pread, pwrite, pread64, pwrite64
 *   标准库层  ： fopen, fopen64, freopen, fdopen, fileno,
 *               fread, fwrite, fclose, fseek, ftell, rewind, fflush
 *
 * 策略：
 *   - constructor 用 dlsym(RTLD_NEXT, ...) 拉 glibc 真实符号
 *   - 对每个被劫持函数：
 *     · 先判断 —— path/fd 是否属于 pack 虚拟文件
 *     · pack 命中 → 翻译为对服务端 pack_file 的带偏移 I/O
 *     · 未命中   → 原样透传给 real_*
 *   - pack 模式下 read/write 每次 lseek(pack_rh, pack_offset + pack_pos)，
 *     lseek 只更新客户端 track 的 pack_pos（不动远程 rh 位置）
 */

#define _GNU_SOURCE
#include "remoteio.h"
#include "rio_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>

/* ================================================================
 *  真实函数指针（constructor 中通过 dlsym(RTLD_NEXT) 初始化）
 * ================================================================ */

/* --- 系统调用层 --- */
typedef int   (*real_open_t)(const char *, int, ...);
typedef int   (*real_openat_t)(int, const char *, int, ...);
typedef int   (*real_open64_t)(const char *, int, ...);
typedef int   (*real_openat64_t)(int, const char *, int, ...);

typedef ssize_t (*real_read_t)(int, void *, size_t);
typedef ssize_t (*real_write_t)(int, const void *, size_t);
typedef int   (*real_close_t)(int);
typedef off_t (*real_lseek_t)(int, off_t, int);
typedef ssize_t (*real_pread_t)(int, void *, size_t, off_t);
typedef ssize_t (*real_pwrite_t)(int, const void *, size_t, off_t);

/* --- stat 家族 --- */
typedef int (*real_fstat_t)(int, struct stat *);
typedef int (*real_newfstatat_t)(int, const char *, struct stat *, int);
typedef int (*real_stat_t)(const char *, struct stat *);
typedef int (*real_lstat_t)(const char *, struct stat *);

/* --- 标准库 FILE* 层 --- */
typedef FILE* (*real_fopen_t)(const char *, const char *);
typedef FILE* (*real_fopen64_t)(const char *, const char *);
typedef FILE* (*real_freopen_t)(const char *, const char *, FILE *);
typedef FILE* (*real_fdopen_t)(int, const char *);
typedef int   (*real_fileno_t)(FILE *);
typedef size_t(*real_fread_t)(void *, size_t, size_t, FILE *);
typedef size_t(*real_fwrite_t)(const void *, size_t, size_t, FILE *);
typedef int   (*real_fclose_t)(FILE *);
typedef int   (*real_fseek_t)(FILE *, long, int);
typedef long  (*real_ftell_t)(FILE *);
typedef int   (*real_rewind_t)(FILE *);
typedef int   (*real_fflush_t)(FILE *);

/* 全局指针 */
static real_open_t    real_open    = NULL;
static real_openat_t  real_openat  = NULL;
static real_open64_t  real_open64  = NULL;
static real_openat64_t real_openat64 = NULL;
static real_read_t    real_read    = NULL;
static real_write_t   real_write   = NULL;
static real_close_t   real_close   = NULL;
static real_lseek_t   real_lseek   = NULL;
static real_pread_t   real_pread   = NULL;
static real_pwrite_t  real_pwrite  = NULL;

static real_fstat_t       real_fstat       = NULL;
static real_newfstatat_t  real_newfstatat  = NULL;
static real_stat_t        real_stat        = NULL;
static real_lstat_t       real_lstat       = NULL;

static real_fopen_t   real_fopen   = NULL;
static real_fopen64_t real_fopen64 = NULL;
static real_freopen_t real_freopen = NULL;
static real_fdopen_t  real_fdopen  = NULL;
static real_fileno_t  real_fileno  = NULL;
static real_fread_t   real_fread   = NULL;
static real_fwrite_t  real_fwrite  = NULL;
static real_fclose_t  real_fclose  = NULL;
static real_fseek_t   real_fseek   = NULL;
static real_ftell_t   real_ftell   = NULL;
static real_rewind_t  real_rewind  = NULL;
static real_fflush_t  real_fflush  = NULL;

/* ================================================================
 *  初始化
 * ================================================================ */

#define RESOLVE(var, symname)  do { \
    if (!(var)) { \
        dlerror(); \
        (var) = (typeof(var)) dlsym(RTLD_NEXT, symname); \
        const char *e = dlerror(); \
        RIO_LOG("dlsym(" symname ") -> %p  err=%s", (void*)(var), e ? e : "(none)"); \
    } \
} while (0)

__attribute__((constructor(65534)))
static void rio_init(void) {
    RESOLVE(real_open, "open");          RESOLVE(real_openat, "openat");
    RESOLVE(real_open64, "open64");      RESOLVE(real_openat64, "openat64");
    RESOLVE(real_read, "read");          RESOLVE(real_write, "write");
    RESOLVE(real_close, "close");        RESOLVE(real_lseek, "lseek");
    RESOLVE(real_pread, "pread");        RESOLVE(real_pwrite, "pwrite");

    RESOLVE(real_fstat, "fstat");                    RESOLVE(real_newfstatat, "newfstatat");
    RESOLVE(real_stat, "stat");                      RESOLVE(real_lstat, "lstat");

    RESOLVE(real_fopen, "fopen");        RESOLVE(real_fopen64, "fopen64");
    RESOLVE(real_freopen, "freopen");    RESOLVE(real_fdopen, "fdopen");
    RESOLVE(real_fileno, "fileno");      RESOLVE(real_fread, "fread");
    RESOLVE(real_fwrite, "fwrite");      RESOLVE(real_fclose, "fclose");
    RESOLVE(real_fseek, "fseek");        RESOLVE(real_ftell, "ftell");
    RESOLVE(real_rewind, "rewind");      RESOLVE(real_fflush, "fflush");

    rio_pack_init();
}

/* ================================================================
 *  open 家族 —— 只查 pack_lookup，命中则走 pack，否则透传
 * ================================================================ */

static mode_t extract_mode(int flags, va_list *ap) {
    if (flags & O_CREAT) {
        mode_t m = va_arg(*ap, mode_t);
        return m;
    }
    return 0;
}

/*
 * pack 模式的 open 共享辅助：
 *   查 rio_pack_lookup(path) → 打开 pack_file → rio_map_alloc(rh, offset, size)
 * 成功返回 fake fd；path 未注册或打开失败返回 -1。
 */
static int rio_open_pack_path(const char *path, int flags, mode_t mode) {
    const rio_pack_entry_t *pent = rio_pack_lookup(path);
    if (!pent) return -1;

    rio_connect();
    remote_handle_t rh = 0;
    if (rio_do_open(rio_pack_pack_file(), flags, mode, &rh) < 0) {
        errno = EIO; return -1;
    }
    int fake = rio_map_alloc(rh, pent->offset, pent->size);
    if (fake < 0) { rio_do_close(rh); errno = EMFILE; return -1; }
    RIO_LOG("open(pack) %s -> %s rh=%u off=%lu sz=%lu -> fakefd=%d",
            path, rio_pack_pack_file(), rh,
            (unsigned long)pent->offset, (unsigned long)pent->size, fake);
    return fake;
}

int open(const char *pathname, int flags, ...) {
    RIO_LOG("open(\"%s\", 0x%x)", pathname ? pathname : "(null)", flags);
    RESOLVE(real_open, "open");

    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);

    int fake = rio_open_pack_path(pathname, flags, mode);
    if (fake >= 0) return fake;
    return real_open ? real_open(pathname, flags, mode) : -1;
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    RIO_LOG("openat(dirfd=%d, \"%s\", 0x%x)", dirfd, pathname ? pathname : "(null)", flags);
    RESOLVE(real_openat, "openat");

    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);

    int fake = rio_open_pack_path(pathname, flags, mode);
    if (fake >= 0) return fake;
    return real_openat ? real_openat(dirfd, pathname, flags, mode) : -1;
}

int open64(const char *pathname, int flags, ...) {
    RESOLVE(real_open64, "open64");
    RESOLVE(real_open, "open");
    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);

    int fake = rio_open_pack_path(pathname, flags, mode);
    if (fake >= 0) return fake;
    return real_open64 ? real_open64(pathname, flags, mode)
         : real_open   ? real_open(pathname, flags, mode) : -1;
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    RESOLVE(real_openat64, "openat64");
    RESOLVE(real_openat, "openat");
    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);

    int fake = rio_open_pack_path(pathname, flags, mode);
    if (fake >= 0) return fake;
    return real_openat64 ? real_openat64(dirfd, pathname, flags, mode)
         : real_openat   ? real_openat(dirfd, pathname, flags, mode) : -1;
}

/* ---------- glibc 内部版本化符号包装 ---------- */

int __openat_2(int dirfd, const char *pathname, int flags) {
    return openat(dirfd, pathname, flags);
}
int __openat64_2(int dirfd, const char *pathname, int flags) {
    return openat64(dirfd, pathname, flags);
}
int __open_2(const char *pathname, int flags) {
    return open(pathname, flags);
}
int __open64_2(const char *pathname, int flags) {
    return open64(pathname, flags);
}
int __openat_nocancel(int dirfd, const char *pathname, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);
    return openat(dirfd, pathname, flags, mode);
}
int __open_nocancel(const char *pathname, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);
    return open(pathname, flags, mode);
}

/* ================================================================
 *  read / write / close / lseek —— 所有远程 fd 都是 pack 模式
 *  每次 read/write 前 lseek(pack_rh, pack_offset + pack_pos)
 *  lseek 只更新客户端 track 的 pack_pos
 * ================================================================ */

static size_t clamp_pack_count(const rio_slot_t *s, size_t count) {
    if (!s || s->pack_size == 0) return count;
    uint64_t remain = (s->pack_pos < s->pack_size) ? (s->pack_size - s->pack_pos) : 0;
    return (count > remain) ? (size_t)remain : count;
}

ssize_t read(int fd, void *buf, size_t count) {
    if (!rio_map_is_remote(fd)) return real_read ? real_read(fd, buf, count) : -1;

    rio_connect();
    remote_handle_t rh = rio_map_to_handle(fd);
    const rio_slot_t *s = rio_map_slot(fd);

    count = clamp_pack_count(s, count);
    if (count == 0) return 0;
    rio_do_lseek(rh, (off_t)(s->pack_offset + s->pack_pos), SEEK_SET);
    ssize_t n = rio_do_read(rh, buf, count);
    if (n > 0) rio_map_set_pos(fd, s->pack_pos + (uint64_t)n);
    return n;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (!rio_map_is_remote(fd)) return real_write ? real_write(fd, buf, count) : -1;

    rio_connect();
    remote_handle_t rh = rio_map_to_handle(fd);
    const rio_slot_t *s = rio_map_slot(fd);

    count = clamp_pack_count(s, count);
    if (count == 0) { errno = ENOSPC; return -1; }
    rio_do_lseek(rh, (off_t)(s->pack_offset + s->pack_pos), SEEK_SET);
    ssize_t n = rio_do_write(rh, buf, count);
    if (n > 0) rio_map_set_pos(fd, s->pack_pos + (uint64_t)n);
    return n;
}

int close(int fd) {
    if (!rio_map_is_remote(fd)) return real_close ? real_close(fd) : -1;

    rio_connect();
    remote_handle_t rh = rio_map_to_handle(fd);
    int rc = rio_do_close(rh);
    rio_map_free(fd);
    return rc;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (!rio_map_is_remote(fd)) return real_lseek ? real_lseek(fd, offset, whence) : (off_t)-1;

    const rio_slot_t *s = rio_map_slot(fd);
    uint64_t new_pos;
    switch (whence) {
        case SEEK_SET: new_pos = (offset < 0) ? 0 : (uint64_t)offset; break;
        case SEEK_CUR: new_pos = s->pack_pos + (uint64_t)offset; break;
        case SEEK_END:
            if (s->pack_size == 0) { errno = EINVAL; return (off_t)-1; }
            new_pos = s->pack_size + (uint64_t)offset;
            if (offset < 0 && new_pos > s->pack_size) new_pos = 0;
            break;
        default: errno = EINVAL; return (off_t)-1;
    }
    rio_map_set_pos(fd, new_pos);
    return (off_t)new_pos;
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    if (!rio_map_is_remote(fd)) return real_pread ? real_pread(fd, buf, count, offset) : -1;

    rio_connect();
    remote_handle_t rh = rio_map_to_handle(fd);
    const rio_slot_t *s = rio_map_slot(fd);

    if ((uint64_t)offset + count > s->pack_size && s->pack_size > 0)
        count = (size_t)(s->pack_size - (uint64_t)offset);
    if (count == 0) return 0;
    rio_do_lseek(rh, (off_t)(s->pack_offset + (uint64_t)offset), SEEK_SET);
    return rio_do_read(rh, buf, count);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    if (!rio_map_is_remote(fd)) return real_pwrite ? real_pwrite(fd, buf, count, offset) : -1;

    rio_connect();
    remote_handle_t rh = rio_map_to_handle(fd);
    const rio_slot_t *s = rio_map_slot(fd);

    if ((uint64_t)offset + count > s->pack_size && s->pack_size > 0)
        count = (size_t)(s->pack_size - (uint64_t)offset);
    if (count == 0) { errno = ENOSPC; return -1; }
    rio_do_lseek(rh, (off_t)(s->pack_offset + (uint64_t)offset), SEEK_SET);
    return rio_do_write(rh, buf, count);
}

/* ================================================================
 *  stat 家族 —— 对假 fd 返回合理的 regular file stat
 * ================================================================ */

static void fill_remote_stat(struct stat *st, uint64_t sz) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0644;
    st->st_size = (off_t)sz;
    st->st_nlink = 1;
    st->st_blocks = 0;
    st->st_blksize = 4096;
    st->st_uid = getuid();
    st->st_gid = getgid();
}

int fstat(int fd, struct stat *buf) {
    RESOLVE(real_fstat, "fstat");
    if (rio_map_is_remote(fd)) {
        if (!buf) { errno = EFAULT; return -1; }
        const rio_slot_t *s = rio_map_slot(fd);
        fill_remote_stat(buf, s ? s->pack_size : 0);
        return 0;
    }
    return real_fstat ? real_fstat(fd, buf) : -1;
}

int newfstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    RESOLVE(real_newfstatat, "newfstatat");
    if (rio_map_is_remote(dirfd) && !path) {
        if (!buf) { errno = EFAULT; return -1; }
        fill_remote_stat(buf, 0);
        return 0;
    }
    return real_newfstatat ? real_newfstatat(dirfd, path, buf, flags) : -1;
}

int stat(const char *path, struct stat *buf) {
    RESOLVE(real_stat, "stat");
    const rio_pack_entry_t *pent = rio_pack_lookup(path);
    if (pent) {
        if (!buf) { errno = EFAULT; return -1; }
        fill_remote_stat(buf, pent->size);
        return 0;
    }
    return real_stat ? real_stat(path, buf) : -1;
}

int lstat(const char *path, struct stat *buf) {
    RESOLVE(real_lstat, "lstat");
    const rio_pack_entry_t *pent = rio_pack_lookup(path);
    if (pent) {
        if (!buf) { errno = EFAULT; return -1; }
        fill_remote_stat(buf, pent->size);
        return 0;
    }
    return real_lstat ? real_lstat(path, buf) : -1;
}

/* ================================================================
 *  FILE* 层 —— fopen 用 fopencookie 创建，绕开假 fd 验证
 * ================================================================ */

#define RIO_FP_REMOTE_FLAG   (1u << 29)

static int is_remote_fp(FILE *fp) {
    int fd = real_fileno ? real_fileno(fp) : fileno(fp);
    return rio_map_is_remote(fd);
}

static int parse_mode(const char *mode, mode_t *out_mode) {
    if (!mode || !*mode) return -1;
    if (out_mode) *out_mode = 0666;
    int flags;
    switch (mode[0]) {
        case 'r': flags = O_RDONLY; break;
        case 'w': flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case 'a': flags = O_WRONLY | O_CREAT | O_APPEND; break;
        default: return -1;
    }
    for (const char *m = mode + 1; *m; m++) {
        switch (*m) {
            case '+': flags |= O_RDWR; break;
            case 'x': flags |= O_EXCL; break;
            case 'b': case 't': break;
            default: break;
        }
    }
    return flags;
}

/* ---------- fopencookie cookie ---------- */
typedef struct {
    remote_handle_t rh;
    uint64_t        pack_offset;
    uint64_t        pack_size;
    uint64_t        pack_pos;
} rio_cookie_t;

static ssize_t rio_cookie_read(void *c, char *buf, size_t size) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    rio_connect();
    size_t count = size;
    if (ck->pack_size > 0 && ck->pack_pos >= ck->pack_size) return 0;
    if (ck->pack_size > 0) {
        uint64_t remain = ck->pack_size - ck->pack_pos;
        if (count > remain) count = (size_t)remain;
    }
    rio_do_lseek(ck->rh, (off_t)(ck->pack_offset + ck->pack_pos), SEEK_SET);
    ssize_t n = rio_do_read(ck->rh, buf, count);
    if (n > 0) ck->pack_pos += (uint64_t)n;
    return n;
}
static ssize_t rio_cookie_write(void *c, const char *buf, size_t size) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    rio_connect();
    size_t count = size;
    if (ck->pack_size > 0 && ck->pack_pos >= ck->pack_size) { errno = ENOSPC; return -1; }
    if (ck->pack_size > 0) {
        uint64_t remain = ck->pack_size - ck->pack_pos;
        if (count > remain) count = (size_t)remain;
    }
    rio_do_lseek(ck->rh, (off_t)(ck->pack_offset + ck->pack_pos), SEEK_SET);
    ssize_t n = rio_do_write(ck->rh, buf, count);
    if (n > 0) ck->pack_pos += (uint64_t)n;
    return n;
}
static int rio_cookie_seek(void *c, off64_t *offset, int whence) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    uint64_t new_pos;
    off_t off = (off_t)*offset;
    switch (whence) {
        case SEEK_SET: new_pos = (off < 0) ? 0 : (uint64_t)off; break;
        case SEEK_CUR: new_pos = ck->pack_pos + (uint64_t)off; break;
        case SEEK_END:
            if (ck->pack_size == 0) return -1;
            new_pos = (int64_t)ck->pack_size + off;
            if (new_pos > ck->pack_size) new_pos = ck->pack_size;
            if ((int64_t)new_pos < 0) new_pos = 0;
            break;
        default: return -1;
    }
    ck->pack_pos = new_pos;
    *offset = (off64_t)new_pos;
    return 0;
}
static int rio_cookie_close(void *c) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    rio_connect();
    int rc = rio_do_close(ck->rh);
    free(ck);
    return rc;
}

static FILE *rio_fopen_impl(const char *path, const char *mode) {
    mode_t m = 0666;
    int flags = parse_mode(mode, &m);
    if (flags < 0) { errno = EINVAL; return NULL; }

    rio_connect();
    remote_handle_t rh = 0;
    const rio_pack_entry_t *pent = rio_pack_lookup(path);
    if (!pent) { errno = ENOENT; return NULL; }

    if (rio_do_openat(AT_FDCWD, rio_pack_pack_file(), flags, m, &rh) < 0) {
        errno = EIO; return NULL;
    }

    rio_cookie_t *ck = (rio_cookie_t *)calloc(1, sizeof(*ck));
    if (!ck) { rio_do_close(rh); errno = ENOMEM; return NULL; }
    ck->rh          = rh;
    ck->pack_offset = pent->offset;
    ck->pack_size   = pent->size;
    ck->pack_pos    = 0;

    cookie_io_functions_t funcs = {
        .read  = rio_cookie_read,
        .write = rio_cookie_write,
        .seek  = rio_cookie_seek,
        .close = rio_cookie_close,
    };
    FILE *fp = fopencookie(ck, mode, funcs);
    if (!fp) { rio_do_close(rh); free(ck); errno = EINVAL; return NULL; }
    fp->_flags |= RIO_FP_REMOTE_FLAG;
    return fp;
}

FILE *fopen(const char *path, const char *mode) {
    if (rio_pack_lookup(path)) {
        return rio_fopen_impl(path, mode);
    }
    RESOLVE(real_fopen, "fopen");
    return real_fopen ? real_fopen(path, mode) : NULL;
}

FILE *fopen64(const char *path, const char *mode) {
    if (rio_pack_lookup(path)) {
        return rio_fopen_impl(path, mode);
    }
    RESOLVE(real_fopen64, "fopen64");
    RESOLVE(real_fopen, "fopen");
    return real_fopen64 ? real_fopen64(path, mode)
           : real_fopen  ? real_fopen(path, mode) : NULL;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (rio_map_is_remote(real_fileno ? real_fileno(stream) : fileno(stream))) {
        rio_connect();
        rio_do_close(rio_map_to_handle(real_fileno ? real_fileno(stream) : fileno(stream)));
        rio_map_free(real_fileno ? real_fileno(stream) : fileno(stream));
        stream->_flags &= ~RIO_FP_REMOTE_FLAG;
    }
    FILE *fp = real_freopen ? real_freopen(path, mode, stream) : NULL;
    if (!fp) return NULL;
    int fd = real_fileno ? real_fileno(fp) : fileno(fp);
    if (rio_map_is_remote(fd)) fp->_flags |= RIO_FP_REMOTE_FLAG;
    return fp;
}

FILE *fdopen(int fd, const char *mode) {
    FILE *fp = real_fdopen ? real_fdopen(fd, mode) : NULL;
    if (!fp) return NULL;
    if (rio_map_is_remote(fd)) fp->_flags |= RIO_FP_REMOTE_FLAG;
    return fp;
}

int fileno(FILE *stream) {
    return real_fileno ? real_fileno(stream) : -1;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return real_fread ? real_fread(ptr, size, nmemb, stream) : 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return real_fwrite ? real_fwrite(ptr, size, nmemb, stream) : 0;
}

int fclose(FILE *fp) {
    if (is_remote_fp(fp)) {
        real_fflush ? real_fflush(fp) : fflush(fp);
        int fd = real_fileno ? real_fileno(fp) : fileno(fp);
        rio_connect();
        rio_do_close(rio_map_to_handle(fd));
        rio_map_free(fd);
    }
    return real_fclose ? real_fclose(fp) : EOF;
}

int fseek(FILE *stream, long offset, int whence) {
    if (is_remote_fp(stream)) {
        int fd = real_fileno ? real_fileno(stream) : fileno(stream);
        rio_connect();
        if (real_fflush) real_fflush(stream);
        /* pack 模式下 lseek 只更新客户端 pack_pos */
        off_t r = lseek(fd, offset, whence);
        return (r == (off_t)-1) ? -1 : 0;
    }
    return real_fseek ? real_fseek(stream, offset, whence) : -1;
}

long ftell(FILE *stream) {
    if (is_remote_fp(stream)) {
        int fd = real_fileno ? real_fileno(stream) : fileno(stream);
        if (real_fflush) real_fflush(stream);
        return (long)lseek(fd, 0, SEEK_CUR);
    }
    return real_ftell ? real_ftell(stream) : -1;
}

void rewind(FILE *stream) {
    if (is_remote_fp(stream)) {
        int fd = real_fileno ? real_fileno(stream) : fileno(stream);
        rio_connect();
        if (real_fflush) real_fflush(stream);
        lseek(fd, 0, SEEK_SET);
        stream->_flags &= ~(uint32_t)_IO_EOF_SEEN;
        return;
    }
    if (real_rewind) real_rewind(stream);
}

int fflush(FILE *stream) {
    return real_fflush ? real_fflush(stream) : EOF;
}
