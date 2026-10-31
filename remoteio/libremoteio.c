/*
 * libremoteio.c — LD_PRELOAD 劫持主模块
 *
 * 覆盖 glibc 中的标准文件 I/O 函数族，将 /remote/ 前缀的路径转发到远程节点。
 *
 * 劫持清单：
 *   系统调用层： open, openat, open64, openat64
 *               read, write, close, lseek, lseek64, pread, pwrite, pread64, pwrite64
 *   标准库层  ： fopen, fopen64, freopen, fdopen, fileno,
 *               fread, fwrite, fclose, fseek, ftell, rewind, fflush
 *
 * 策略：
 *   - constructor 初始化时用 dlsym(RTLD_NEXT, ...) 拉真实符号
 *   - 每个劫持函数先判断：路径/handle 是否属于远程
 *     · 远程 → 走 rio_proto 网络协议
 *     · 本地 → 原样透传给 real_*
 *   - FILE* 层内部仍走 open/read/write/fd，但为了让 fopen/fread/fwrite 等也统一，
 *     我们在 FILE* 里存一个标记：在 fdopen/fopen 时把 "是否远程" 写进 _IO_FILE._flags 的一个空闲位
 */

#define _GNU_SOURCE
#include "remoteio.h"

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

/* --- stat 家族 --- */
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
/*
 * constructor 默认优先级 65535（最后执行）。
 * 用 constructor(65534) 确保在 glibc 初始化之后跑，
 * 这样 dlsym(RTLD_NEXT, ...) 才能真正拿到 libc.so.6 的符号。
 *
 * 同时 RESOLVE 宏是懒加载：如果 constructor 时机还是太早，
 * 第一次被劫持的函数调用时也会补一次 resolve。
 */
/*
 * RESOLVE(var, "glibc_symbol_name")
 *   var —— 我们自己的 static 函数指针变量
 *   "glibc_symbol_name" —— glibc 里的真实函数名（字符串字面量）
 *
 * dlsym(RTLD_NEXT, symname) 会从当前库之后加载的库里找符号，
 * 对于 LD_PRELOAD 场景，当前 .so 是最先加载的，所以 RTLD_NEXT 跳过
 * 我们自己的 open 定义，继续找到 glibc 里的 open。
 */
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

    /* stat 家族 —— fdopen 内部调 fstat/newfstatat 验证 fd */
    RESOLVE(real_fstat, "fstat");                    RESOLVE(real_newfstatat, "newfstatat");
    RESOLVE(real_stat, "stat");                      RESOLVE(real_lstat, "lstat");

    RESOLVE(real_fopen, "fopen");        RESOLVE(real_fopen64, "fopen64");
    RESOLVE(real_freopen, "freopen");    RESOLVE(real_fdopen, "fdopen");
    RESOLVE(real_fileno, "fileno");      RESOLVE(real_fread, "fread");
    RESOLVE(real_fwrite, "fwrite");      RESOLVE(real_fclose, "fclose");
    RESOLVE(real_fseek, "fseek");        RESOLVE(real_ftell, "ftell");
    RESOLVE(real_rewind, "rewind");      RESOLVE(real_fflush, "fflush");

    RIO_LOG("libremoteio initialized (prefix=\"%s\")", RIO_REMOTE_PREFIX);
}

/* 兜底：第一次调用时确保所有 real_* 都已 resolve
 * （标记为 unused 因为目前只通过 RESOLVE 宏懒加载，但保留备用） */
static void __attribute__((unused)) rio_ensure_resolved(void) {
    RESOLVE(real_open, "open");          RESOLVE(real_openat, "openat");
    RESOLVE(real_open64, "open64");      RESOLVE(real_openat64, "openat64");
    RESOLVE(real_read, "read");          RESOLVE(real_write, "write");
    RESOLVE(real_close, "close");        RESOLVE(real_lseek, "lseek");
    RESOLVE(real_pread, "pread");        RESOLVE(real_pwrite, "pwrite");
}

/* ================================================================
 *  open 家族 —— 远程路径 → 网络，其他透传
 * ================================================================ */

/* 提取 open/openat 里可变参数的 mode_t */
static mode_t extract_mode(int flags, va_list *ap) {
    if (flags & O_CREAT) {
        mode_t m = va_arg(*ap, mode_t);
        return m;
    }
    return 0;
}

