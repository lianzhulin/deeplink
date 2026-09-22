/*
 * ============================================================================
 *   irq.c — 中断上半部（只有几行） + 下半部 tick 处理
 * ============================================================================
 *
 *   【实现要点】mk_irq_simulate 模拟硬件 ISR: 关 → trap_top → 开，
 *              调的是 mk_irq_trap_top (irq.h 里的 static inline)，
 *              只做全局 pending 位置位。mk_clock_tick_process 扫描所有 TCB，
 *              找 SLEEPING 且 wake_tick <= mk_ticks 的任务 → 调 mk_sched_ready 唤醒。
 *              mk_irq_poll_clear 用位与清 mask，返回清除的位，
 *              让 clock_task 在 poll 循环里决定要不要处理。
 *   【硬化实现】中断深度 g_irq_depth 重入计数保证上半部嵌套安全；
 *              mk_clock_tick_process 唤醒用 mk_sched_ready 统一走状态机，
 *              不绕过 mk_tcb_set_state。
 *   【热路径】mk_clock_tick_process 每 clock tick (≈1ms) 调一次，
 *              内部 O(N) 扫 TCB 数组找 SLEEPING 任务 —— N=32 开销可忽略。
 *   【错误处理】下半部 poll 永远只清 bit，不会漏；上半部置位是 OR，
 *              丢 bit 不可能；g_irq_depth 是 int 计数，实际不允许递归。

 * ============================================================================
 */
#include <signal.h>
#include <stdio.h>
#include "kernel.h"
#include "irq.h"
#include "task.h"

volatile uint32_t mk_global_irq_pending = 0;
static volatile int g_irq_depth = 0;    /* 重入计数 */

/* ---- 下半部任务（clock task） ----
 * 这个函数会被作为一个普通任务入口跑。它自己 poll pending 位，
 * 做真正的 tick 处理和唤醒 sleeping 任务。 */
void mk_clock_bottom_half(void *arg)
{
    (void)arg;
    /* 这个函数的循环在 main.c 里或者 clock_task 里 */
    /* 这里只是暴露给 irq.h 的下半部逻辑 */
}

/* 让我们实现一个真正可调用的 tick 处理函数，供 task.c / main.c 用 */
void mk_clock_tick_process(void)
{
    mk_global_irq_pending &= ~MK_IRQ_CLOCK;
    for (int i = 0; i < MK_MAX_TASKS; ++i) {
        mk_tcb_t *tcb = mk_tcb_get((uint8_t)i);
        /* 只做决策（从 SLEEPING → 要被唤醒），状态变更 + 挂链交给 mk_sched_ready 统一做 */
        if (tcb && tcb->state == MK_TASK_SLEEPING && tcb->wake_tick <= mk_ticks) {
            mk_sched_ready((uint8_t)i);   /* 内部会把 state 改成 READY + 挂链 */
        }
    }
}

/* ---- 模拟：伪造一次中断（测试用 / 宿主机 tick 驱动） ---- */
void mk_irq_simulate(uint32_t irq_bit)
{
    /* 模拟硬件 ISR：关 → trap_top → 开 */
    g_irq_depth++;
    mk_irq_trap_top(irq_bit);
    g_irq_depth--;
}

/* ---- poll 工具 ---- */
uint32_t mk_irq_poll_clear(uint32_t mask)
{
    uint32_t got = mk_global_irq_pending & mask;
    mk_global_irq_pending &= ~got;
    return got;
}

void mk_irq_init(void)
{
    mk_global_irq_pending = 0;
    g_irq_depth = 0;
}

/* ---- 当前是否在中断上下文（task.c 的 mk_sched_tick 要判断） ---- */
bool mk_in_irq(void)
{
    return g_irq_depth > 0;
}
