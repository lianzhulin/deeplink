/*
 * task.c — TCB 数组、上下文切换、环形调度器
 *
 * 切换开销 = swapcontext() 一次（寄存器保存 + 恢复）
 * 调度开销 = O(1) 环形链表前进 + 找第一个 READY
 */
#define _GNU_SOURCE
#include <ucontext.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "kernel.h"
#include "task.h"

/* ---- TCB 数组：静态分配，最多 32 个 ---- */
static mk_tcb_t   g_tasks[MK_MAX_TASKS];

/* 每个任务的 entry/arg，由 mk_task_create 填，mk_task_wrapper 取 */
typedef struct {
    mk_task_entry_t entry;
    void           *arg;
} mk_entry_info_t;
static mk_entry_info_t g_entries[MK_MAX_TASKS];

/* 调度器状态 */
static uint8_t     g_current   = MK_MAX_TASKS;    /* 未启动 */
static uint8_t     g_chain_head = MK_MAX_TASKS;   /* 环形链表头 */

volatile uint64_t mk_ticks = 0;

/* ---- trampoline：被 makecontext 调起 ----
 * 设计：调度器在 swapcontext 之前已经把 g_current 设成了 next 的 tid，
 * 所以 wrapper 里直接 mk_current_tid() 就能拿到自己的 tid。
 * 完全不依赖 makecontext 的参数传递。
 */
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
        g_tasks[i].next  = (uint8_t)i;   /* 先自指 */
        g_tasks[i].irq_pending = 0;
    }
    g_current = MK_MAX_TASKS;
    g_chain_head = MK_MAX_TASKS;
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
    tcb->state       = MK_TASK_DEAD;   /* mk_sched_ready 里会改成 READY */
    tcb->stack       = stack;
    tcb->stack_size  = stack_size;
    tcb->irq_pending = 0;
    tcb->wake_tick   = 0;
    tcb->ipc_waiter  = (uint8_t)-1;
    tcb->ipc_blocked  = false;
    tcb->ipc_has_msg  = false;
    strncpy(tcb->name, name ? name : "anon", sizeof(tcb->name) - 1);

    /* makecontext：entry 先存起来，wrapper 里调 */
    g_entries[tid].entry = entry;
    g_entries[tid].arg   = arg;

    getcontext(&tcb->ctx);
    tcb->ctx.uc_stack.ss_sp   = stack;
    tcb->ctx.uc_stack.ss_size = stack_size;
    tcb->ctx.uc_link          = NULL;   /* 我们在 mk_task_exit 处理返回 */

    /* makecontext：无参数，PC = wrapper，栈 = 分配好的栈。
     * wrapper 里直接用 mk_current_tid() 拿 tid（mk_sched_tick 在 swapcontext 前已设好 g_current）。 */
    makecontext(&tcb->ctx, (void (*)(void))mk_task_wrapper, 0);

    /* 挂入调度链 */
    mk_sched_ready((uint8_t)tid);

    return tid;
}

