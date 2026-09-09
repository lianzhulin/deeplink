/*
 * task.h — 任务控制块 & 调度器
 *
 * 设计要点：
 *   - 环形单向链表调度，O(1) 切换
 *   - 切换时只 swapcontext，开销 = 一次寄存器保存 + 一次寄存器恢复
 *   - 空闲任务 idler 永远 READY，保证调度器不空
 */
#ifndef MK_TASK_H
#define MK_TASK_H

#include <ucontext.h>
#include "kernel.h"

/* ---- TCB ---- */
typedef struct mk_tcb {
    uint8_t            tid;
    uint8_t            prio;          /* 预留给后续扩展；当前调度器不使用 */
    volatile mk_task_state_t state;
    char               name[16];

    ucontext_t         ctx;           /* 寄存器上下文 + 栈 */
    void              *stack;
    size_t             stack_size;

    /* 环形调度链 */
    uint8_t            next;          /* 下一个 ready 任务的 tid */

    /* 睡眠唤醒 tick（时钟任务会检查并把 sleeping -> ready） */
    uint64_t           wake_tick;

    /* IPC 相关状态（ipc.c 自己填充，这里只预留） */
    uint8_t            ipc_waiter;    /* 等谁 reply，或谁在等我 reply */
    volatile bool      ipc_blocked;
    volatile bool      ipc_has_msg;   /* 收件箱是否有未读消息 */

    /* 事件 bitmap（中断上半部置位，任务自己 poll） */
    volatile uint32_t  irq_pending;
} mk_tcb_t;

/* ---- 任务入口函数签名 ---- */
typedef void (*mk_task_entry_t)(void *arg);

/* ---- 公共 API ---- */

/* 初始化子系统；必须先于一切 mk_task_create 调用 */
void mk_task_init(void);

/* 创建任务，返回 tid（>=0）或 mk_err_t（<0）。
 * flags 当前未使用，预留。 */
int  mk_task_create(const char *name, mk_task_entry_t entry,
                    void *arg, size_t stack_size, uint8_t prio);

/* 自杀自己 */
void mk_task_exit(void) __attribute__((noreturn));

/* 主动让出 CPU（yield），调度下一个 ready 任务 */
void mk_task_yield(void);

/* 睡眠若干 tick（由时钟任务唤醒）。
 * 调用者会从 READY 转到 SLEEPING，醒来后转回 READY。 */
void mk_task_sleep(uint64_t ticks);

/* 获取当前任务 tid */
uint8_t mk_current_tid(void);
mk_tcb_t *mk_current(void);
mk_tcb_t *mk_tcb_get(uint8_t tid);

/* 调度器内部：把 tid 加/移出调度链 */
void mk_sched_ready(uint8_t tid);
void mk_sched_unready(uint8_t tid);

/* 强制切换到下一个 ready 任务（调度器内部 + 时钟 tick 用） */
void mk_sched_tick(void);

/* 启动调度器，从 idler 开始跑。永不返回。 */
void mk_sched_run(void) __attribute__((noreturn));

#endif /* MK_TASK_H */
