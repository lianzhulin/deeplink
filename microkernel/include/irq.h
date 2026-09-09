/*
 * irq.h — 中断上半部（极简版）
 *
 * 设计目标：**上半部只有几行 C**，不做任何处理逻辑。
 *
 *   硬件 → irq_trap() → 置 irq_pending 位 → 返回 →
 *   （当前任务继续跑，直到下一次调度点）
 *   → 时钟 tick 或下次调度时 → 时钟任务/其他任务 poll irq_pending → 下半部处理
 *
 * 这样"中断上半部处理时间"就只有 2~3 条指令。
 */
#ifndef MK_IRQ_H
#define MK_IRQ_H

#include "kernel.h"

/* 模拟：宿主机上没有真正的硬件中断，我们用 mk_irq_simulate() 来"伪造"一次中断。
 * 生产环境下这里替换成真正的汇编 ISR 入口。 */
void mk_irq_simulate(uint32_t irq_bit);

/* 上半部本体：被硬件 ISR 或 mk_irq_simulate 调到。 */
static inline void mk_irq_trap_top(uint32_t irq_bit)
{
    /* 核心：置位全局 pending + 当前运行任务的 irq_pending
     * 然后返回。仅此而已。 */
    extern volatile uint32_t mk_global_irq_pending;
    mk_global_irq_pending |= irq_bit;

    /* 关键优化：我们不在这里做任何上下文切换、不投递任何 IPC、
     * 不唤醒任何任务——这些统统留给下半部任务 poll 完成。 */
}

/* 查询/清 pending（下半部任务用） */
uint32_t mk_irq_poll_clear(uint32_t mask);

/* 初始化：注册"中断下半部任务"。
 * 当前原型把时钟的下半部逻辑就塞在 clock task 里，不用单独注册。 */
void mk_irq_init(void);

#endif /* MK_IRQ_H */
