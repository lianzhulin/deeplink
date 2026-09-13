/*
 * ipc.c — IPC 三原语 send / receive / reply（经典同步消息传递）
 *
 * 语义（模仿 L4 / QNX / Mach 的 rendezvous 风格）：
 *
 *   Client                       Server
 *     |-- send(server, req) --->|  client BLOCKED 等 reply
 *     |                         |  receive() 取出 req
 *     |                         |  处理请求
 *     |<-- reply(client, res) --|  reply 不阻塞，立即 unblock client
 *     |  (send 返回)             |
 *
 * mailbox = 4 槽环形队列 + reply_to 字段。
 *   队列满时 send 覆盖最老消息（原型简化，不阻塞）。
 *   reply_to 记录"最近一个 send 等谁 reply"。
 *
 * 原型简化策略：
 *   - send 永不因队列满而阻塞（直接覆盖最老），只在等 reply 时阻塞
 *   - receive 队空才阻塞，醒来只可能来自 send 或 reply 的 wake
 *   - 没有"send 因队列满而阻塞 → receive 空槽唤醒它"的状态机
 *     （原型阶段简化，避免 receive 里广播唤醒所有 BLOCKED 造成无限循环）
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
        g_mboxes[i].head = g_mboxes[i].tail = g_mboxes[i].count = 0;
    }
}

/* ---- 环形队列 ---- */
static inline bool q_empty(const mk_mailbox_t *m) { return m->count == 0; }
static inline bool q_full (const mk_mailbox_t *m) { return m->count == MK_IPC_QUEUE_SIZE; }

static void q_push(mk_mailbox_t *m, const mk_msg_t *msg)
{
    m->slots[m->tail] = *msg;
    m->tail = (m->tail + 1) % MK_IPC_QUEUE_SIZE;
    m->count++;
}

static void q_pop(mk_mailbox_t *m, mk_msg_t *out)
{
    if (out) *out = m->slots[m->head];
    m->head = (m->head + 1) % MK_IPC_QUEUE_SIZE;
    m->count--;
}

/* ---- 内部阻塞 / 唤醒 ---- */
static void block_self(void)
{
    uint8_t tid = mk_current_tid();
    mk_tcb_t *tcb = mk_tcb_get(tid);

    tcb->ipc_blocked = true;
    tcb->state = MK_TASK_BLOCKED;
    mk_sched_unready(tid);

    mk_sched_tick();
}

static void wake_unblocked(uint8_t tid)
{
    mk_tcb_t *tcb = mk_tcb_get(tid);
    if (tcb->ipc_blocked && tcb->state == MK_TASK_BLOCKED) {
        tcb->ipc_blocked = false;
        mk_sched_ready(tid);
    }
}

/* ================================================================
 *  send — 同步发送，阻塞等对方 reply
 *
 *  只有一层阻塞：等对方 reply。
 *  队列满时直接丢最老的（原型简化，不做"send 因队列满阻塞"的状态机）。
 * ================================================================ */
mk_err_t mk_ipc_send(uint8_t to, const mk_msg_t *msg)
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

    /* 队列满 → 丢最老的，再 push。原型简化，避免阻塞 send。 */
    if (q_full(dst_mbox)) {
        q_pop(dst_mbox, NULL);
    }
    q_push(dst_mbox, &full_msg);
    dst_tcb->ipc_has_msg = true;

    /* 登记：我是 "to 要 reply 的对象" */
    dst_mbox->reply_to = self;

    /* 如果对方正 receive 阻塞，唤醒它来处理这条消息 */
    wake_unblocked(to);

    /* **send 自己阻塞，等对方 reply 唤醒** */
    block_self();

    /* 被 reply() 唤醒后返回。对方的 reply 消息已经在我的 inbox 队列里。 */
    return MK_OK;
}

/* ================================================================
 *  receive — 取队列头的消息；队空就阻塞
 *
 *  阻塞只可能被 send() 或 reply() 的 wake_unblocked 解。
 *  醒来后重新检查队列（可能被多次 push 多条）。
 * ================================================================ */
mk_err_t mk_ipc_receive(mk_msg_t *msg)
{
    if (!msg) return MK_ERR_INVALID;

    uint8_t tid = mk_current_tid();
    mk_tcb_t     *tcb  = mk_tcb_get(tid);
    mk_mailbox_t *mbox = &g_mboxes[tid];

    while (q_empty(mbox)) {
        block_self();
        /* 醒来后循环再看：可能唤醒者 push 了消息，也可能是假唤醒（原型里不会有） */
    }

    q_pop(mbox, msg);
    tcb->ipc_has_msg = !q_empty(mbox);

    return MK_OK;
}

/* ================================================================
 *  reply — 回复对方，自己不阻塞。send 的 "解结者"。
 *
 *  把 reply 消息投递到对方队列，唤醒对方（不管它是 send 阻塞等 reply，
 *  还是 receive 阻塞等任何消息）。自己立即返回。
 * ================================================================ */
mk_err_t mk_ipc_reply(uint8_t to, const mk_msg_t *msg)
{
    uint8_t self = mk_current_tid();
    if (to >= MK_MAX_TASKS || to == self) return MK_ERR_INVALID;
    if (!msg) return MK_ERR_INVALID;

    mk_tcb_t     *dst_tcb  = mk_tcb_get(to);
    mk_mailbox_t *dst_mbox = &g_mboxes[to];
    mk_mailbox_t *my_mbox  = &g_mboxes[self];

    if (!dst_tcb || dst_tcb->state == MK_TASK_DEAD) return MK_ERR_NOIPC;

    mk_msg_t full_msg = *msg;
    full_msg.from = self;
    full_msg.to   = to;

    /* 投递 reply。队列满 → 丢最老。原型不阻塞 reply。 */
    if (q_full(dst_mbox)) {
        q_pop(dst_mbox, NULL);
    }
    q_push(dst_mbox, &full_msg);
    dst_tcb->ipc_has_msg = true;

    /* 清掉自己 mailbox 的 reply_to —— 这笔账结清了 */
    my_mbox->reply_to = -1;

    /* 唤醒对方。对方可能是在 send() 里阻塞等我 reply，
     * 也可能是在 receive() 里阻塞等任何消息。 */
    wake_unblocked(to);

    /* reply 自己不阻塞，立即返回。这是它和 send 的根本区别。 */
    return MK_OK;
}

bool mk_ipc_poll(void)
{
    uint8_t tid = mk_current_tid();
    return !q_empty(&g_mboxes[tid]);
}
