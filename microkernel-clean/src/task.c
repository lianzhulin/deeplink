/*
 * ============================================================================
 *   task.c — TCB 数组、L2 轻量上下文切换、bitmask 循环调度器实现
 * ============================================================================
 *
 *   【实现要点】TCB 用静态数组 g_tasks[32]，每 task 一个槽。
 *              调度器是 uint32_t g_ready_mask 位掩码 + __builtin_ctz 找下一个。
 *              pick_next 从 g_current+1 开始找 READY 任务（循环调度），
 *              wrap 时回最低位。mk_ctx_swap / mk_ctx_load / mk_ctx_init 是
 *              汇编原语（在 ctx.S 里），这里只负责安全校验。
 *   【硬化实现】TCB 前后 canary (0xCAFEBABEDEADBEEF / 0x0BADF00DCAFEFACE)，
 *              tcb_check_canary 每 tick 对 idler 必查一次；
 *              状态机白名单 g_valid_trans 5×5 查表，所有 state 写必须走
 *              mk_tcb_set_state（canary 检查 + 状态转换合法性）；
 *              切换前 tcb_check_ctx_integrity 验证 rsp 在 [stack, stack+size) 内；
 *              mk_task_exit 先调 mk_ipc_cleanup_dead_service 解所有阻塞 client，
 *              再 free stack、切下一个（顺序严格，反了会永久 BLOCKED）。
 *   【热路径】mk_sched_tick（yield/sleep/IPC block 全指向它）、
 *              mk_sched_ready / mk_sched_unready（IPC send/recv 唤醒时调）、
 *              pick_next（每 tick 一次）。全部是 O(1)。
 *   【错误处理】canary 破坏 / 非法状态转换 / rsp 越界 / g_current 越界
 *              → fprintf + abort（不能 silent corruption）；
 *              mk_task_create 返回时 tid<0 为 mk_err_t 错误码，由调用者处理。

 * ============================================================================
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <signal.h>

#include "kernel.h"
#include "task.h"
#include "ipc.h"

/* ---- TCB 数组：静态分配，最多 32 个 ---- */
static mk_tcb_t   g_tasks[MK_MAX_TASKS];

/* ================================================================
 *  TCB 硬化基础设施
 *  canary + 状态机白名单 + ctx/stack 完整性
 * ================================================================ */

static void tcb_init_canary(mk_tcb_t *tcb)
{
    tcb->canary_head = MK_TCB_CANARY_HEAD;
    tcb->canary_tail = MK_TCB_CANARY_TAIL;
}

static void tcb_check_canary(const mk_tcb_t *tcb)
{
    if (tcb->canary_head != MK_TCB_CANARY_HEAD
            || tcb->canary_tail != MK_TCB_CANARY_TAIL) {
        fprintf(stderr,
            "FATAL: TCB canary corrupted tid=%u head=0x%llX tail=0x%llX\n",
            tcb->tid,
            (unsigned long long)tcb->canary_head,
            (unsigned long long)tcb->canary_tail);
        abort();
    }
}

/* ctx/stack 完整性 —— 切换前调
 *
 * L2：轻量 ctx 只存 rsp。我们验证 rsp 落在 [stack, stack + stack_size) 范围内。
 * 这样能抓到：栈指针野飞、栈被 free 后 ctx 没清等情况。 */
static void tcb_check_ctx_integrity(const mk_tcb_t *tcb)
{
    if (!tcb->stack || tcb->stack_size == 0) {
        fprintf(stderr,
            "FATAL: TCB ctx integrity fail tid=%u stack=NULL or size=0\n",
            tcb->tid);
        abort();
    }

    uintptr_t rsp   = (uintptr_t)tcb->ctx.rsp;
    uintptr_t sbase = (uintptr_t)tcb->stack;
    uintptr_t stop  = sbase + tcb->stack_size;

    if (rsp < sbase || rsp >= stop) {
        fprintf(stderr,
            "FATAL: TCB ctx rsp out of stack tid=%u "
            "ctx.rsp=0x%lx stack=[0x%lx, 0x%lx)\n",
            tcb->tid, (unsigned long)rsp,
            (unsigned long)sbase, (unsigned long)stop);
        abort();
    }
}

/* 状态机 helper —— 所有 state 写点必须走这里
 *
 * 白名单转换（2D 数组查表代替 switch，O(1)）：
 *        DEAD  READY  RUNNING  BLOCKED  SLEEPING
 *  DEAD    1     1      0        0        0
 *  READY   0     1      1        0        0
 *  RUNNING 1     1      0        1        1
 *  BLOCKED 1     1      0        0        0
 *  SLEEPING 1    1      0        0        0
 *  额外 escape hatch：任何状态 → DEAD 都合法 */
static const uint8_t g_valid_trans[5][5] = {
    { 1, 1, 0, 0, 0 },   /* DEAD    → DEAD/READY */
    { 0, 1, 1, 0, 0 },   /* READY   → READY/RUNNING */
    { 1, 1, 0, 1, 1 },   /* RUNNING → READY/BLOCKED/SLEEPING + DEAD escape */
    { 1, 1, 0, 0, 0 },   /* BLOCKED → READY/DEAD */
    { 1, 1, 0, 0, 0 },   /* SLEEPING → READY/DEAD */
};

