/*
 * irq.c — 中断上半部（只有几行） + 下半部模拟入口
 *
 * 上半部原则：**关全局 irq → 置位 pending → 开全局 irq → 返回**
 * 不做任何任务切换、不投递 IPC、不唤醒任务。
 * 所有处理逻辑交给" poll 模式"的下半部任务。
 *
 * 真实硬件上：
 *   irq_trap_top 就是汇编 ISR 入口里调的那一个 C 函数。
 *   上半部汇编只做：push 寄存器 → 调 irq_trap_top → pop 寄存器 → iret。
 *   这里为了宿主机模拟，提供 mk_irq_simulate 伪造一次时钟 tick。
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
