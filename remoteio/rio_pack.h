/*
 * rio_pack.h —— 多虚拟文件 → 单远程大文件 合并层
 *
 * 服务端只有一个 pack 大文件（默认 /remote/pack.bin，可通过 RIO_PACK_FILE 宏覆盖）。
 * 各虚拟路径 → pack 内 offset/size 的映射在 rio_pack.c 里用静态数组声明。
 * 编译启用 —— 不需要 INI、不需要环境变量。
 */

#ifndef RIO_PACK_H
#define RIO_PACK_H

#include "remoteio.h"

#ifndef RIO_PACK_FILE
#define RIO_PACK_FILE  "/remote/pack.bin"
#endif

#ifndef RIO_PACK_PREFIX
#define RIO_PACK_PREFIX  "/virtual"
#endif

/* 一个虚拟文件 → pack 大文件里的一段 */
typedef struct {
    const char *vpath;     /* 虚拟路径：/virtual/config.txt */
    uint64_t    offset;    /* 在 pack_file 里的起始偏移 */
    uint64_t    size;      /* 该虚拟文件的最大长度（0=不限） */
} rio_pack_entry_t;

/* 全局只读配置 */
typedef struct {
    const char       *prefix;
    const char       *pack_file;
    const rio_pack_entry_t *entries;
    int               n_entries;
} rio_pack_cfg_t;

/* 在 constructor 里调一次（内部空操作，仅预留） */
void rio_pack_init(void);

/* 查虚拟路径是否在 pack 映射表里 */
const rio_pack_entry_t *rio_pack_lookup(const char *vpath);

/* 获取全局配置（只读） */
const rio_pack_cfg_t *rio_pack_cfg(void);

#endif /* RIO_PACK_H */
