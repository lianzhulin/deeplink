/*
 * rio_pack.h — 多虚拟文件 → 单远程大文件 合并层
 *
 * 设计原则：libremoteio.so 本身**不携带任何** pack.bin 内部布局信息。
 * 具体的 虚拟路径 → offset/size 映射由外部（业务代码）通过
 * rio_pack_register() 在运行时注入。
 *
 * 典型用法（业务程序 main 开头）:
 *   rio_pack_set_pack_file("/remote/pack.bin");
 *   rio_pack_register("/virtual/config.txt", 0,    64);
 *   rio_pack_register("/virtual/header.bin", 64,   128);
 *   rio_pack_register("/virtual/records.log", 192, 256);
 * 然后 open("/virtual/config.txt") 自动走 pack 模式。
 */

#ifndef RIO_PACK_H
#define RIO_PACK_H

#include "remoteio.h"

/* 服务端合并后的 pack 大文件默认路径 —— 仅作为 fallback，
 * 业务代码应通过 rio_pack_set_pack_file() 运行时覆盖 */
#ifndef RIO_PACK_FILE
#define RIO_PACK_FILE  "/remote/pack.bin"
#endif

/* 一个虚拟文件 → pack 大文件里的一段 */
typedef struct {
    const char *vpath;     /* 虚拟路径：/virtual/config.txt */
    uint64_t    offset;    /* 在 pack_file 里的起始偏移 */
    uint64_t    size;      /* 该虚拟文件最大长度（0 = 不限）*/
} rio_pack_entry_t;

/* ---- 运行时注册 API ---- */

/* 覆盖默认 pack_file 路径（业务代码通常只需要调一次）*/
void rio_pack_set_pack_file(const char *pack_file);

/* 注册一个虚拟文件段。返回 0=成功，-1=dup 或内存不足 */
int rio_pack_register(const char *vpath, uint64_t offset, uint64_t size);

/* 清空所有已注册的段（测试或热重载时用）*/
void rio_pack_clear(void);

/* ---- 查询 API ---- */

/* 查虚拟路径是否在已注册的映射表里，返回 NULL 表示未注册 */
const rio_pack_entry_t *rio_pack_lookup(const char *vpath);

/* 获取当前 pack_file 路径（只读）*/
const char *rio_pack_pack_file(void);

/* 是否已注册至少一个段 */
int rio_pack_is_enabled(void);

/* constructor 内部自动调，业务代码不需要调 */
void rio_pack_init(void);

#endif /* RIO_PACK_H */
