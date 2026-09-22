
#ifndef MK_KERNEL_H
#define MK_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MK_MAX_TASKS   32
#define MK_TID_BITS    5               
#define MK_TID_MASK    (MK_MAX_TASKS - 1)

#define MK_TID_IDLER   0               
#define MK_TID_CLOCK   1               
#define MK_TID_MM      2               
#define MK_TID_FIRST_USER 3            

#define MK_DEFAULT_STACK_SIZE  (64 * 1024)

typedef enum {
    MK_TASK_DEAD     = 0,
    MK_TASK_READY    = 1,
    MK_TASK_RUNNING  = 2,
    MK_TASK_BLOCKED  = 3,             
    MK_TASK_SLEEPING = 4              
} mk_task_state_t;

typedef enum {
    MK_OK           =  0,
    MK_ERR_INVALID  = -1,
    MK_ERR_NOMEM    = -2,
    MK_ERR_NOIPC    = -3,
    MK_ERR_TIMEOUT  = -4,
    MK_ERR_BUSY     = -5,
    MK_ERR_EXIST    = -6
} mk_err_t;

#define MK_IRQ_CLOCK   (1u << 0)
#define MK_IRQ_IOBASE  (1u << 1)     

extern volatile uint64_t mk_ticks;

void mk_kernel_start(void);

#endif 