/* ---- 自杀 ---- */
void mk_task_exit(void)
{
    uint8_t tid = g_current;
    mk_tcb_t *tcb = &g_tasks[tid];

    tcb->state = MK_TASK_DEAD;
    if (tcb->stack) {
        free(tcb->stack);
        tcb->stack = NULL;
    }
    g_entries[tid].entry = NULL;
    g_entries[tid].arg   = NULL;

    mk_sched_unready(tid);

    /* 切换到下一个 */
    mk_sched_tick();
    /* swapcontext 不返回 */
    mk_sched_run();  /* 兜底，理论上不会走到 */
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

/* ---- 调度链维护 ----
 * 这是单向环形链表：每个节点的 next 指向下一个 READY 节点。 */

static bool is_ready_state(mk_task_state_t s) {
    return s == MK_TASK_READY || s == MK_TASK_RUNNING;
}

void mk_sched_ready(uint8_t tid)
{
    mk_tcb_t *tcb = &g_tasks[tid];
    if (is_ready_state(tcb->state)) return;   /* 已在链里 */

    tcb->state = MK_TASK_READY;

    if (g_chain_head == MK_MAX_TASKS) {
        /* 链表空 —— 自己指自己 */
        tcb->next = tid;
        g_chain_head = tid;
        return;
    }

    /* 插入到 head 之前，也就是 head 变成新节点 */
    /* 找到 head 前一个，把它的 next 指向 tid；tid->next = head；更新 head */
    uint8_t prev = g_chain_head;
    while (g_tasks[prev].next != g_chain_head) {
        prev = g_tasks[prev].next;
    }
    tcb->next = g_chain_head;
    g_tasks[prev].next = tid;
    g_chain_head = tid;
}

void mk_sched_unready(uint8_t tid)
{
    /* 从链里摘出 */
    if (g_chain_head == MK_MAX_TASKS) return;

    uint8_t prev = g_chain_head;
    bool found = false;
    while (g_tasks[prev].next != g_chain_head) {
        if (g_tasks[prev].next == tid) {
            found = true;
            break;
        }
        prev = g_tasks[prev].next;
    }
    if (g_tasks[prev].next == tid) found = true;   /* 也处理 head 是 tid 的情况 */

    if (!found && g_chain_head != tid) return;      /* 没在链里 */

    if (g_tasks[prev].next == tid) {
        g_tasks[prev].next = g_tasks[tid].next;
    }
    if (g_chain_head == tid) {
        g_chain_head = (g_tasks[tid].next == tid) ? MK_MAX_TASKS : g_tasks[tid].next;
    }
    g_tasks[tid].next = tid;    /* 自指，清干净 */
}

/* ---- 决定下一个要跑的 tid ---- */
static uint8_t pick_next(void)
{
    if (g_chain_head == MK_MAX_TASKS) return MK_TID_IDLER;   /* 空链，兜底 idler */

    uint8_t start = (g_current == MK_MAX_TASKS) ? g_chain_head : g_tasks[g_current].next;
    uint8_t next = start;
    int guard = MK_MAX_TASKS + 1;
    while (!is_ready_state(g_tasks[next].state) && guard-- > 0) {
        next = g_tasks[next].next;
        if (next == start) break;
    }

    /* 最终兜底：idler 永在 READY */
    if (!is_ready_state(g_tasks[next].state)) {
        if (!is_ready_state(g_tasks[MK_TID_IDLER].state)) {
            g_tasks[MK_TID_IDLER].state = MK_TASK_READY;
            mk_sched_ready(MK_TID_IDLER);
        }
        return MK_TID_IDLER;
    }
    return next;
}

/* ---- tick：yield / sleep / IPC block 等调度点调用 ----
 *
 * 调度器只负责挑下一个 + swapcontext。
 * **不处理 irq**：上半部只置位 pending，由 clock 任务 poll 后做真正的处理。
 * 这样职责分离，且调度器热路径保持极简。 */
void mk_sched_tick(void)
{
    uint8_t next = pick_next();
    uint8_t prev = g_current;
    if (prev == next) return;    /* 没人可切 */

    if (prev != MK_MAX_TASKS) {
        /* 只有正常 yield（RUNNING→READY）才由调度器改状态。
         * sleep / IPC block 等路径自己会设好 state，这里不动。 */
        if (g_tasks[prev].state == MK_TASK_RUNNING) {
            g_tasks[prev].state = MK_TASK_READY;
        }
    }
    g_tasks[next].state = MK_TASK_RUNNING;

    g_current = next;
    swapcontext(&g_tasks[prev].ctx, &g_tasks[next].ctx);
    /* 从 swapcontext 返回时，我们已经在 prev 的下一次调度里被换回 */
}

/* ---- 启动调度器 ---- */
void mk_sched_run(void)
{
    /* 先确保 idler 已创建并且在链里 */
    if (g_current == MK_MAX_TASKS) {
        /* 还没跑过任何任务。找链里第一个 ready 的。 */
        uint8_t first = (g_chain_head == MK_MAX_TASKS) ? MK_TID_IDLER : g_chain_head;
        if (!is_ready_state(g_tasks[first].state)) {
            first = MK_TID_IDLER;
        }
        g_tasks[first].state = MK_TASK_RUNNING;
        g_current = first;

        /* 第一次跑，没有 prev 的 ctx 要 swap，
         * 直接 setcontext 进目标任务（之后都用 swapcontext）。 */
        setcontext(&g_tasks[first].ctx);
        /* 理论上永不返回 */
        abort();
    }
    /* 兜底：如果 g_current 已经有值，就 tick 一次切到它 */
    mk_sched_tick();
}
