/*
 * task.c — TCB 数组、上下文切换、bitmask 调度器
 *
 * L2 轻量上下文：mk_ctx_swap/mk_ctx_load/mk_ctx_init（汇编原语）
 *   - 只保存 callee-saved 6 个寄存器 + rsp = 56B
 *   - 对比 glibc swapcontext：省掉 FPU/SSE 状态、signal mask 等
 * 调度开销 = 入队/出队 一条位指令；选下一个 = tzcnt 硬件指令（x86-64）
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "kernel.h"
#include "task.h"
#include "ipc.h"

/* ---- TCB 数组：静态分配，最多 32 个 ---- */
static mk_tcb_t   g_tasks[MK_MAX_TASKS];

/* ================================================================
 *
 *  一次 mk_sched_tick（= 一次完整的 ctx_swap 调用）拆成 7 步:
 *
 *   T0  tcb_check_canary(idler)       兜底：idler canary 每 tick 必查
 *   T1  pick_next()                    __builtin_ctz 选下一个 READY
 *   T2  prev state 检查 + 可选 set_state(prev, READY)
 *   T3  set_state(next, RUNNING)       含 canary + 状态机查表 + state 写
 *   T4  ctx_integrity(next)            rsp 栈范围验证
 *   T5  g_current 赋值 + 无关紧要
 *   T6  ★ mk_ctx_swap 汇编本体         6 callee-saved 寄存器保存/恢复 + ret
 *
 * ================================================================ */
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
#include <x86intrin.h>
#endif

enum tick_prof_step {
};

static int      g_tick_call_count;   /* tick 被调用次数 */

    g_tick_call_count = 0;
}

{
    int tick_calls = g_tick_call_count;
    if (tick_calls == 0) tick_calls = 1;   /* 防除零 */
    printf("│ %-30s %10s %10s %8s\n", "Step", "total", "avg", "pct");
    printf("├────────────────────────────┬────────────┬────────────┬────────┤\n");

        "T0 idler canary check",
        "T1 pick_next (ctz)",
        "T2 prev state + set_state",
        "T3 next set_state(RUNNING)",
        "T4 ctx_integrity check",
        "T5 g_current = next",
        "T6 ★ mk_ctx_swap asm",
    };

    uint64_t total = 0;

        double avg = (double)t / tick_calls;
        double pct = total ? 100.0 * t / total : 0;
        printf("│ %-30s %10lu %10.1f %7.1f%% │\n",
               names[s], t, avg, pct);
    }

    printf("├────────────────────────────┼────────────┼────────────┼────────┤\n");
    printf("│ %-30s %10lu %10.1f %7.1f%% │\n",
    printf("│ %-30s %10lu %10.1f %7.1f%% │\n",
           "调度器开销 (T0-T5)", sched_ovhd, (double)sched_ovhd/tick_calls,
           total ? 100.0*sched_ovhd/total : 0);
    printf("├────────────────────────────┼────────────┼────────────┼────────┤\n");
    printf("│ %-30s %10lu %10.1f %7.1f%% │\n",
           "TOTAL per tick", total, (double)total/tick_calls, 100.0);
    printf("└────────────────────────────────────────────────────────────────┘\n");
}

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
 * 这样能抓到：栈指针野飞、栈被 free 后 ctx 没清等情况。
 *
 * 注意：新创建的任务 ctx 也会过这里 —— mk_ctx_init 把 rsp 设到栈顶附近，
 * 一定在范围内。已运行过的任务 save 下来的 rsp 也指向自己栈上某处，
 * 同样在范围内。只有内存破坏或 use-after-free 才会让它越界。 */
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
 *  白名单转换（L1 优化：2D 数组查表代替 switch，O(1)）：
 *        DEAD  READY  RUNNING  BLOCKED  SLEEPING
 *  DEAD    1     1      0        0        0
 *  READY   0     1      1        0        0
 *  RUNNING 1     1      0        1        1     ← 还可到 BLOCKED/SLEEPING
 *  BLOCKED 1     1      0        0        1     ← 还可到 SLEEPING（被 sleep 覆盖的罕见情况）
 *  SLEEPING 1    1      0        0        0
 *  额外 escape hatch：任何状态 → DEAD 都合法 */
static const uint8_t g_valid_trans[5][5] = {
    { 1, 1, 0, 0, 0 },   /* DEAD    → DEAD/READY */
    { 0, 1, 1, 0, 0 },   /* READY   → READY/RUNNING */
    { 1, 1, 0, 1, 1 },   /* RUNNING → 允许 READY/BLOCKED/SLEEPING + DEAD escape */
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

/* ---- trampoline：mk_ctx_init 把 mk_task_wrapper 作为 entry 压入栈 ----
 * 调度器在 mk_ctx_swap 前已经把 g_current 设成 next 的 tid，
 * 所以 wrapper 里直接 mk_current_tid() 就能拿到自己的 tid。 */
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

    /* 分配栈 —— posix_memalign 保证 16 字节对齐，符合 makecontext 要求 */
    void *stack = NULL;
    if (posix_memalign(&stack, 16, stack_size) != 0) {
        return MK_ERR_NOMEM;
    }

    memset(tcb, 0, sizeof(*tcb));
    tcb_init_canary(tcb);
    tcb->tid         = (uint8_t)tid;
    tcb->prio        = prio;
        mk_tcb_set_state(tcb, MK_TASK_DEAD);   /* mk_sched_ready 里改成 READY */
    tcb->stack       = stack;
    tcb->stack_size  = stack_size;
    tcb->wake_tick   = 0;
    strncpy(tcb->name, name ? name : "anon", sizeof(tcb->name) - 1);

    /* entry/arg 先存起来，wrapper 运行时从 g_entries[tid] 取 */
    g_entries[tid].entry = entry;
    g_entries[tid].arg   = arg;

    /* L2: mk_ctx_init 一次性搞定 —— 清零 callee-saved 寄存器，
     * 栈顶压入 mk_task_wrapper 作为 return address，
     * rsp 指向栈顶。比 getcontext+makecontext 省几百字节。 */
    mk_ctx_init(&tcb->ctx, (void (*)(void))mk_task_wrapper,
                stack, stack_size);

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
        /* L1 优化 + 硬化修复：之前直接写 state 绕过 mk_tcb_set_state → canary 漏检 */
        mk_tcb_set_state(&g_tasks[tid], MK_TASK_READY);
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
 *  只负责挑下一个 + swapcontext。热路径。
 *
 *  L1 优化：去掉 tick 里的 canary 全量/按需检查。
 *  所有 state 变更点（send/exit/sleep/mk_sched_ready）都走 mk_tcb_set_state，
 *  canary 在那里已经检查过。tick 里再查 prev+next 是重复的。
 *  但保留两条兜底：
 *    1. ctx 完整性检查 —— swapcontext 独有的，之前 state 变更点没覆盖
 *    2. idler canary —— idler 永不退出，如果有人能覆盖它的 canary，
 *       说明内存破坏已经严重到整个内核随时会崩，不能等下次 state 变更
 *       才被发现。 */

void mk_sched_tick(void)
{
    g_tick_call_count++;

    tcb_check_canary(&g_tasks[MK_TID_IDLER]);

    uint8_t next = pick_next();

    uint8_t prev = g_current;
    if (prev == next) {
        return;
    }

    /* prev 如果是 RUNNING（正常 yield），状态转 READY；
     * 如果是 SLEEPING/BLOCKED（sleep/IPC 自己设的），不动。
     * mk_tcb_set_state 内部会查 canary */
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
        abort();
    }
    mk_sched_tick();
}
