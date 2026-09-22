/*
 * ============================================================================
 *   kernel.h — 微内核原型的公共常量、类型、错误码定义
 * ============================================================================
 *
 *   【架构角色】内核的"宪法文件"。定义所有模块共享的基础常量
 *              (MK_MAX_TASKS、TID 约定、错误码、IRQ bit)。
 *              被 task.h / ipc.h / mm.h / irq.h 全部 include，
 *              本身不依赖任何其他模块头文件 (只依赖 stdint/stdbool)。
 *   【硬化要点】TID 硬约束用 #define 编码成编译期常量，
 *              错误码集中在 mk_err_t enum，避免 magic number 散落。
 *   【性能要点】无热路径；全部是编译期常量。
 *   【不变量】MK_TID_BITS=5 → MK_MAX_TASKS=32 → 所有 tid 必须 < 32；
 *              MK_TID_IDLER=0 永远合法。
 *   【约束】TID 范围 0~31；错误码 < 0 为失败。
 *   【数据结构】mk_task_state_t (5 态 enum)、mk_err_t (7 错误码 enum) —— 无对齐要求。
 *   【线程安全】纯常量定义，无并发写。

 * ============================================================================
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