int open(const char *pathname, int flags, ...) {
    RIO_LOG("open(\"%s\", 0x%x) real_open=%p", pathname ? pathname : "(null)", flags, (void*)real_open);
    if (rio_is_remote_path(pathname)) {
        va_list ap; va_start(ap, flags);
        mode_t mode = extract_mode(flags, &ap); va_end(ap);

        rio_connect();
        remote_handle_t rh = 0;
        if (rio_do_open(pathname, flags, mode, &rh) < 0) {
            errno = EIO; return -1;
        }
        int fake = rio_map_alloc(rh);
        if (fake < 0) { rio_do_close(rh); errno = EMFILE; return -1; }
        RIO_LOG("open(remote) %s -> fakefd=%d handle=%u", pathname, fake, rh);
        return fake;
    }
    /* 本地：透传给 real_open */
    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);
    if (real_open) return real_open(pathname, flags, mode);
    return -1;  /* constructor 没拉到符号（极罕见） */
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    RIO_LOG("openat(dirfd=%d, \"%s\", 0x%x) is_remote=%d",
            dirfd, pathname ? pathname : "(null)", flags, rio_is_remote_path(pathname));
    RESOLVE(real_openat, "openat");
    if (rio_is_remote_path(pathname)) {
        va_list ap; va_start(ap, flags);
        mode_t mode = extract_mode(flags, &ap); va_end(ap);

        rio_connect();
        remote_handle_t rh = 0;
        if (rio_do_openat(dirfd, pathname, flags, mode, &rh) < 0) {
            errno = EIO; return -1;
        }
        int fake = rio_map_alloc(rh);
        if (fake < 0) { rio_do_close(rh); errno = EMFILE; return -1; }
        RIO_LOG("openat(remote) dirfd=%d %s -> fakefd=%d", dirfd, pathname, fake);
        return fake;
    }
    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);
    if (real_openat) return real_openat(dirfd, pathname, flags, mode);
    return -1;
}

/* open64 / openat64 通常只是 open 的别名，libc 里常见弱别名 */
int open64(const char *pathname, int flags, ...) {
    if (rio_is_remote_path(pathname)) {
        va_list ap; va_start(ap, flags);
        mode_t mode = extract_mode(flags, &ap); va_end(ap);
        rio_connect();
        remote_handle_t rh = 0;
        if (rio_do_open(pathname, flags, mode, &rh) < 0) { errno = EIO; return -1; }
        int fake = rio_map_alloc(rh);
        if (fake < 0) { rio_do_close(rh); errno = EMFILE; return -1; }
        return fake;
    }
    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);
    if (real_open64) return real_open64(pathname, flags, mode);
    if (real_open)   return real_open(pathname, flags, mode);
    return -1;
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    RIO_LOG("openat64(dirfd=%d, \"%s\", 0x%x)", dirfd, pathname ? pathname : "(null)", flags);
    RESOLVE(real_openat64, "openat64");
    if (rio_is_remote_path(pathname)) {
        va_list ap; va_start(ap, flags);
        mode_t mode = extract_mode(flags, &ap); va_end(ap);
        rio_connect();
        remote_handle_t rh = 0;
        if (rio_do_openat(dirfd, pathname, flags, mode, &rh) < 0) { errno = EIO; return -1; }
        int fake = rio_map_alloc(rh);
        if (fake < 0) { rio_do_close(rh); errno = EMFILE; return -1; }
        return fake;
    }
    va_list ap; va_start(ap, flags);
    mode_t mode = extract_mode(flags, &ap); va_end(ap);
    if (real_openat64) return real_openat64(dirfd, pathname, flags, mode);
    if (real_openat)   return real_openat(dirfd, pathname, flags, mode);
    return -1;
}

/* ================================================================
 *  read / write / close / lseek —— 根据 fd 决定走远程还是本地
 * ================================================================ */

