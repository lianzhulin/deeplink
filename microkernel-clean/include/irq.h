
#ifndef MK_IRQ_H
#define MK_IRQ_H

#include "kernel.h"

void mk_irq_simulate(uint32_t irq_bit);

static inline void mk_irq_trap_top(uint32_t irq_bit)
{
    
    extern volatile uint32_t mk_global_irq_pending;
    mk_global_irq_pending |= irq_bit;

    
}

uint32_t mk_irq_poll_clear(uint32_t mask);

void mk_irq_init(void);

#endif 
