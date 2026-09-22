
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <signal.h>

#include "kernel.h"
#include "task.h"
#include "ipc.h"

static mk_tcb_t   g_tasks[MK_MAX_TASKS];

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

static const uint8_t g_valid_trans[5][5] = {
    { 1, 1, 0, 0, 0 },   
    { 0, 1, 1, 0, 0 },   
    { 1, 1, 0, 1, 1 },   
    { 1, 1, 0, 0, 0 },   
    { 1, 1, 0, 0, 0 },   
};

void mk_tcb_set_state(mk_tcb_t *tcb, mk_task_state_t new_state)
{
    tcb_check_canary(tcb);

    mk_task_state_t cur = tcb->state;

    
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

typedef struct {
    mk_task_entry_t entry;
    void           *arg;
} mk_entry_info_t;
static mk_entry_info_t g_entries[MK_MAX_TASKS];

static uint8_t     g_current   = MK_MAX_TASKS;   
static uint32_t    g_ready_mask = 0;              

volatile uint64_t mk_ticks = 0;

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
    mk_task_exit();     
}

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

int mk_task_create(const char *name, mk_task_entry_t entry,
                   void *arg, size_t stack_size, uint8_t prio)
{
    if (!entry) return MK_ERR_INVALID;
    if (stack_size == 0) stack_size = MK_DEFAULT_STACK_SIZE;
    if (stack_size < 8192) stack_size = 8192;

    
    int tid = -1;
    for (int i = 0; i < MK_MAX_TASKS; ++i) {
        if (g_tasks[i].state == MK_TASK_DEAD) {
            tid = i;
            break;
        }
    }
    if (tid < 0) return MK_ERR_NOMEM;

    mk_tcb_t *tcb = &g_tasks[tid];

    
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

    
    g_entries[tid].entry = entry;
    g_entries[tid].arg   = arg;

    
    mk_ctx_init(&tcb->ctx, (void (*)(void))mk_task_wrapper,
                stack, stack_size);

    
    mk_sched_ready((uint8_t)tid);

    return tid;
}

void mk_task_exit(void)
{
    
    if (g_current >= MK_MAX_TASKS) {
        fprintf(stderr, "mk_task_exit: g_current=%u out of range, abort\n", g_current);
        abort();
    }

    uint8_t tid = g_current;
    mk_tcb_t *tcb = &g_tasks[tid];

    
    mk_ipc_cleanup_dead_service(tid);

    
    mk_tcb_set_state(tcb, MK_TASK_DEAD);
    if (tcb->stack) {
        free(tcb->stack);
        tcb->stack = NULL;
    }
    g_entries[tid].entry = NULL;
    g_entries[tid].arg   = NULL;

    mk_sched_unready(tid);

    
    mk_sched_tick();

    
    abort();
}

void mk_task_yield(void)
{
    mk_sched_tick();
}

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

uint8_t  mk_current_tid(void) { return g_current; }
mk_tcb_t *mk_current(void)    { return &g_tasks[g_current]; }
mk_tcb_t *mk_tcb_get(uint8_t tid) {
    if (tid >= MK_MAX_TASKS) return NULL;
    return &g_tasks[tid];
}

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

static uint8_t pick_next(void)
{
    uint32_t mask = g_ready_mask;
    if (mask == 0) return MK_TID_IDLER;

    uint8_t cur = g_current;

    if (cur + 1 >= 32) {
        
        return (uint8_t)__builtin_ctz(mask);
    }

    uint32_t after = mask & ~((1u << (cur + 1)) - 1u);
    if (after) {
        return (uint8_t)__builtin_ctz(after);
    }
    
    return (uint8_t)__builtin_ctz(mask);
}

void mk_sched_tick(void)
{
    tcb_check_canary(&g_tasks[MK_TID_IDLER]);

    uint8_t next = pick_next();

    uint8_t prev = g_current;
    if (prev == next) {
        return;
    }

    
    if (prev != MK_MAX_TASKS && g_tasks[prev].state == MK_TASK_RUNNING) {
        mk_tcb_set_state(&g_tasks[prev], MK_TASK_READY);
    }

    mk_tcb_set_state(&g_tasks[next], MK_TASK_RUNNING);

    tcb_check_ctx_integrity(&g_tasks[next]);

    g_current = next;

    mk_ctx_swap(&g_tasks[prev].ctx, &g_tasks[next].ctx);
}

void mk_sched_run(void)
{
    if (g_current == MK_MAX_TASKS) {
        
        uint8_t first;
        if (g_ready_mask) {
            first = (uint8_t)__builtin_ctz(g_ready_mask);
        } else {
            
            first = MK_TID_IDLER;
            mk_sched_ready(first);
        }

        mk_tcb_set_state(&g_tasks[first], MK_TASK_RUNNING);

        
        tcb_check_canary(&g_tasks[first]);
        tcb_check_ctx_integrity(&g_tasks[first]);

        g_current = first;

        
        mk_ctx_load(&g_tasks[first].ctx);
        abort();    
    }
    mk_sched_tick();
    abort();        
}