void mk_tcb_set_state(mk_tcb_t *tcb, mk_task_state_t new_state)
{
    tcb_check_canary(tcb);

    mk_task_state_t cur = tcb->state;

    /* Escape hatch: 任何状态 → DEAD 都合法 */
    bool ok = (new_state == MK_TASK_DEAD)
            || g_valid_trans[cur][new_state];

    if (!ok) {
        fprintf(stderr,
            "FATAL: illegal state transition tid=%u %u→%u\n",
            tcb->tid, (unsigned)cur, (unsigned)new_state);
        abort();
    }
    tcb->state = new_state;
}

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

/* ---- trampoline：mk_ctx_init 把 mk_task_wrapper 作为 entry 压入栈 ---- */
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
        tcb_init_canary(&g_tasks[i]);
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

    /* 分配栈 —— posix_memalign 保证 16 字节对齐，符合 SysV ABI 要求 */
    void *stack = NULL;
    if (posix_memalign(&stack, 16, stack_size) != 0) {
        return MK_ERR_NOMEM;
    }

    memset(tcb, 0, sizeof(*tcb));
    tcb_init_canary(tcb);
    tcb->tid         = (uint8_t)tid;
    tcb->prio        = prio;
    mk_tcb_set_state(tcb, MK_TASK_DEAD);
    tcb->stack       = stack;
    tcb->stack_size  = stack_size;
    tcb->wake_tick   = 0;
    strncpy(tcb->name, name ? name : "anon", sizeof(tcb->name) - 1);

    /* entry/arg 先存起来，wrapper 运行时从 g_entries[tid] 取 */
    g_entries[tid].entry = entry;
    g_entries[tid].arg   = arg;

    /* L2: mk_ctx_init 一次性搞定 —— 清零 callee-saved 寄存器，
     * 栈顶压入 mk_task_wrapper 作为 return address */
    mk_ctx_init(&tcb->ctx, (void (*)(void))mk_task_wrapper,
                stack, stack_size);

    /* 置 READY 位 + 改 state */
    mk_sched_ready((uint8_t)tid);

    return tid;
}

/* ---- 自杀 ---- */
void mk_task_exit(void)
{
    /* 防御：g_current 边界检查 */
    if (g_current >= MK_MAX_TASKS) {
        fprintf(stderr, "mk_task_exit: g_current=%u out of range, abort\n", g_current);
        abort();
    }

    uint8_t tid = g_current;
    mk_tcb_t *tcb = &g_tasks[tid];

    /* P0-1 硬化：IPC cleanup 必须在任何资源释放之前
     * 我的 reply_id_of_slot[] 里可能有还在等 reply 的 client。
     * 如果我先 free stack，那些 client 永久 BLOCKED。 */
    mk_ipc_cleanup_dead_service(tid);

    /* 然后正常收尾 */
    mk_tcb_set_state(tcb, MK_TASK_DEAD);
    if (tcb->stack) {
        free(tcb->stack);
        tcb->stack = NULL;
    }
    g_entries[tid].entry = NULL;
    g_entries[tid].arg   = NULL;

    mk_sched_unready(tid);

    /* 切下一个（必须成功，因为 idler 永在 mask 里） */
    mk_sched_tick();

    /* 不可达：mk_sched_tick 切走后不会 return 到这里
     * 但加个 abort 让编译器满意 noreturn 属性 */
    abort();
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
    mk_tcb_set_state(tcb, MK_TASK_SLEEPING);
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
        mk_tcb_set_state(&g_tasks[tid], MK_TASK_READY);
    }
}

void mk_sched_unready(uint8_t tid)
{
    g_ready_mask &= ~(1u << tid);
}

/* ---- 从 current 之后选下一个 READY 任务 ---- */
static uint8_t pick_next(void)
{
    uint32_t mask = g_ready_mask;
    if (mask == 0) return MK_TID_IDLER;

    uint8_t cur = g_current;

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
 * L1 优化：去掉冗余 canary 检查。所有 state 变更点都走 mk_tcb_set_state，
 * canary 在那里已经检查过。tick 里只留两条兜底：
 *   1. ctx 完整性检查 —— swapcontext 独有的
 *   2. idler canary —— idler 永不退出，内存破坏兜底 */

void mk_sched_tick(void)
{
    tcb_check_canary(&g_tasks[MK_TID_IDLER]);

    uint8_t next = pick_next();

    uint8_t prev = g_current;
    if (prev == next) {
        return;
    }

    /* prev 如果是 RUNNING（正常 yield），状态转 READY；
     * 如果是 SLEEPING/BLOCKED（sleep/IPC 自己设的），不动。 */
    if (prev != MK_MAX_TASKS && g_tasks[prev].state == MK_TASK_RUNNING) {
        mk_tcb_set_state(&g_tasks[prev], MK_TASK_READY);
    }

    mk_tcb_set_state(&g_tasks[next], MK_TASK_RUNNING);

    tcb_check_ctx_integrity(&g_tasks[next]);

    g_current = next;

    mk_ctx_swap(&g_tasks[prev].ctx, &g_tasks[next].ctx);
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

        mk_tcb_set_state(&g_tasks[first], MK_TASK_RUNNING);

        /* 硬化：mk_ctx_load 前过 canary + ctx 完整性 */
        tcb_check_canary(&g_tasks[first]);
        tcb_check_ctx_integrity(&g_tasks[first]);

        g_current = first;

        /* 第一次跑，没有 prev 的 ctx 要 swap，直接 mk_ctx_load 跳进去 */
        mk_ctx_load(&g_tasks[first].ctx);
        abort();    /* 不可达 */
    }
    mk_sched_tick();
    abort();        /* 不可达 */
}
