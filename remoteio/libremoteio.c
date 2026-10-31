/*
 * libremoteio.c — LD_PRELOAD 劫持主模块（按需拉取 + 版本化）
 *
 * 用户端：
 *   - 启动只拉 meta（~3KB header + entries + versions[]）
 *   - 每段 body 按需拉，每次 read 前版本检测（OP_SEG_VERSION → 变了才 OP_SEG_READ）
 *   - 服务端随时可以修改某段 body — 客户端下次 read 自动发现并重拉
 *   - write: 本地 shm 写 + dirty=1 —— close 时批量 OP_WRITE_SEG 穿透回服务端
 *
 * 劫持清单：
 *   系统调用层：open, openat, open64, openat64
 *               read, write, close, lseek, lseek64, pread, pwrite, pread64, pwrite64
 *   标准库层  ：fopen, fopen64, freopen, fdopen, fileno,
 *               fread, fwrite, fclose, fseek, ftell, rewind, fflush
 *
 * 关键约束：
 *   - constructor(65534) 必须比 glibc init 稍晚，但比应用 init 早
 *   - 必须劫持 __open_2 / __openat_2 等 glibc 版本化符号
 *   - libremoteio.so 本身绝不能硬编码任何 /virtual/xxx 业务路径
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
 *  真实 glibc 函数指针（constructor 中通过 dlsym(RTLD_NEXT) 初始化）
 * ================================================================ */

typedef int     (*real_open_t)(const char *, int, ...);
typedef int     (*real_openat_t)(int, const char *, int, ...);
typedef int     (*real_open64_t)(const char *, int, ...);
typedef int     (*real_openat64_t)(int, const char *, int, ...);

typedef ssize_t (*real_read_t)(int, void *, size_t);
typedef ssize_t (*real_write_t)(int, const void *, size_t);
typedef int     (*real_close_t)(int);
typedef off_t   (*real_lseek_t)(int, off_t, int);
typedef ssize_t (*real_pread_t)(int, void *, size_t, off_t);
typedef ssize_t (*real_pwrite_t)(int, const void *, size_t, off_t);

typedef int (*real_fstat_t)(int, struct stat *);
typedef int (*real_newfstatat_t)(int, const char *, struct stat *, int);
typedef int (*real_stat_t)(const char *, struct stat *);
typedef int (*real_lstat_t)(const char *, struct stat *);

typedef FILE*   (*real_fopen_t)(const char *, const char *);
typedef FILE*   (*real_fopen64_t)(const char *, const char *);
typedef FILE*   (*real_freopen_t)(const char *, const char *, FILE *);
typedef FILE*   (*real_fdopen_t)(int, const char *);
typedef int     (*real_fileno_t)(FILE *);
typedef size_t  (*real_fread_t)(void *, size_t, size_t, FILE *);
typedef size_t  (*real_fwrite_t)(const void *, size_t, size_t, FILE *);
typedef int     (*real_fclose_t)(FILE *);
typedef int     (*real_fseek_t)(FILE *, long, int);
typedef long    (*real_ftell_t)(FILE *);
typedef int     (*real_rewind_t)(FILE *);
typedef int     (*real_fflush_t)(FILE *);

static real_open_t     real_open     = NULL;
static real_openat_t   real_openat   = NULL;
static real_open64_t   real_open64   = NULL;
static real_openat64_t real_openat64 = NULL;
static real_read_t     real_read     = NULL;
static real_write_t    real_write    = NULL;
static real_close_t    real_close    = NULL;
static real_lseek_t    real_lseek    = NULL;
static real_pread_t    real_pread    = NULL;
static real_pwrite_t   real_pwrite   = NULL;

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
        (var) = (typeof(var)) dlsym(RTLD_NEXT, symname); \
    } \
} while (0)

__attribute__((constructor(65534)))
static void rio_init(void) {
    RESOLVE(real_open, "open");          RESOLVE(real_openat, "openat");
    RESOLVE(real_open64, "open64");      RESOLVE(real_openat64, "openat64");
    RESOLVE(real_read, "read");          RESOLVE(real_write, "write");
    RESOLVE(real_close, "close");        RESOLVE(real_lseek, "lseek");
    RESOLVE(real_pread, "pread");        RESOLVE(real_pwrite, "pwrite");

    RESOLVE(real_fstat, "fstat");                 RESOLVE(real_newfstatat, "newfstatat");
    RESOLVE(real_stat, "stat");                   RESOLVE(real_lstat, "lstat");

    RESOLVE(real_fopen, "fopen");        RESOLVE(real_fopen64, "fopen64");
    RESOLVE(real_freopen, "freopen");    RESOLVE(real_fdopen, "fdopen");
    RESOLVE(real_fileno, "fileno");      RESOLVE(real_fread, "fread");
    RESOLVE(real_fwrite, "fwrite");      RESOLVE(real_fclose, "fclose");
    RESOLVE(real_fseek, "fseek");        RESOLVE(real_ftell, "ftell");
    RESOLVE(real_rewind, "rewind");      RESOLVE(real_fflush, "fflush");

    rio_pack_init();
}

