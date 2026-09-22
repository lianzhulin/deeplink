/*
 * ============================================================================
 *   mm.h — 内存管理模块接口（作为独立任务通过 IPC 被调用）
 * ============================================================================
 *
 *   【架构角色】提供内存分配/释放/查询的便捷封装。mm 是一个普通任务，
 *              不持有内核特殊权限；其他任务想分配/释放内存，
 *              必须发 IPC 给它。依赖 kernel.h / ipc.h。
 *   【硬化要点】mm 任务通过 IPC 的 reply 机制保证每次请求必定收到 ack
 *              才返回（mm 对未知 tag 也会 reply 解对端 send 的阻塞）；
 *              指针拆成两个 int32 在 data[0..1] 里传输，避免 64-bit 端序问题。
 *   【性能要点】mm 任务内部是 while(1){receive; switch(tag)} 循环，
 *              服务者模型保证串行、无锁；mm 自己直接调 arena_alloc 不走 IPC。
 *   【不变量】mk_alloc / mk_free 对 tid==MK_TID_MM 的调用走本地路径，
 *              对其他 tid 走 IPC 路径，两条路径行为一致。
 *   【约束】mk_free 必须传 mk_alloc 返回的原始指针；调用者必须保证
 *              mk_alloc 返回的指针最终被 mk_free 释放（否则 arena 永久占块）。
 *   【数据结构】无；tag 常量定义 (MK_MM_TAG_ALLOC=0x4D41 / FREE / QUERY)。
 *   【线程安全】mm 任务内的 arena 操作由任务上下文串行保证；
 *              其他任务只通过 IPC 同步调用。

 * ============================================================================
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