ssize_t read(int fd, void *buf, size_t count) {
    if (rio_map_is_remote(fd)) {
        rio_connect();
        remote_handle_t rh = rio_map_to_handle(fd);
        return rio_do_read(rh, buf, count);
    }
    return real_read ? real_read(fd, buf, count) : -1;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (rio_map_is_remote(fd)) {
        rio_connect();
        remote_handle_t rh = rio_map_to_handle(fd);
        return rio_do_write(rh, buf, count);
    }
    return real_write ? real_write(fd, buf, count) : -1;
}

int close(int fd) {
    if (rio_map_is_remote(fd)) {
        rio_connect();
        remote_handle_t rh = rio_map_to_handle(fd);
        int rc = rio_do_close(rh);
        rio_map_free(fd);
        return rc;
    }
    return real_close ? real_close(fd) : -1;
}

/* ---------- glibc 内部版本化符号包装 ----------
 *
 * glibc 头文件 <fcntl.h> 已经声明了这些函数，签名不带可变参数。
 * 我们只需要导出同签名的实现即可（内部走 openat 劫持链）。
 *
 *   __openat_2 / __openat64_2   (glibc 2.7+)
 *   __open_2 / __open64_2
 */

/* 注意：这些函数没有可变参数 —— open 的可变参数模式只在用户公开的 open() 里需要 */
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
 *  lseek / pread / pwrite
 * ================================================================ */
off_t lseek(int fd, off_t offset, int whence) {
    if (rio_map_is_remote(fd)) {
        rio_connect();
        return rio_do_lseek(rio_map_to_handle(fd), offset, whence);
    }
    return real_lseek ? real_lseek(fd, offset, whence) : (off_t)-1;
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    if (rio_map_is_remote(fd)) {
        /* 简单实现：先 lseek 再 read，不保留原位置 */
        rio_connect();
        rio_do_lseek(rio_map_to_handle(fd), offset, SEEK_SET);
        return rio_do_read(rio_map_to_handle(fd), buf, count);
    }
    return real_pread ? real_pread(fd, buf, count, offset) : -1;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    if (rio_map_is_remote(fd)) {
        rio_connect();
        rio_do_lseek(rio_map_to_handle(fd), offset, SEEK_SET);
        return rio_do_write(rio_map_to_handle(fd), buf, count);
    }
    return real_pwrite ? real_pwrite(fd, buf, count, offset) : -1;
}

/* ================================================================
 *  stat 家族 —— fdopen / fseek 等内部会调 fstat / newfstatat 验证 fd
 *  对我们的假 fd，返回一个合理的 regular file stat
 * ================================================================ */

/*
 * 对假 fd，我们需要返回它对应的远程文件 size。
 * 最快的方式：服务端已经 track 了 file pos，但 stat 需要 size。
 * 简化处理：返回 size=0 的 regular file（fdopen 只检查文件类型是不是 REG）。
 * 如果 fdopen 还要检查权限位等，我们返回默认 0644。
 */
static void fill_remote_stat(struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0644;
    st->st_size = 0;
    st->st_nlink = 1;
    st->st_blocks = 0;
    st->st_blksize = 4096;
    /* uid/gid 用当前进程 */
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_atim = (struct timespec){0};
    st->st_mtim = (struct timespec){0};
    st->st_ctim = (struct timespec){0};
}

int fstat(int fd, struct stat *buf) {
    RIO_LOG("fstat(%d)", fd);
    RESOLVE(real_fstat, "fstat");
    if (rio_map_is_remote(fd)) {
        if (!buf) { errno = EFAULT; return -1; }
        fill_remote_stat(buf);
        return 0;
    }
    return real_fstat ? real_fstat(fd, buf) : -1;
}

/* newfstatat(fd, path, buf, flags) —— glibc 新版 openat/fdopen 内部用 */
int newfstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    RIO_LOG("newfstatat(dirfd=%d, \"%s\", fl=%d)", dirfd, path ? path : "(null)", flags);
    RESOLVE(real_newfstatat, "newfstatat");
    /*
     * 两种情况：
     *   1. dirfd 是假 fd，path 是 NULL（对 openat(dirfd=fd, NULL, ...) 或 fstatat(fd, NULL, ...)）
     *   2. path 是 /remote/... 开头的路径
     */
    if ((rio_map_is_remote(dirfd) && !path) || rio_is_remote_path(path)) {
        if (!buf) { errno = EFAULT; return -1; }
        fill_remote_stat(buf);
        return 0;
    }
    return real_newfstatat ? real_newfstatat(dirfd, path, buf, flags) : -1;
}

