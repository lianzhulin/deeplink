/*
 * remoteio.h — LD_PRELOAD 远程文件 I/O 劫持库 公共接口
 *
 * 当前版本仅支持 pack 模式：
 *   业务代码在运行时通过 rio_pack_register() 注册虚拟路径 → offset/size 映射，
 *   libremoteio 将对虚拟路径的 open/read/write/lseek 翻译为对服务端 pack_file 的
 *   带偏移操作。
 *
 * 协议格式（二进制，网络字节序）：
 *   [4B magic][4B opcode][4B body_len][body...]
 *
 * 请求操作码：
 *   OP_OPEN    = 1   body: [4B path_len][path...][4B flags][4B mode]
 *   OP_OPENAT  = 2   body: [4B dirfd][4B path_len][path...][4B flags][4B mode]
 *   OP_READ    = 3   body: [4B handle][4B count]
 *   OP_WRITE   = 4   body: [4B handle][4B count][data...]
 *   OP_CLOSE   = 5   body: [4B handle]
 *   OP_LSEEK   = 6   body: [4B handle][8B offset][4B whence]
 *
 * 响应格式：
 *   OP_ACK / OP_ERR  通用：[8B ret_val]
 *   OP_READ 特例     [4B ret_val][data...]
 */

#ifndef REMOTEIO_H
#define REMOTEIO_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <fcntl.h>

/* ---------- 协议常量 ---------- */
#define RIO_MAGIC   0x52494F31u   /* "RIO1" */

enum {
    OP_OPEN   = 1,
    OP_OPENAT = 2,
    OP_READ   = 3,
    OP_WRITE  = 4,
    OP_CLOSE  = 5,
    OP_LSEEK  = 6,
    OP_SHM_OPEN = 10,            /* 新增：客户端请求服务端初始化 shm 段 */
    /* 服务端响应 */
    OP_ACK    = 0x80,
    OP_ERR    = 0x81,
};

/* ---------- 远程连接配置 ---------- */
#ifndef RIO_REMOTE_HOST
#define RIO_REMOTE_HOST  "127.0.0.1"
#endif
#ifndef RIO_REMOTE_PORT
#define RIO_REMOTE_PORT  9527
#endif
#ifndef RIO_REMOTE_PREFIX
#define RIO_REMOTE_PREFIX  "/remote/"
#endif

/* ---------- 共享内存 pack 配置 ---------- */
#ifndef RIO_PACK_SHM_NAME
#define RIO_PACK_SHM_NAME  "/rio_packshm"   /* POSIX shm_open 名称 */
#endif
#ifndef RIO_PACK_SHM_SIZE
#define RIO_PACK_SHM_SIZE  (16 * 1024 * 1024)   /* 16 MB 固定大小 */
#endif

/* ---------- 协议包头 ---------- */
typedef struct {
    uint32_t magic;
    uint32_t opcode;
    uint32_t body_len;
} rio_hdr_t;

/* ---------- 假 fd 管理 ---------- */
/*
 * 客户端拿到的"假 fd"编码规则：
 *   RIO_FAKE_FD_BASE (默认 10000) 及以上为 pack 模式远程 fd
 *   < base 的走本地真实文件
 */
#define RIO_FAKE_FD_BASE  10000

/* 最多同时打开的远程文件数 */
#define RIO_MAX_REMOTE_FDS  4096

/* 服务端侧的文件句柄编号 */
typedef uint32_t remote_handle_t;

/* 映射表记录：假 fd → 共享内存段 + 虚拟位置
 *
 * 所有远程 fd 都是 pack 模式：
 *   shm_base    —— 客户端 mmap 到的 16MB 共享内存段基址
 *   shm_size    —— shm 段总大小（固定 RIO_PACK_SHM_SIZE）
 *   virtual_off —— pack 段在 shm 里的起始偏移（rio_pack_entry_t.offset）
 *   virtual_sz  —— 虚拟文件最大长度（rio_pack_entry_t.size）
 *   local_pos   —— 该 fd 当前虚拟位置（open 时初始 0）
 *
 * read/write: memcpy  buf <-> shm_base[virtual_off + local_pos]
 * lseek:     只更新 local_pos，不动 shm
 */
typedef struct {
    int              used;
    uint8_t         *shm_base;      /* mmap 基址 */
    size_t           shm_size;      /* mmap 大小 */
    uint64_t         virtual_off;   /* pack 段起始偏移 */
    uint64_t         virtual_sz;    /* 虚拟文件最大长度 */
    uint64_t         local_pos;     /* 该 fd 当前位置 */
} rio_slot_t;

/* ---------- 连接管理 ---------- */
int  rio_connect(void);
void rio_disconnect(void);

/* OP_SHM_OPEN: 请求服务端初始化 shm 段，返回 0 成功 */
int  rio_do_shm_open_req(const char *shm_name);

/* （保留的通用协议函数，shm 模式下不使用） */
int  rio_do_open(const char *path, int flags, mode_t mode, remote_handle_t *out);
int  rio_do_openat(int dirfd, const char *path, int flags, mode_t mode, remote_handle_t *out);
ssize_t rio_do_read (remote_handle_t rh, void *buf, size_t count);
ssize_t rio_do_write(remote_handle_t rh, const void *buf, size_t count);
int  rio_do_close(remote_handle_t rh);
off_t  rio_do_lseek(remote_handle_t rh, off_t offset, int whence);

/* ---------- 映射表操作 ---------- */

/* 分配 pack 模式假 fd */
int  rio_map_alloc(uint8_t *shm_base, size_t shm_size,
                   uint64_t virtual_offset, uint64_t virtual_size);

int  rio_map_is_remote(int fd);
const rio_slot_t *rio_map_slot(int fd);
void rio_map_set_pos(int fd, uint64_t pos);
uint64_t rio_map_get_pos(int fd);
void rio_map_free(int fd);

/* ---------- 日志 ---------- */
#ifdef RIO_DEBUG
#define RIO_LOG(fmt, ...) fprintf(stderr, "[libremoteio] " fmt "\n", ##__VA_ARGS__)
#else
#define RIO_LOG(fmt, ...) ((void)0)
#endif

#endif /* REMOTEIO_H */
