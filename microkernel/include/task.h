/*
 * task.h — 任务控制块 & 调度器
 *
 * 设计要点：
 *   - bitmask 就绪集：g_ready_mask 的 bit i = 1 表示 tid=i 在 READY 集
 *     入队/出队 = 一条位操作指令；选下一个 = tzcnt 硬件指令
 *   - 切换时只 swapcontext，开销 = 一次寄存器保存 + 一次恢复
 *   - 空闲任务 idler (tid=0) 永远在 mask 里，保证调度器不空
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

    /* 睡眠唤醒 tick（时钟任务会检查并把 sleeping -> ready） */
    uint64_t           wake_tick;

    /* ---- IPC 硬化版阻塞状态 ----
     * 两种独立的阻塞原因，精确隔离，互不污染：
     *
     *   ipc_send_wait = true: send() 投出了消息，等对方 reply 解
     *   ipc_recv_wait = true: receive() 队空阻塞，等 send/reply 投递解
     *
     * 为什么没有 space_wait（队列满阻塞）？
     *   send 是同步的 → 每个任务同时最多 1 条 inflight send。
     *   QUEUE_SIZE = MAX_TASKS → 一个 mailbox 最多积压 MAX_TASKS-1 条 send
     *   （其他所有任务各 1 条），永远差 1 格。
     *   背压是死代码，删。不变量：同步 send + N 槽 ≥ N-1 积压。 */
    volatile bool      ipc_send_wait;
    volatile bool      ipc_recv_wait;

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
