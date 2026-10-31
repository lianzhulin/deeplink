/*
 * rio_pack.h — pack 多虚拟文件自描述模块（共享内存模式）
 *
 * pack 自描述格式（服务端初始化 16MB POSIX 共享内存段，段内包含头部）：
 *
 *   offset 0:   rio_pack_hdr_t (固定 256 字节)
 *   offset 256: rio_pack_entry_t[n_entries] (每项 144 字节)
 *   offset H+E: pack body (各虚拟文件实际内容)
 *
 * 每个 entry 的 offset 是 shm 内的**绝对偏移**（从 shm 基址算起）。
 *
 * 加载流程（懒加载）：
 *   第一次 rio_pack_lookup() 被调用时：
 *     → rio_do_shm_open_req() 让服务端确认 shm
 *     → 客户端 shm_open(RIO_PACK_SHM_NAME) + mmap
 *     → 从 shm 头部 memcpy 读 header + entries → 填充内部表
 *   之后所有虚拟文件 I/O 都是对本地 mmap 的 memcpy —— 零网络。
 *
 * 业务代码集成：
 *   业务代码零配置，直接 open("/virtual/config.txt") 即可。
 */

#ifndef RIO_PACK_H
#define RIO_PACK_H

#include "remoteio.h"
#include <stdint.h>

/* pack 头部魔数 + 版本 */
#define RIO_PACK_MAGIC     "RIOPACK!"
#define RIO_PACK_MAGIC_LEN 8
#define RIO_PACK_VERSION   1

/* ---- pack.bin 二进制格式 ---- */

/* pack 文件头（固定 256 字节）*/
typedef struct {
    char     magic[8];       /* "RIOPACK!" */
    uint32_t version;        /* 格式版本号，当前 = 1 */
    uint32_t n_entries;      /* 虚拟文件段数量 */
    uint64_t reserved;       /* 保留字段，置 0 */
    uint8_t  pad[232];       /* 填充到 256 字节 */
} rio_pack_hdr_t;            /* 8+4+4+8+232 = 256 */

/* 每个虚拟文件段描述（固定 144 字节） */
typedef struct {
    char     vpath[128];     /* 虚拟路径，\0 结尾，不足补 \0 */
    uint64_t offset;         /* 在 pack.bin 内的起始偏移（从文件头算起）*/
    uint64_t size;           /* 虚拟文件最大长度（0 = 不限）*/
} rio_pack_entry_t;

/* ---- 运行时 API ---- */

/* 覆盖默认 pack_file 路径（可选 —— 如果用默认 /remote/pack.bin 就不用调）*/
void rio_pack_set_pack_file(const char *pack_file);

/* 手动注册一个虚拟文件段（覆盖自动加载的映射 —— 调试或热修时用）*/
int rio_pack_register(const char *vpath, uint64_t offset, uint64_t size);

/* 清空所有映射表 */
void rio_pack_clear(void);

/* ---- 查询 API ---- */

/* 查虚拟路径是否已注册（首次调用时自动从 pack 懒加载）*/
const rio_pack_entry_t *rio_pack_lookup(const char *vpath);

/* shm 模式下 pack 文件路径已无意义（保留空实现，兼容旧代码）*/
const char *rio_pack_pack_file(void);

/* 是否已注册至少一个段 */
int rio_pack_is_enabled(void);

/* shm 模式下暴露给 libremoteio.c —— 客户端本地 mmap 基址 */
uint8_t *rio_pack_shm_base(void);
size_t   rio_pack_shm_size(void);

/* ---- 按需拉取 + 版本化 API ---- */

/* 通过 vpath 找 seg_idx（内部从 g_entries 线性搜索）*/
int rio_pack_find_seg_idx(const char *vpath);

/* 确保某段 body 已加载到本地 shm
 * 返回 0 成功，-1 失败
 * 内部做 OP_SEG_VERSION 检查 + 必要时 OP_SEG_READ 拉 body */
int rio_pack_ensure_seg(uint32_t seg_idx);

/* 标记某段 dirty（write 本地 shm 之后调用）*/
void rio_pack_mark_dirty(uint32_t seg_idx);

/* 把指定 seg 完整穿透到服务端 —— write/pwrite 后立刻调用保证实时可见 */
int rio_pack_push_seg(uint32_t seg_idx);

/* 把所有 dirty 段穿透写回服务端（close/flush 时调用 — 兜底）
 * 返回成功写回的段数（≥0），-1 表示网络故障（部分可能没写完）*/
int rio_pack_flush_dirty_segs(void);

/* ---- 内部 ---- */
void rio_pack_init(void);        /* constructor 调 */
int  rio_pack_autoload(void);    /* 懒加载入口：拉 meta + 本地 shm_create + mmap */

#endif /* RIO_PACK_H */
