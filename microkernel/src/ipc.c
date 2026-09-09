/*
 * ipc.c — IPC 三原语 send / receive / reply
 *
 * mailbox 是"每任务一个槽位 + 一个 reply waiter"的极简化设计：
 *   - 每条消息是 mk_msg_t（64 字节），一次一槽
 *   - send：直接写入对方 inbox；若对方正 receive-ing（BLOCKED），唤醒它
 *   - receive：inbox 有就直接读，没有就自己 BLOCKED 等 send/reply
 *   - reply：写入对方 inbox；reply 本身不阻塞
 *
 * 所有对 TCB 的访问走 task.h 暴露的公共 API，不碰 task.c 的 static 变量。
 */
#include <string.h>
#include "kernel.h"
#include "task.h"
#include "ipc.h"

/* mailbox 数组：和 TCB 对齐，每个 tid 一个 */
static mk_mailbox_t g_mboxes[MK_MAX_TASKS];

void mk_ipc_init(void)
{
    memset(g_mboxes, 0, sizeof(g_mboxes));
    for (int i = 0; i < MK_MAX_TASKS; ++i) {
        g_mboxes[i].reply_to = -1;
    }
}

/* ---- 内部：阻塞自己等待 inbox 变为 available ---- */
static void block_on_inbox(void)
{
    uint8_t tid = mk_current_tid();
    mk_tcb_t *tcb = mk_tcb_get(tid);

    tcb->ipc_blocked = true;
    tcb->state = MK_TASK_BLOCKED;
    mk_sched_unready(tid);

    mk_sched_tick();
}

/* ---- 内部：唤醒一个被 BLOCKED 的任务 ----
 * 注意：先改 ipc_blocked，然后 mk_sched_ready 会设 state + 挂链 */
static void wake_if_blocked(uint8_t tid)
{
    mk_tcb_t *tcb = mk_tcb_get(tid);
    if (tcb->ipc_blocked && tcb->state == MK_TASK_BLOCKED) {
        tcb->ipc_blocked = false;
        mk_sched_ready(tid);   /* 内部改 state=READY + 挂链 */
    }
}

mk_err_t mk_ipc_send(uint8_t to, const mk_msg_t *msg)
{
    uint8_t self = mk_current_tid();
    if (to >= MK_MAX_TASKS || to == self) return MK_ERR_INVALID;
    if (!msg) return MK_ERR_INVALID;

    mk_tcb_t     *dst_tcb  = mk_tcb_get(to);
    mk_mailbox_t *dst_mbox = &g_mboxes[to];

    if (!dst_tcb || dst_tcb->state == MK_TASK_DEAD) return MK_ERR_NOIPC;

    /* 填 from/to */
    mk_msg_t full_msg = *msg;
    full_msg.from = self;
    full_msg.to   = to;

    /* 直接写入 inbox。当前策略：如果已有未读消息，覆盖。 */
    dst_mbox->inbox = full_msg;
    dst_mbox->inbox_has = true;
    dst_tcb->ipc_has_msg = true;

    wake_if_blocked(to);

    /* send 不阻塞 */
    return MK_OK;
}

mk_err_t mk_ipc_receive(mk_msg_t *msg)
{
    if (!msg) return MK_ERR_INVALID;

    uint8_t tid = mk_current_tid();
    mk_tcb_t *tcb = mk_tcb_get(tid);
    mk_mailbox_t *mbox = &g_mboxes[tid];

    if (!mbox->inbox_has) {
        block_on_inbox();   /* 被唤醒后继续 */
    }

    *msg = mbox->inbox;
    mbox->inbox_has = false;
    tcb->ipc_has_msg = false;

    return MK_OK;
}

mk_err_t mk_ipc_reply(uint8_t to, const mk_msg_t *msg)
{
    uint8_t self = mk_current_tid();
    if (to >= MK_MAX_TASKS || to == self) return MK_ERR_INVALID;
    if (!msg) return MK_ERR_INVALID;

    mk_tcb_t     *dst_tcb  = mk_tcb_get(to);
    mk_mailbox_t *dst_mbox = &g_mboxes[to];

    if (!dst_tcb || dst_tcb->state == MK_TASK_DEAD) return MK_ERR_NOIPC;

    mk_msg_t full_msg = *msg;
    full_msg.from = self;
    full_msg.to   = to;

    dst_mbox->inbox = full_msg;
    dst_mbox->inbox_has = true;
    dst_tcb->ipc_has_msg = true;

    wake_if_blocked(to);

    return MK_OK;
}

bool mk_ipc_poll(void)
{
    uint8_t tid = mk_current_tid();
    return g_mboxes[tid].inbox_has;
}
