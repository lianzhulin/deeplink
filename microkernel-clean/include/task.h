
#ifndef MK_TASK_H
#define MK_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"

typedef struct mk_light_ctx {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
} mk_light_ctx_t;

#define MK_TCB_CANARY_HEAD  0xCAFEBABEDEADBEEFULL
#define MK_TCB_CANARY_TAIL  0x0BADF00DCAFEFACEULL

typedef struct mk_tcb {
    uint64_t           canary_head;    

    uint8_t            tid;
    uint8_t            prio;          
    volatile mk_task_state_t state;
    char               name[16];

    mk_light_ctx_t     ctx;           
    void              *stack;
    size_t             stack_size;

    
    uint64_t           wake_tick;

    
    volatile bool      ipc_send_wait;
    volatile bool      ipc_recv_wait;

    
    volatile uint32_t  irq_pending;

    uint64_t           canary_tail;    
} mk_tcb_t;

typedef void (*mk_task_entry_t)(void *arg);

void mk_task_init(void);

int  mk_task_create(const char *name, mk_task_entry_t entry,
                    void *arg, size_t stack_size, uint8_t prio);

void mk_task_exit(void) __attribute__((noreturn));

void mk_task_yield(void);

void mk_task_sleep(uint64_t ticks);

uint8_t mk_current_tid(void);
mk_tcb_t *mk_current(void);
mk_tcb_t *mk_tcb_get(uint8_t tid);

void    mk_tcb_set_state(mk_tcb_t *tcb, mk_task_state_t new_state);

void mk_ctx_init(mk_light_ctx_t *ctx, void (*entry)(void),
                 void *stack, size_t stack_size);

void mk_ctx_swap(mk_light_ctx_t *old_ctx, mk_light_ctx_t *new_ctx);

void mk_ctx_load(mk_light_ctx_t *ctx);

void mk_sched_ready(uint8_t tid);
void mk_sched_unready(uint8_t tid);

void mk_sched_tick(void);

void mk_sched_run(void) __attribute__((noreturn));

#endif 
