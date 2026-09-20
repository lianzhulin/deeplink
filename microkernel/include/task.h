/*
 * task.h — 任务控制块 & 调度器
 *
 * 设计要点：
 *   - bitmask 就绪集：g_ready_mask 的 bit i = 1 表示 tid=i 在 READY 集
 *     入队/出队 = 一条位操作指令；选下一个 = tzcnt 硬件指令
 *   - L2 轻量上下文：mk_light_ctx_t 只有 56B（callee-saved 6 个寄存器 + rsp），
 *     对比 glibc ucontext_t 的 968B —— 省掉 FPU/SSE 状态、signal mask、
 *     uc_link/uc_stack 等内核根本不关心的字段
 *   - 空闲任务 idler (tid=0) 永远在 mask 里，保证调度器不空
 */
#ifndef MK_TASK_H
#define MK_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"

/* ---- L2: 轻量上下文（x86-64） ----
 *
 * SysV ABI callee-saved 寄存器：rbx, rbp, r12, r13, r14, r15
 * 加上 rsp。返回地址通过 ret 指令从栈弹出，不单独存。
 * 合计 7 × 8 = 56 字节。 */
typedef struct mk_light_ctx {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
} mk_light_ctx_t;

/* ---- TCB 硬化：前后魔数 ----
 * 两个魔数夹着整个 TCB，任何方向的野指针/栈溢出覆盖都会破坏其中一个。
 * 调度器每 tick 检查一次，发现 mismatch 就 abort 而不是 silent corruption。 */
#define MK_TCB_CANARY_HEAD  0xCAFEBABEDEADBEEFULL
#define MK_TCB_CANARY_TAIL  0x0BADF00DCAFEFACEULL

/* ---- TCB ---- */
typedef struct mk_tcb {
    uint64_t           canary_head;    /* 硬化：前魔数 */

    uint8_t            tid;
    uint8_t            prio;          /* 预留给后续扩展；当前调度器不使用 */
    volatile mk_task_state_t state;
    char               name[16];

    mk_light_ctx_t     ctx;           /* L2: 56B 轻量寄存器上下文 */
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

    uint64_t           canary_tail;    /* 硬化：后魔数 */
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

/* 硬化：状态机 helper + canary 检查。所有 state 写必须走它。 */
void    mk_tcb_set_state(mk_tcb_t *tcb, mk_task_state_t new_state);

/* ---- L2: 轻量上下文原语（汇编实现） ---- */

/* 初始化一个新上下文：entry 是任务入口函数，
 * stack + stack_size 是栈内存（调用者已分配好）。
 * 把 entry 压入栈作为 return address，callee-saved 寄存器清零。 */
void mk_ctx_init(mk_light_ctx_t *ctx, void (*entry)(void),
                 void *stack, size_t stack_size);

/* 切换上下文：保存当前到 old，恢复并跳转到 new。
 * 相当于 swapcontext(old, new)。 */
void mk_ctx_swap(mk_light_ctx_t *old_ctx, mk_light_ctx_t *new_ctx);

/* 加载并跳转到 ctx。只恢复不保存。
 * 相当于 setcontext(ctx)。 */
void mk_ctx_load(mk_light_ctx_t *ctx);

/* 调度器内部：把 tid 加/移出调度链 */
void mk_sched_ready(uint8_t tid);
void mk_sched_unready(uint8_t tid);

/* 强制切换到下一个 ready 任务（调度器内部 + 时钟 tick 用） */
void mk_sched_tick(void);

/* 启动调度器，从 idler 开始跑。永不返回。 */
void mk_sched_run(void) __attribute__((noreturn));

#endif /* MK_TASK_H */
