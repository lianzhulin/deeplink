
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "kernel.h"
#include "task.h"
#include "ipc.h"

static mk_mailbox_t g_mboxes[MK_MAX_TASKS];
static uint16_t     g_reply_counter;   

void mk_ipc_init(void)
{
    memset(g_mboxes, 0, sizeof(g_mboxes));
    g_reply_counter = 0;
}

static inline bool q_empty(const mk_mailbox_t *m) { return m->count == 0; }

static inline uint8_t q_push(mk_mailbox_t *m, const mk_msg_t *msg)
{
    uint8_t slot = m->tail;
    m->slots[slot] = *msg;
    m->tail = (uint8_t)((slot + 1) & (MK_IPC_QUEUE_SIZE - 1));
    m->count++;
    return slot;
}

static inline uint8_t q_pop(mk_mailbox_t *m, mk_msg_t *out)
{
    uint8_t slot = m->head;
    if (out) *out = m->slots[slot];
    m->head = (uint8_t)((slot + 1) & (MK_IPC_QUEUE_SIZE - 1));
    m->count--;
    return slot;
}

static inline void wake_send_waiter(uint8_t tid)
{
    mk_tcb_t *tcb = mk_tcb_get(tid);
    if (tcb->ipc_send_wait && tcb->state == MK_TASK_BLOCKED) {
        tcb->ipc_send_wait = false;
        mk_sched_ready(tid);
    }
}

static inline void wake_recv_waiter(uint8_t tid)
{
    mk_tcb_t *tcb = mk_tcb_get(tid);
    if (tcb->ipc_recv_wait && tcb->state == MK_TASK_BLOCKED) {
        tcb->ipc_recv_wait = false;
        mk_sched_ready(tid);
    }
}

mk_err_t mk_ipc_send(uint8_t to, const mk_msg_t *msg)
{
    uint8_t self = mk_current_tid();
    if (to >= MK_MAX_TASKS || to == self) return MK_ERR_INVALID;
    if (!msg) return MK_ERR_INVALID;

    mk_tcb_t     *dst_tcb  = mk_tcb_get(to);
    mk_mailbox_t *dst_mbox = &g_mboxes[to];
    mk_tcb_t     *me       = mk_tcb_get(self);

    if (!dst_tcb || dst_tcb->state == MK_TASK_DEAD) return MK_ERR_NOIPC;

    
    uint16_t reply_id = (uint16_t)(++g_reply_counter);
    if (reply_id == 0) reply_id = 1;   

    mk_msg_t full_msg = *msg;
    full_msg.from     = self;
    full_msg.reply_id = reply_id;

    uint8_t slot = q_push(dst_mbox, &full_msg);
    dst_mbox->reply_id_of_slot[slot] = reply_id;   

    wake_recv_waiter(to);

    me->ipc_send_wait = true;
    mk_tcb_set_state(me, MK_TASK_BLOCKED);
    mk_sched_unready(self);

    mk_sched_tick();

    me->ipc_send_wait = false;
    return MK_OK;
}

mk_err_t mk_ipc_receive(mk_msg_t *msg)
{
    if (!msg) return MK_ERR_INVALID;

    uint8_t tid = mk_current_tid();
    mk_tcb_t     *tcb  = mk_tcb_get(tid);
    mk_mailbox_t *mbox = &g_mboxes[tid];

    while (q_empty(mbox)) {
        tcb->ipc_recv_wait = true;
        mk_tcb_set_state(tcb, MK_TASK_BLOCKED);
        mk_sched_unready(tid);

        mk_sched_tick();
    }

    q_pop(mbox, msg);
    return MK_OK;
}

mk_err_t mk_ipc_reply(uint8_t to, const mk_msg_t *msg)
{
    uint8_t self = mk_current_tid();
    if (to >= MK_MAX_TASKS || to == self) return MK_ERR_INVALID;
    if (!msg) return MK_ERR_INVALID;

    
    if (msg->reply_id == 0) return MK_ERR_INVALID;

    mk_tcb_t     *to_tcb   = mk_tcb_get(to);
    mk_mailbox_t *dst_mbox = &g_mboxes[to];      
    mk_mailbox_t *my_mbox  = &g_mboxes[self];     

    if (!to_tcb || to_tcb->state == MK_TASK_DEAD) return MK_ERR_NOIPC;

    
    bool inflight_found = false;

    for (uint8_t i = 0; i < MK_IPC_QUEUE_SIZE; ++i) {
        
        if (my_mbox->reply_id_of_slot[i] == msg->reply_id) {
            my_mbox->reply_id_of_slot[i] = 0;   
            inflight_found = true;
            break;
        }
    }

    if (!inflight_found) {
        return MK_ERR_INVALID;
    }

    
    mk_msg_t full_msg = *msg;
    full_msg.from = self;

    q_push(dst_mbox, &full_msg);

    wake_send_waiter(to);

    return MK_OK;
}

bool mk_ipc_poll(void)
{
    uint8_t tid = mk_current_tid();
    return !q_empty(&g_mboxes[tid]);
}

void mk_ipc_cleanup_dead_service(uint8_t dead_tid)
{
    if (dead_tid >= MK_MAX_TASKS) return;

    mk_mailbox_t *my = &g_mboxes[dead_tid];

    for (uint8_t i = 0; i < MK_IPC_QUEUE_SIZE; ++i) {
        if (my->reply_id_of_slot[i] == 0) continue;   

        uint8_t  client   = my->slots[i].from;
        uint16_t reply_id = my->reply_id_of_slot[i];

        
        my->reply_id_of_slot[i] = 0;

        
        if (client >= MK_MAX_TASKS) continue;

        
        mk_msg_t err;
        memset(&err, 0, sizeof(err));
        err.reply_id = reply_id;
        err.from     = dead_tid;
        err.data[0]  = MK_ERR_NOIPC;

        mk_mailbox_t *dst = &g_mboxes[client];
        q_push(dst, &err);

        
        wake_send_waiter(client);
    }

    
    while (!q_empty(my)) {
        q_pop(my, NULL);
    }
}
