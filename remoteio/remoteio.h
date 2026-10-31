/*
 * remoteio.h — LD_PRELOAD 远程文件 I/O 劫持库 公共接口
 *
 * 当前版本：**按需拉取 + 版本检测 + 写穿透**（跨机器 + 服务端数据实时可变）
 *
 * 核心流程：
 *   1) 客户端启动发 OP_PACK_META → 服务端返回 header + entries + seg_versions[]
 *      网络开销 ~3KB（16MB shm 的 99.98% 空间在 body，这里完全不拉）
 *   2) 客户端本地创建 16MB POSIX shm + mmap → 只把 header+entries 填进去
 *   3) 每段 body 按需拉：第一次 read 某段 → OP_SEG_READ(seg_idx) → 客户端本地 memcpy
 *   4) 每次 read 前先 OP_SEG_VERSION 检查服务端该段 version，变了就重拉
 *   5) write: 本地 shm 写 + dirty=1 → close/flush 时 OP_WRITE_SEG 穿透到服务端
 *   6) 之后所有虚拟文件 I/O 仍然是本地 memcpy —— 版本检测+按需拉只在必要时走 TCP
 *
 * 服务端侧：
 *   维护 g_shm_base (16MB) + seg_versions[n_segs]
 *   任何人修改某段 body 后 version++
 *   客户端发 OP_SEG_VERSION 就能发现
 *
 * 协议格式（二进制，网络字节序）：
 *   [4B magic][4B opcode][4B body_len][body...]
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
    OP_OPEN    = 1,
    OP_OPENAT  = 2,
    OP_READ    = 3,
    OP_WRITE   = 4,
    OP_CLOSE   = 5,
    OP_LSEEK   = 6,

    /* ====== 按需拉取 + 版本化 专用 ====== */
    OP_PACK_META  = 10,   /* 客户端拉 meta: 请求空; 响应 body = [4B n_entries][pack_hdr+entries][4B*n_versions] */
    OP_SEG_READ   = 11,   /* body: [4B seg_idx] → 响应: [8B version][seg_size 字节 body] */
    OP_SEG_VERSION= 12,   /* body: [4B seg_idx] → 响应: [8B version] (快速查询, 不拉 body) */
    OP_WRITE_SEG  = 13,   /* body: [4B seg_idx][4B data_len][data...] → 服务端写回 + version++ */

    /* 旧 OP_SHM_OPEN 保留做同机兼容（本方案不使用） */
    OP_SHM_OPEN = 10,      /* ← 注意: OP_PACK_META 取代了它的位置，这里仅保留文档 */

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

/* 服务端 shm 名称 — 客户端只通过 OP_PACK_META 读它的 header/entries，
 * 通过 OP_SEG_READ 读它的 seg body（不拉整个 shm）。
 * 注意：客户端**绝不直接 shm_open 这个名称**（会 O_TRUNC 破坏服务端 shm）！*/
#ifndef RIO_PACK_SHM_NAME_SRV
#define RIO_PACK_SHM_NAME_SRV  "/rio_packshm_srv"
#endif

/* 客户端本地 shm 名称 — 客户端自己 O_CREAT|O_TRUNC 这个，
 * 和服务端完全独立（同机也不会互相影响）。
 * 客户端只把 OP_PACK_META 拉到的 header+entries 写进去，
 * body 区域按需通过 OP_SEG_READ 拉取后填充。*/
#ifndef RIO_PACK_SHM_NAME
#define RIO_PACK_SHM_NAME  "/rio_packshm_client"
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
    uint32_t         seg_idx;       /* 对应 pack entry 下标（按需拉取 + 版本化用）*/
} rio_slot_t;

/* ---------- 连接管理 ---------- */
int  rio_connect(void);
void rio_disconnect(void);

/* ---------- 按需拉取 + 版本化 新协议 ---------- */

/*
 * OP_PACK_META: 拉 pack 元数据（header + entries + 每段 version 数组）
 * 响应格式:
 *   [4B n_entries][sizeof(rio_pack_hdr_t) 字节 hdr][sizeof(rio_pack_entry_t)*n_entries 字节 entries]
 *   [4B*n_entries 字节 seg_versions[]]
 * 调用方传入 out_meta_cap，成功返回实际 body_len（≤ out_meta_cap）。
 */
int rio_do_pack_meta(uint8_t *out_meta_buf, uint32_t out_meta_cap, uint32_t *out_meta_len);

/* OP_SEG_VERSION: 快速查询服务端某段当前 version（不拉 body） */
int rio_do_seg_version(uint32_t seg_idx, uint32_t *out_version);

/* OP_SEG_READ: 拉某段 body 内容
 * 响应: [8B version][seg_size 字节 body]
 * 调用方必须确保 out_buf 容量 ≥ seg_size（从 pack_entry_t.size 知道）。
 */
int rio_do_seg_read(uint32_t seg_idx, uint32_t seg_size,
                    uint8_t *out_buf, uint32_t out_cap, uint32_t *out_version);

/* OP_WRITE_SEG: 把某段 body 穿透写回服务端
 * 请求: [4B seg_idx][4B data_len][data...]
 * 服务端: memcpy(g_shm_base + entry.offset, data, data_len) + version++
 */
int rio_do_write_seg(uint32_t seg_idx, const uint8_t *data, uint32_t data_len);

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
                   uint64_t virtual_offset, uint64_t virtual_size,
                   uint32_t seg_idx);

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
