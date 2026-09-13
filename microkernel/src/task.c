/*
 * task.c — TCB 数组、上下文切换、bitmask 调度器
 *
 * 切换开销 = swapcontext() 一次（寄存器保存 + 恢复）
 * 调度开销 = 入队/出队 一条位指令；选下一个 = tzcnt 硬件指令（x86-64）
 */
#define _GNU_SOURCE
#include <ucontext.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "kernel.h"
#include "task.h"
#include "ipc.h"

/* ---- TCB 数组：静态分配，最多 32 个 ---- */
static mk_tcb_t   g_tasks[MK_MAX_TASKS];

/* 每个任务的 entry/arg，由 mk_task_create 填，mk_task_wrapper 取 */
typedef struct {
    mk_task_entry_t entry;
    void           *arg;
} mk_entry_info_t;
static mk_entry_info_t g_entries[MK_MAX_TASKS];

/* 调度器状态 */
static uint8_t     g_current   = MK_MAX_TASKS;   /* 未启动 */
static uint32_t    g_ready_mask = 0;              /* bit i=1 表示 tid=i 在 READY 集 */

volatile uint64_t mk_ticks = 0;

/* ---- trampoline：被 makecontext 调起 ----
 * 调度器在 swapcontext 前已经把 g_current 设成 next 的 tid，
 * 所以 wrapper 里直接 mk_current_tid() 就能拿到自己的 tid。
 * 完全不依赖 makecontext 的参数传递。 */
static void mk_task_wrapper(void)
{
    int tid = (int)mk_current_tid();
    if (tid < 0 || tid >= MK_MAX_TASKS) {
        fprintf(stderr, "mk_task_wrapper: bad tid=%d (g_current not set?)\n", tid);
        abort();
    }
    mk_entry_info_t *info = &g_entries[tid];

    if (info->entry) {
        info->entry(info->arg);
    }
    mk_task_exit();     /* 任务函数返回 = 自杀 */
}

/* ---- 初始化 ---- */
void mk_task_init(void)
{
    memset(g_tasks, 0, sizeof(g_tasks));
    memset(g_entries, 0, sizeof(g_entries));
    for (int i = 0; i < MK_MAX_TASKS; ++i) {
        g_tasks[i].tid = (uint8_t)i;
        g_tasks[i].state = MK_TASK_DEAD;
    }
    g_current   = MK_MAX_TASKS;
    g_ready_mask = 0;
}

/* ---- 创建任务 ---- */
int mk_task_create(const char *name, mk_task_entry_t entry,
                   void *arg, size_t stack_size, uint8_t prio)
{
    if (!entry) return MK_ERR_INVALID;
    if (stack_size == 0) stack_size = MK_DEFAULT_STACK_SIZE;
    if (stack_size < 8192) stack_size = 8192;

    /* 找一个 DEAD 槽位 */
    int tid = -1;
    for (int i = 0; i < MK_MAX_TASKS; ++i) {
        if (g_tasks[i].state == MK_TASK_DEAD) {
            tid = i;
            break;
        }
    }
    if (tid < 0) return MK_ERR_NOMEM;

    mk_tcb_t *tcb = &g_tasks[tid];

    /* 分配栈 —— posix_memalign 保证 16 字节对齐，符合 makecontext 要求 */
    void *stack = NULL;
    if (posix_memalign(&stack, 16, stack_size) != 0) {
        return MK_ERR_NOMEM;
    }

    memset(tcb, 0, sizeof(*tcb));
    tcb->tid         = (uint8_t)tid;
    tcb->prio        = prio;
    tcb->state       = MK_TASK_DEAD;   /* mk_sched_ready 里改成 READY */
    tcb->stack       = stack;
    tcb->stack_size  = stack_size;
    tcb->wake_tick   = 0;
    strncpy(tcb->name, name ? name : "anon", sizeof(tcb->name) - 1);

    /* makecontext：entry 先存起来，wrapper 里调 */
    g_entries[tid].entry = entry;
    g_entries[tid].arg   = arg;

    getcontext(&tcb->ctx);
    tcb->ctx.uc_stack.ss_sp   = stack;
    tcb->ctx.uc_stack.ss_size = stack_size;
    tcb->ctx.uc_link          = NULL;

    makecontext(&tcb->ctx, (void (*)(void))mk_task_wrapper, 0);

    /* 置 READY 位 + 改 state */
    mk_sched_ready((uint8_t)tid);

    return tid;
}

/* ---- 自杀 ---- */
void mk_task_exit(void)
{
    /* 防御：g_current 边界检查 —— 正常路径不会到这里，
     * 但 exit 是收尾函数，调度器 race 或上下文错乱时要兜底 */
    if (g_current >= MK_MAX_TASKS) {
        fprintf(stderr, "mk_task_exit: g_current=%u out of range, abort\n", g_current);
        abort();
    }

    uint8_t tid = g_current;
    mk_tcb_t *tcb = &g_tasks[tid];

    /* ---- P0-1 硬化：IPC cleanup 必须在任何资源释放之前 ----
     * 我的 inflight[] 里可能有还在等 reply 的 client。
     * 如果我先把自己从 ready_mask 拿掉、free stack，
     * 那些 client 的 send_wait 永远不会被解 → 永久 BLOCKED。
     * cleanup 会：
     *   1. 扫 inflight[] → 给每个 client 推 synthetic error reply → wake_send_waiter
     *   2. 清空 mailbox 队列残留（还没被 receive 的 send 消息） */
    mk_ipc_cleanup_dead_service(tid);

    /* 然后正常收尾 */
    tcb->state = MK_TASK_DEAD;
    if (tcb->stack) {
        free(tcb->stack);
        tcb->stack = NULL;
    }
    g_entries[tid].entry = NULL;
    g_entries[tid].arg   = NULL;

    mk_sched_unready(tid);

    /* 切下一个（必须成功，因为 idler 永在 mask 里） */
    mk_sched_tick();
}