/* stat(path, buf) —— 某些库可能用 */
int stat(const char *path, struct stat *buf) {
    RESOLVE(real_stat, "stat");
    if (rio_is_remote_path(path)) {
        if (!buf) { errno = EFAULT; return -1; }
        fill_remote_stat(buf);
        return 0;
    }
    return real_stat ? real_stat(path, buf) : -1;
}

int lstat(const char *path, struct stat *buf) {
    RESOLVE(real_lstat, "lstat");
    if (rio_is_remote_path(path)) {
        if (!buf) { errno = EFAULT; return -1; }
        fill_remote_stat(buf);
        return 0;
    }
    return real_lstat ? real_lstat(path, buf) : -1;
}

/* ================================================================
 *  FILE* 层 —— fopen/fread/fwrite/fclose 等
 *
 *  glibc FILE* 内部走 read/write/fd 上的系统调用。
 *  所以只要 open 返回我们的假 fd，fread/fwrite 自动被上面的 read/write 劫持覆盖。
 *  我们只需保证：fopen 也走到我们的 open（用 open 劫持已经做到了）。
 *  下面的函数显式劫持以保证 FILE*→fd 一致性，且处理 fseek/ftell/rewind。
 * ================================================================ */

/*
 * _IO_FILE._flags 的第 27 位 glibc 叫 _IO_EOF_SEEN，我们不碰它。
 * 用第 30 位（远离 glibc 已知标志位）标记 "这个 FILE* 绑定的 fd 是远程的"。
 * 实际标记不一定依赖这个位，fileno() 会返回假 fd，后续 read/write 劫持已经能识别。
 * 这里保留标记位只是个冗余保险。
 */
#define RIO_FP_REMOTE_FLAG   (1u << 29)

static int is_remote_fp(FILE *fp) {
    int fd = real_fileno ? real_fileno(fp) : fileno(fp);
    return rio_map_is_remote(fd);
}

/*
 * parse_mode —— C 的 fopen 模式串 → openat flags + mode
 * 标准 C 模式：r/w/a/r+/w+/a+ 以及带 b、带 x 变体
 */
static int parse_mode(const char *mode, mode_t *out_mode) {
    if (!mode || !*mode) return -1;
    if (out_mode) *out_mode = 0666;   /* 默认 umask 会调整 */
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
            case 'b': case 't': break;    /* text/binary, 无效果 */
            default: break;
        }
    }
    return flags;
}

/* ---------- fopencookie cookie ----------
 *
 * 用 fopencookie 创建 FILE* —— 回调内部直接走 rio_proto 远程协议。
 * cookie 里存远程 handle + 当前位置（服务端 track pos，但 FILE* 层需要）。
 */
typedef struct {
    remote_handle_t rh;
} rio_cookie_t;

static ssize_t rio_cookie_read(void *c, char *buf, size_t size) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    rio_connect();
    return rio_do_read(ck->rh, buf, size);
}
static ssize_t rio_cookie_write(void *c, const char *buf, size_t size) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    rio_connect();
    return rio_do_write(ck->rh, buf, size);
}
static int rio_cookie_seek(void *c, off64_t *offset, int whence) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    rio_connect();
    off_t r = rio_do_lseek(ck->rh, (off_t)*offset, whence);
    if (r == (off_t)-1) return -1;
    *offset = r;
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

    /* 确保连接 + 远程 open */
    rio_connect();
    remote_handle_t rh = 0;
    if (rio_do_openat(AT_FDCWD, path, flags, m, &rh) < 0) {
        errno = EIO; return NULL;
    }

    /* 用 fopencookie 创建 FILE* —— 完全绕开 glibc fdopen 对 fd 的验证 */
    rio_cookie_t *ck = (rio_cookie_t *)malloc(sizeof(*ck));
    if (!ck) { rio_do_close(rh); errno = ENOMEM; return NULL; }
    ck->rh = rh;

    cookie_io_functions_t funcs = {
        .read  = rio_cookie_read,
        .write = rio_cookie_write,
        .seek  = rio_cookie_seek,
        .close = rio_cookie_close,
    };
    FILE *fp = fopencookie(ck, mode, funcs);
    if (!fp) {
        rio_do_close(rh);
        free(ck);
        errno = EINVAL;
        return NULL;
    }
    fp->_flags |= RIO_FP_REMOTE_FLAG;
    return fp;
}

