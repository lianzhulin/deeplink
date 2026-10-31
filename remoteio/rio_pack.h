/*
 * rio_pack.h —— 多虚拟文件 → 单远程大文件 合并层
 *
 * 功能：客户端配置里声明多个虚拟路径（如 /virtual/config.txt），
 *       每个虚拟路径映射到"远程 pack 大文件 + offset + size"，
 *       LD_PRELOAD 劫持层自动把对虚拟文件的 open/read/write/seek
 *       翻译成对远程 pack 大文件的带偏移操作。
 *
 * 配置文件格式（INI）：
 *   [pack]
 *   prefix    = /virtual              ; 虚拟路径前缀（客户端访问路径）
 *   pack_file = /remote/pack.bin      ; 服务端单个大文件
 *
 *   [/virtual/config.txt]
 *   offset = 0                        ; 在 pack_file 里的起始偏移
 *   size   = 4096                     ; 该虚拟文件的最大长度
 *
 *   [/virtual/header.bin]
 *   offset = 4096
 *   size   = 256
 *
 * 环境变量 RIO_PACK_CONFIG 指定配置文件路径；未设则不启用 pack 模式，
 * 退化为原有的"直接远程文件"模式。
 */

#ifndef RIO_PACK_H
#define RIO_PACK_H

#include "remoteio.h"

#define RIO_PACK_MAX_ENTRIES  512
#define RIO_PACK_PATH_MAX     1024

/* 一个虚拟文件 → pack 大文件里的一段 */
typedef struct {
    int      valid;
    char     vpath[RIO_PACK_PATH_MAX];   /* 虚拟路径：/virtual/config.txt */
    uint64_t offset;                     /* 在 pack_file 里的起始偏移 */
    uint64_t size;                       /* 该虚拟文件的最大长度（0=不限） */
} rio_pack_entry_t;

/* 全局配置 */
typedef struct {
    int      enabled;                                /* 是否启用（加载到了配置文件） */
    char     prefix[RIO_PACK_PATH_MAX];              /* 虚拟路径前缀 */
    char     pack_file[RIO_PACK_PATH_MAX];           /* 合并后的远程大文件路径 */
    rio_pack_entry_t entries[RIO_PACK_MAX_ENTRIES];  /* 各虚拟文件映射 */
    int      n_entries;
} rio_pack_cfg_t;

/* 加载配置文件（通常在 constructor 里调用），返回 1=启用 0=不启用 */
int rio_pack_load(const char *path);

/* 在 constructor 里自动从 $RIO_PACK_CONFIG 或默认路径加载 */
void rio_pack_autoload(void);

/* 查虚拟路径是否在 pack 映射表里 */
const rio_pack_entry_t *rio_pack_lookup(const char *vpath);

/* 获取全局配置（只读访问） */
const rio_pack_cfg_t *rio_pack_cfg(void);

#endif /* RIO_PACK_H */