/* ================================================================
 *  open 家族 —— 找到 seg_idx，分配 slot
 * ================================================================ */

static mode_t extract_mode(int flags, va_list *ap) {
    if (flags & O_CREAT) {
        mode_t m = va_arg(*ap, mode_t);
        return m;
    }
    return 0;
}

static int rio_open_pack_path(const char *path, int flags, mode_t mode) {
    (void)flags; (void)mode;

    const rio_pack_entry_t *pent = rio_pack_lookup(path);
    if (!pent) return -1;

    /* 找 seg_idx —— rio_pack_lookup 已经在 g_entries 里了，
     * 我们用 rio_pack_find_seg_idx 也能拿到（内部 linear search）*/
    int seg_idx = rio_pack_find_seg_idx(path);
    if (seg_idx < 0) return -1;

    uint8_t *base = rio_pack_shm_base();
    size_t   sz   = rio_pack_shm_size();
    if (!base) {
        RIO_LOG("open(pack): shm_base is NULL — autoload 失败？");
        errno = EIO; return -1;
    }

    int fake = rio_map_alloc(base, sz, pent->offset, pent->size, (uint32_t)seg_idx);
    if (fake < 0) { errno = EMFILE; return -1; }

    RIO_LOG("open(pack) %s -> shm+%lu sz=%lu seg=%d -> fakefd=%d",
            path, (unsigned long)pent->offset, (unsigned long)pent->size,
            seg_idx, fake);
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
 *  read / write / close / lseek —— 假 fd 全是本地 memcpy
 * ================================================================ */

/* clamp count 到 [local_pos, virtual_sz] 范围内，返回要读写的实际字节数 */
static size_t clamp_virtual_count(const rio_slot_t *s, uint64_t pos, size_t count) {
    if (!s) return count;
    if (s->virtual_sz == 0) return count;  /* 0 = 不限 */
    if (pos >= s->virtual_sz) return 0;
    uint64_t remain = s->virtual_sz - pos;
    return (count > remain) ? (size_t)remain : count;
}

ssize_t read(int fd, void *buf, size_t count) {
    if (!rio_map_is_remote(fd)) return real_read ? real_read(fd, buf, count) : -1;

    const rio_slot_t *s = rio_map_slot(fd);
    if (!s || !s->shm_base) { errno = EBADF; return -1; }

    /* 确保这段 body 已拉取 + 版本最新 */
    if (rio_pack_ensure_seg(s->seg_idx) < 0) { errno = EIO; return -1; }

    uint64_t pos = s->local_pos;
    size_t   n   = clamp_virtual_count(s, pos, count);
    if (n == 0) return 0;

    memcpy(buf, s->shm_base + s->virtual_off + pos, n);
    rio_map_set_pos(fd, pos + (uint64_t)n);
    return (ssize_t)n;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (!rio_map_is_remote(fd)) return real_write ? real_write(fd, buf, count) : -1;

    const rio_slot_t *s = rio_map_slot(fd);
    if (!s || !s->shm_base) { errno = EBADF; return -1; }

    /* 确保这段 body 已拉取（可能要覆盖已有数据 —— 但先 ensure 拿到最新版本）*/
    if (rio_pack_ensure_seg(s->seg_idx) < 0) { errno = EIO; return -1; }

    uint64_t pos = s->local_pos;
    size_t   n   = clamp_virtual_count(s, pos, count);
    if (n == 0) { errno = ENOSPC; return -1; }

    memcpy(s->shm_base + s->virtual_off + pos, buf, n);
    rio_map_set_pos(fd, pos + (uint64_t)n);

    /* 标记 dirty + 立刻穿透到服务端 —— 保证下次 ensure_seg 拉回来的是最新 */
    rio_pack_mark_dirty(s->seg_idx);
    rio_pack_push_seg(s->seg_idx);

    return (ssize_t)n;
}

int close(int fd) {
    if (!rio_map_is_remote(fd)) return real_close ? real_close(fd) : -1;

    /* close 前尝试 flush dirty segs（忽略网络失败 —— 客户端退出前尽力而为）*/
    rio_pack_flush_dirty_segs();

    rio_map_free(fd);
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (!rio_map_is_remote(fd)) return real_lseek ? real_lseek(fd, offset, whence) : (off_t)-1;

    const rio_slot_t *s = rio_map_slot(fd);
    if (!s) { errno = EBADF; return (off_t)-1; }

    uint64_t new_pos;
    switch (whence) {
        case SEEK_SET: new_pos = (offset < 0) ? 0 : (uint64_t)offset; break;
        case SEEK_CUR: new_pos = s->local_pos + (uint64_t)offset; break;
        case SEEK_END:
            if (s->virtual_sz == 0) { errno = EINVAL; return (off_t)-1; }
            new_pos = s->virtual_sz + (int64_t)offset;
            if ((int64_t)new_pos < 0) new_pos = 0;
            if (new_pos > s->virtual_sz) new_pos = s->virtual_sz;
            break;
        default: errno = EINVAL; return (off_t)-1;
    }
    rio_map_set_pos(fd, new_pos);
    return (off_t)new_pos;
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    if (!rio_map_is_remote(fd)) return real_pread ? real_pread(fd, buf, count, offset) : -1;

    const rio_slot_t *s = rio_map_slot(fd);
    if (!s || !s->shm_base) { errno = EBADF; return -1; }

    if (rio_pack_ensure_seg(s->seg_idx) < 0) { errno = EIO; return -1; }

    if (offset < 0) { errno = EINVAL; return -1; }
    uint64_t pos = (uint64_t)offset;
    size_t   n   = clamp_virtual_count(s, pos, count);
    if (n == 0) return 0;

    memcpy(buf, s->shm_base + s->virtual_off + pos, n);
    /* pread 不更新 local_pos */
    return (ssize_t)n;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    if (!rio_map_is_remote(fd)) return real_pwrite ? real_pwrite(fd, buf, count, offset) : -1;

    const rio_slot_t *s = rio_map_slot(fd);
    if (!s || !s->shm_base) { errno = EBADF; return -1; }

    if (rio_pack_ensure_seg(s->seg_idx) < 0) { errno = EIO; return -1; }

    if (offset < 0) { errno = EINVAL; return -1; }
    uint64_t pos = (uint64_t)offset;
    size_t   n   = clamp_virtual_count(s, pos, count);
    if (n == 0) { errno = ENOSPC; return -1; }

    memcpy(s->shm_base + s->virtual_off + pos, buf, n);
    rio_pack_mark_dirty(s->seg_idx);
    rio_pack_push_seg(s->seg_idx);
    /* pwrite 不更新 local_pos */
    return (ssize_t)n;
}

/* lseek64 / pread64 / pwrite64 在 glibc 里通常就是别名，通常不需要单独劫持 */

/* ================================================================
 *  stat 家族
 * ================================================================ */

static void fill_remote_stat(struct stat *st, uint64_t sz) {
    memset(st, 0, sizeof(*st));
    st->st_mode    = S_IFREG | 0644;
    st->st_size    = (off_t)sz;
    st->st_nlink   = 1;
    st->st_blocks  = 0;
    st->st_blksize = 4096;
    st->st_uid     = getuid();
    st->st_gid     = getgid();
}

int fstat(int fd, struct stat *buf) {
    RESOLVE(real_fstat, "fstat");
    if (rio_map_is_remote(fd)) {
        if (!buf) { errno = EFAULT; return -1; }
        const rio_slot_t *s = rio_map_slot(fd);
        fill_remote_stat(buf, s ? s->virtual_sz : 0);
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
 *  FILE* 层 —— fopencookie 创建，回调全是 memcpy
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

/* ---------- fopencookie cookie（按需拉取 + 版本化） ---------- */
typedef struct {
    uint8_t  *shm_base;
    size_t    shm_size;
    uint64_t  virtual_off;
    uint64_t  virtual_sz;
    uint64_t  local_pos;
    uint32_t  seg_idx;       /* 对应 pack entry 下标 */
} rio_cookie_t;

/* 辅助：读前 ensure_seg */
static int rio_cookie_ensure(rio_cookie_t *ck) {
    return rio_pack_ensure_seg(ck->seg_idx);
}

static ssize_t rio_cookie_read(void *c, char *buf, size_t size) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    if (rio_cookie_ensure(ck) < 0) return -1;

    /* clamp_virtual_count 里 s==NULL 返回 count，我们在这里自己 clamp */
    size_t n = size;
    if (ck->virtual_sz > 0 && ck->local_pos >= ck->virtual_sz) return 0;
    if (ck->virtual_sz > 0) {
        uint64_t remain = ck->virtual_sz - ck->local_pos;
        if (n > remain) n = (size_t)remain;
    }
    if (n == 0) return 0;
    memcpy(buf, ck->shm_base + ck->virtual_off + ck->local_pos, n);
    ck->local_pos += (uint64_t)n;
    return (ssize_t)n;
}

static ssize_t rio_cookie_write(void *c, const char *buf, size_t size) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    if (rio_cookie_ensure(ck) < 0) return -1;

    if (ck->virtual_sz > 0 && ck->local_pos >= ck->virtual_sz) { errno = ENOSPC; return -1; }
    size_t n = size;
    if (ck->virtual_sz > 0) {
        uint64_t remain = ck->virtual_sz - ck->local_pos;
        if (n > remain) n = (size_t)remain;
    }
    if (n == 0) { errno = ENOSPC; return -1; }
    memcpy(ck->shm_base + ck->virtual_off + ck->local_pos, buf, n);
    ck->local_pos += (uint64_t)n;
    rio_pack_mark_dirty(ck->seg_idx);
    rio_pack_push_seg(ck->seg_idx);
    return (ssize_t)n;
}

static int rio_cookie_seek(void *c, off64_t *offset, int whence) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    uint64_t new_pos;
    off_t off = (off_t)*offset;
    switch (whence) {
        case SEEK_SET: new_pos = (off < 0) ? 0 : (uint64_t)off; break;
        case SEEK_CUR: new_pos = ck->local_pos + (uint64_t)off; break;
        case SEEK_END:
            if (ck->virtual_sz == 0) return -1;
            new_pos = (int64_t)ck->virtual_sz + off;
            if (new_pos > ck->virtual_sz) new_pos = ck->virtual_sz;
            if ((int64_t)new_pos < 0) new_pos = 0;
            break;
        default: return -1;
    }
    ck->local_pos = new_pos;
    *offset = (off64_t)new_pos;
    return 0;
}

