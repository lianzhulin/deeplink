
#include <signal.h>
#include <stdio.h>
#include "kernel.h"
#include "irq.h"
#include "task.h"

volatile uint32_t mk_global_irq_pending = 0;
static volatile int g_irq_depth = 0;    

void mk_clock_bottom_half(void *arg)
{
    (void)arg;
    
    
}

void mk_clock_tick_process(void)
{
    mk_global_irq_pending &= ~MK_IRQ_CLOCK;
    for (int i = 0; i < MK_MAX_TASKS; ++i) {
        mk_tcb_t *tcb = mk_tcb_get((uint8_t)i);
        
        if (tcb && tcb->state == MK_TASK_SLEEPING && tcb->wake_tick <= mk_ticks) {
            mk_sched_ready((uint8_t)i);   
        }
    }
}

void mk_irq_simulate(uint32_t irq_bit)
{
    
    g_irq_depth++;
    mk_irq_trap_top(irq_bit);
    g_irq_depth--;
}

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

bool mk_in_irq(void)
{
    return g_irq_depth > 0;
}
