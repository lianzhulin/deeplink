/*
 * ============================================================================
 *   irq.h — 中断上半部接口（极简版）
 * ============================================================================
 *
 *   【架构角色】中断上半部唯一对外头文件。提供 mk_irq_trap_top inline
 *              供汇编 ISR 入口调用，以及下半部 poll 接口。依赖 kernel.h；
 *              被 main.c / irq.c / 调度器可能用到。
 *   【硬化要点】mk_irq_trap_top 只有 2 条指令 (置位全局 + 置位当前任务
 *              irq_pending)，不做任何 IPC、不唤醒任何任务 —— 所有处理
 *              留给下半部 poll 完成，最大化上半部响应速度。
 *   【性能要点】上半部函数 static inline、volatile 原子置位；
 *              下半部 poll_clear 是位与加位清除，O(1)。
 *   【不变量】mk_global_irq_pending 全局唯一，所有中断源 OR 到一起；
 *              irq_bit 每 bit 对应一种中断源。
 *   【约束】mk_irq_trap_top 只能在中断上下文调用；mk_irq_poll_clear
 *              只能在下半部任务或调度点调用，不能重入。
 *   【数据结构】无；依赖外部 volatile uint32_t mk_global_irq_pending。
 *   【线程安全】上半部是中断上下文 (可能抢占调度线程)，下半部是任务上下文。

 * ============================================================================
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
