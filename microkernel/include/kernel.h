/*
 * kernel.h — 公共常量、类型、错误码
 *
 * 整个微内核原型的基础约定都在这里。
 */
#ifndef MK_KERNEL_H
#define MK_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- 硬约束：最多 32 个任务 ---- */
#define MK_MAX_TASKS   32
#define MK_TID_BITS    5               /* 2^5 = 32 */
#define MK_TID_MASK    (MK_MAX_TASKS - 1)

/* ---- TID 约定 ---- */
#define MK_TID_IDLER   0               /* 空闲任务，永不阻塞 */
#define MK_TID_CLOCK   1               /* 时钟下半部任务 */
#define MK_TID_MM      2               /* 内存管理任务 */
#define MK_TID_FIRST_USER 3            /* 第一个用户任务 */

/* ---- 默认栈大小 ---- */
#define MK_DEFAULT_STACK_SIZE  (64 * 1024)

/* ---- 任务状态 ---- */
typedef enum {
    MK_TASK_DEAD     = 0,
    MK_TASK_READY    = 1,
    MK_TASK_RUNNING  = 2,
    MK_TASK_BLOCKED  = 3,             /* 等 IPC / 等事件 */
    MK_TASK_SLEEPING = 4              /* 等时钟 tick */
} mk_task_state_t;

/* ---- 错误码 ---- */
typedef enum {
    MK_OK           =  0,
    MK_ERR_INVALID  = -1,
    MK_ERR_NOMEM    = -2,
    MK_ERR_NOIPC    = -3,
    MK_ERR_TIMEOUT  = -4,
    MK_ERR_BUSY     = -5,
    MK_ERR_EXIST    = -6
} mk_err_t;

/* ---- 事件 / 软中断 ----
 * 中断上半部就是置这些位，下半部任务自己跑。 */
#define MK_IRQ_CLOCK   (1u << 0)
#define MK_IRQ_IOBASE  (1u << 1)     /* 预留给设备中断 */

/* ---- tick 计数（时钟 tick，由上半部累加） ---- */
extern volatile uint64_t mk_ticks;

/* ---- 启动入口，main.c 里实现 ---- */
void mk_kernel_start(void);

#endif /* MK_KERNEL_H */