/* ---- yield ---- */
void mk_task_yield(void)
{
    mk_sched_tick();
}

/* ---- sleep ---- */
void mk_task_sleep(uint64_t ticks)
{
    if (ticks == 0) { mk_task_yield(); return; }

    uint8_t tid = g_current;
    mk_tcb_t *tcb = &g_tasks[tid];

    tcb->wake_tick = mk_ticks + ticks;
    tcb->state     = MK_TASK_SLEEPING;
    mk_sched_unready(tid);

    mk_sched_tick();
}

/* ---- 查询 ---- */
uint8_t  mk_current_tid(void) { return g_current; }
mk_tcb_t *mk_current(void)    { return &g_tasks[g_current]; }
mk_tcb_t *mk_tcb_get(uint8_t tid) {
    if (tid >= MK_MAX_TASKS) return NULL;
    return &g_tasks[tid];
}

/* ---- 就绪集操作（bitmask 版） ----
 *
 *  入队: g_ready_mask |= (1u << tid)     —— 一条位指令
 *  出队: g_ready_mask &= ~(1u << tid)    —— 一条位指令
 *  选下一个: tzcnt (硬件指令)
 *
 *  约束：idler (tid=0) 永远在 mask 里，mask 永不为 0。 */

void mk_sched_ready(uint8_t tid)
{
    g_ready_mask |= (1u << tid);
    if (g_tasks[tid].state != MK_TASK_RUNNING) {
        g_tasks[tid].state = MK_TASK_READY;
    }
}

void mk_sched_unready(uint8_t tid)
{
    g_ready_mask &= ~(1u << tid);
}

/* ---- 从 current 之后选下一个 READY 任务 ----
 *
 *  思路：把 mask 右移 (current+1) 位，低位就是 "current 之后第一个 READY"。
 *  如果右移后全 0，说明要 wrap 到最低位。
 *  用 __builtin_ctz（count trailing zeros）—— x86 上是硬件 tzcnt/bsf 指令。
 *  当 current == 31 时，(current+1)==32 左移 32 位在 C 里是 UB，所以单独处理。 */
static uint8_t pick_next(void)
{
    uint32_t mask = g_ready_mask;
    if (mask == 0) return MK_TID_IDLER;

    uint8_t cur = g_current;

    /* 移走 current+1 个低 bits，剩下的就是 "current 之后" 的 READY */
    if (cur + 1 >= 32) {
        /* cur==31：之后就是最低位，wrap */
        return (uint8_t)__builtin_ctz(mask);
    }

    uint32_t after = mask & ~((1u << (cur + 1)) - 1u);
    if (after) {
        return (uint8_t)__builtin_ctz(after);
    }
    /* wrap */
    return (uint8_t)__builtin_ctz(mask);
}

/* ---- tick：yield / sleep / IPC block 等调度点调用 ----
 *
 *  只负责挑下一个 + swapcontext。热路径，零分支判断（除了 prev==next）。 */
void mk_sched_tick(void)
{
    uint8_t next = pick_next();
    uint8_t prev = g_current;
    if (prev == next) return;

    /* prev 如果是 RUNNING（正常 yield），状态转 READY；
     * 如果是 SLEEPING/BLOCKED（sleep/IPC 自己设的），不动 */
    if (prev != MK_MAX_TASKS && g_tasks[prev].state == MK_TASK_RUNNING) {
        g_tasks[prev].state = MK_TASK_READY;
    }
    g_tasks[next].state = MK_TASK_RUNNING;

    g_current = next;
    swapcontext(&g_tasks[prev].ctx, &g_tasks[next].ctx);
    /* 从 swapcontext 返回时，我们已经在 prev 的下一次调度里被换回 */
}

/* ---- 启动调度器 ---- */
void mk_sched_run(void)
{
    if (g_current == MK_MAX_TASKS) {
        /* 找 mask 里最低位（通常就是 tid=0 idler） */
        uint8_t first;
        if (g_ready_mask) {
            first = (uint8_t)__builtin_ctz(g_ready_mask);
        } else {
            /* 兜底：确保 idler 在 mask 里 */
            first = MK_TID_IDLER;
            mk_sched_ready(first);
        }

        g_tasks[first].state = MK_TASK_RUNNING;
        g_current = first;

        /* 第一次跑，没有 prev 的 ctx 要 swap，直接 setcontext */
        setcontext(&g_tasks[first].ctx);
        abort();
    }
    mk_sched_tick();
}