FILE *fopen(const char *path, const char *mode) {
    /* 如果是远程路径 —— 自己实现，绕开 glibc fopen 内部硬编码的 openat 调用 */
    if (rio_is_remote_path(path)) {
        return rio_fopen_impl(path, mode);
    }
    /* 本地路径 —— 直接走 glibc，它内部 openat 会走 PLT 然后到我们的劫持链 */
    RESOLVE(real_fopen, "fopen");
    return real_fopen ? real_fopen(path, mode) : NULL;
}

FILE *fopen64(const char *path, const char *mode) {
    if (rio_is_remote_path(path)) {
        return rio_fopen_impl(path, mode);
    }
    RESOLVE(real_fopen64, "fopen64");
    RESOLVE(real_fopen, "fopen");
    return real_fopen64 ? real_fopen64(path, mode)
           : real_fopen  ? real_fopen(path, mode) : NULL;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (rio_map_is_remote(real_fileno ? real_fileno(stream) : fileno(stream))) {
        /* 旧流是远程的 —— 先手动 close 远程 handle */
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
    /* 对远程 FILE*，返回我们的假 fd；让后续 read/write 自动走远程 */
    return real_fileno ? real_fileno(stream) : -1;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    /* FILE* 层自带缓冲。对于远程 FILE*，如果内部缓冲数据足够直接返回；
     * 否则 FILE* 层会调 read() 拉数据 —— 我们的 read() 劫持自动覆盖。
     * 所以这里不需要额外处理，透传给 real_fread 即可。 */
    return real_fread ? real_fread(ptr, size, nmemb, stream) : 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return real_fwrite ? real_fwrite(ptr, size, nmemb, stream) : 0;
}

int fclose(FILE *fp) {
    /* 远程 FILE* 先 flush 内部缓冲，然后 close 远程 handle */
    if (is_remote_fp(fp)) {
        real_fflush ? real_fflush(fp) : fflush(fp);
        int fd = real_fileno ? real_fileno(fp) : fileno(fp);
        rio_connect();
        rio_do_close(rio_map_to_handle(fd));
        rio_map_free(fd);
        /* 仍要让真实 fclose 释放 FILE* 内存结构（它不会再调我们的 close，因为 fd 不是系统 fd 范围）*/
    }
    return real_fclose ? real_fclose(fp) : EOF;
}

int fseek(FILE *stream, long offset, int whence) {
    if (is_remote_fp(stream)) {
        int fd = real_fileno ? real_fileno(stream) : fileno(stream);
        rio_connect();
        /* 先清掉 FILE* 的缓冲（保证 lseek 基准是正确的文件位置） */
        if (real_fflush) real_fflush(stream);
        off_t r = rio_do_lseek(rio_map_to_handle(fd), offset, whence);
        return (r == (off_t)-1) ? -1 : 0;
    }
    return real_fseek ? real_fseek(stream, offset, whence) : -1;
}

long ftell(FILE *stream) {
    if (is_remote_fp(stream)) {
        int fd = real_fileno ? real_fileno(stream) : fileno(stream);
        rio_connect();
        if (real_fflush) real_fflush(stream);
        return (long)rio_do_lseek(rio_map_to_handle(fd), 0, SEEK_CUR);
    }
    return real_ftell ? real_ftell(stream) : -1;
}

void rewind(FILE *stream) {
    if (is_remote_fp(stream)) {
        int fd = real_fileno ? real_fileno(stream) : fileno(stream);
        rio_connect();
        if (real_fflush) real_fflush(stream);
        rio_do_lseek(rio_map_to_handle(fd), 0, SEEK_SET);
        stream->_flags &= ~(uint32_t)_IO_EOF_SEEN;
        return;
    }
    if (real_rewind) real_rewind(stream);
}

int fflush(FILE *stream) {
    /* 远程 FILE* 调 fflush 时，FILE* 会把缓冲里的数据调我们的 write() 刷出去 */
    return real_fflush ? real_fflush(stream) : EOF;
}
