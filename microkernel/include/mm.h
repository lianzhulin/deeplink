/*
 * mm.h — 内存管理（作为独立任务，通过 IPC 被调用）
 *
 * 关键约束：**mm 是一个普通任务，不持有内核特殊权限**。
 * 其他任务想分配/释放内存，必须发 IPC 给它。
 *
 * 这体现了微内核"策略独立于机制"的核心思想：
 *   - 内核只提供最小的地址空间原语（当前原型共用一个进程地址空间，简化了）
 *   - 分配策略（首次适应 / 最佳适应）在 mm 任务里随便换
 */
#ifndef MK_MM_H
#define MK_MM_H

#include "kernel.h"
#include "ipc.h"

/* IPC tag 定义 —— mm 任务识别这些 tag */
#define MK_MM_TAG_ALLOC   0x4D41    /* 'MA' */
#define MK_MM_TAG_FREE    0x4D46    /* 'MF' */
#define MK_MM_TAG_QUERY   0x4D51    /* 'MQ' */

/* 启动 mm 任务，内部实现 */
void mk_mm_start(void);

/* 便捷封装：任务发 IPC 给 mm 分配内存 */
void *mk_alloc(size_t size);

/* 便捷封装：释放 */
void  mk_free(void *ptr);

/* 查询 mm 状态，填到 out_buf（data[0]=total, data[1]=used, data[2]=free） */
mk_err_t mk_mm_query(int32_t out_buf[3]);

#endif /* MK_MM_H */
