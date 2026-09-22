
#ifndef MK_IPC_H
#define MK_IPC_H

#include "kernel.h"

typedef struct mk_msg {
    uint16_t  tag;
    uint16_t  reply_id;   
    uint8_t   from;       
    uint8_t   flags;
    int32_t   data[4];
} mk_msg_t;

#define MK_IPC_QUEUE_SIZE MK_MAX_TASKS

typedef struct {
    mk_msg_t  slots[MK_IPC_QUEUE_SIZE];
    uint8_t   head;
    uint8_t   tail;
    uint8_t   count;
    uint16_t  reply_id_of_slot[MK_IPC_QUEUE_SIZE]; 
} mk_mailbox_t;

void    mk_ipc_init(void);

mk_err_t mk_ipc_send(uint8_t to, const mk_msg_t *msg);

mk_err_t mk_ipc_receive(mk_msg_t *msg);

mk_err_t mk_ipc_reply(uint8_t to, const mk_msg_t *msg);

bool     mk_ipc_poll(void);

void     mk_ipc_cleanup_dead_service(uint8_t dead_tid);

#endif 