static int rio_cookie_close(void *c) {
    rio_cookie_t *ck = (rio_cookie_t *)c;
    free(ck);
    /* fopencookie 路径的 flush 走 real_fflush + real_fclose ——
     * 但 close 劫持函数会 flush 所有 dirty segs */
    return 0;
}

static FILE *rio_fopen_impl(const char *path, const char *mode) {
    mode_t m = 0666;
    int flags = parse_mode(mode, &m);
    if (flags < 0) { errno = EINVAL; return NULL; }

    const rio_pack_entry_t *pent = rio_pack_lookup(path);
    if (!pent) { errno = ENOENT; return NULL; }

    uint8_t *base = rio_pack_shm_base();
    size_t   sz   = rio_pack_shm_size();
    if (!base) { errno = EIO; return NULL; }

    int seg_idx = rio_pack_find_seg_idx(path);

    rio_cookie_t *ck = (rio_cookie_t *)calloc(1, sizeof(*ck));
    if (!ck) { errno = ENOMEM; return NULL; }
    ck->shm_base    = base;
    ck->shm_size    = sz;
    ck->virtual_off = pent->offset;
    ck->virtual_sz  = pent->size;
    ck->local_pos   = 0;
    ck->seg_idx     = (uint32_t)(seg_idx < 0 ? 0 : seg_idx);

    cookie_io_functions_t funcs = {
        .read  = rio_cookie_read,
        .write = rio_cookie_write,
        .seek  = rio_cookie_seek,
        .close = rio_cookie_close,
    };
    FILE *fp = fopencookie(ck, mode, funcs);
    if (!fp) { free(ck); errno = EINVAL; return NULL; }
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
    /* shm 模式下不支持对假 fd 做 freopen —— 走真实 freopen */
    FILE *fp = real_freopen ? real_freopen(path, mode, stream) : NULL;
    return fp;
}

FILE *fdopen(int fd, const char *mode) {
    /* 对假 fd，glibc 真实 fdopen 会 EBADF —— 让它走真实 fdopen */
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
    return real_fclose ? real_fclose(fp) : EOF;
}

int fseek(FILE *stream, long offset, int whence) {
    return real_fseek ? real_fseek(stream, offset, whence) : -1;
}

long ftell(FILE *stream) {
    return real_ftell ? real_ftell(stream) : -1;
}

void rewind(FILE *stream) {
    if (real_rewind) real_rewind(stream);
}

int fflush(FILE *stream) {
    return real_fflush ? real_fflush(stream) : EOF;
}
