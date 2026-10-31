/*
 * remoteio.h — LD_PRELOAD 远程文件 I/O 劫持库 公共接口
 *
 * 当前版本仅支持 pack 模式（跨机器全量同步 + 本地 memcpy）：
 *   服务端启动时创建 16MB POSIX 共享内存 (RIO_PACK_SHM_NAME)，
 *   写入自描述 pack 格式头部 (rio_pack_hdr_t + entries + body)。
 *   客户端首次 open() 虚拟路径时：
 *     → OP_SHM_OPEN 请求服务端 dump 整个 16MB shm 到 TCP 响应
 *     → 客户端本地创建同名 POSIX shm + mmap + memcpy 写入
 *     → 之后所有虚拟文件 I/O 都是本地 memcpy —— 零网络、零磁盘。
 *   业务代码零配置，直接 open("/virtual/config.txt") 即可。
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
 *   OP_SHM_OPEN = 10 body: [4B name_len][shm_name\0]  ← dump 整个 shm
 *
 * 响应格式：
 *   OP_ACK / OP_ERR  通用: [8B ret_val]  可选追加 body
 *   OP_SHM_OPEN 特例: [8B shm_size][shm_size 字节 pack 数据]
 *
 * 关键架构约束：
 *   - POSIX 共享内存是单机内核对象，跨机器**无法共享**
 *   - 所以"同机模式下客户端直接 shm_open + mmap 同一段"不可行
 *   - 改为：一次 TCP 全量同步 16MB → 客户端本地 shm + memcpy
 *   - 之后零网络零磁盘，与同机模式的 I/O 层行为完全一致
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

/* OP_SHM_OPEN: 从远程服务端全量下载 pack 数据（服务端 dump 它的 16MB shm）
 * 成功时 *out_pack_data 是 malloc 的 buffer（调用方 free），*out_pack_size 是长度 */
int  rio_do_shm_open_req(const char *shm_name,
                         uint8_t **out_pack_data, uint32_t *out_pack_size);

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
