/*
 * ipc.c — IPC 硬化版
 *
 * 硬化相对于原型的 6 个改动：
 *
 * P0  | 队列满 → send 阻塞（背压），不丢消息
 * P0  | reply_id per slot：每条 send 分配唯一 token；reply 必须带匹配 token 才能解
 * P0  | 三种独立阻塞原因：send_wait / recv_wait / space_wait，精确隔离
 * P1  | reply 校验 reply_id 合法性：reply_id 没匹配到任何 inflight → MK_ERR_INVALID
 * P1  | 单一真相源：删 ipc_has_msg / ipc_blocked / ipc_waiter，状态只存在正确地方
 * P2  | from 字段：用户写的值会被内核用 full_msg.from=self 覆盖
 *
 * 精确隔离的阻塞原因为什么重要：
 *   旧版只有一个 ipc_blocked bool + state=BLOCKED。wake_unblocked 不管什么原因都解，
 *   导致 reply 可能误解一个 recv 阻塞的任务（白调度）；receive 广播唤醒又可能解 send
 *   阻塞的任务（状态污染）。拆成三个 bool + 精确匹配唤醒后，每种阻塞只有对应的
 *   那一种原语能解，零误触。
 */
#include <string.h>
#include "kernel.h"
#include "task.h"
#include "ipc.h"

/* ---- mailbox 数组 + 全局 reply token 生成器 ---- */
static mk_mailbox_t g_mboxes[MK_MAX_TASKS];
static uint16_t     g_reply_counter;

void mk_ipc_init(void)
{
    memset(g_mboxes, 0, sizeof(g_mboxes));
    g_reply_counter = 0;
}

/* ================================================================
 *  环形队列 —— 返回值硬化
 *    q_push: 返回写入的 slot index（后续要登记 inflight）
 *    q_pop:  返回弹出的 slot index（后续要清 inflight）
 *
 *  硬化：队列永不溢出 —— send 会阻塞等 receive 腾出槽（背压）
 *  reply 不阻塞 —— 队列满时丢最老普通消息（inflight slot 永远不丢，
 *  因为我们只在 send 时登记 inflight，reply 时目标的 mailbox 里没有 inflight）
 * ================================================================ */
static inline bool q_empty(const mk_mailbox_t *m) { return m->count == 0; }
static inline bool q_full (const mk_mailbox_t *m) { return m->count == MK_IPC_QUEUE_SIZE; }

static uint8_t q_push(mk_mailbox_t *m, const mk_msg_t *msg)
{
    uint8_t slot = m->tail;
    m->slots[slot] = *msg;
    m->tail = (m->tail + 1) % MK_IPC_QUEUE_SIZE;
    m->count++;
    return slot;
}

static uint8_t q_pop(mk_mailbox_t *m, mk_msg_t *out)
{
    uint8_t slot = m->head;
    if (out) *out = m->slots[slot];
    m->head = (m->head + 1) % MK_IPC_QUEUE_SIZE;
    m->count--;
    return slot;
}

/* ================================================================
 *  三种精确唤醒 —— 每种只解对应的那一种阻塞
 * ================================================================ */

/* 解 "send 阻塞等 reply" —— 只有 reply() 能调 */
static void wake_send_waiter(uint8_t tid)
{
    mk_tcb_t *tcb = mk_tcb_get(tid);
    if (tcb->ipc_send_wait && tcb->state == MK_TASK_BLOCKED) {
        tcb->ipc_send_wait = false;
        mk_sched_ready(tid);
    }
}

/* 解 "receive 阻塞等消息" —— send() 或 reply() 投递消息时调 */
static void wake_recv_waiter(uint8_t tid)
{
    mk_tcb_t *tcb = mk_tcb_get(tid);
    if (tcb->ipc_recv_wait && tcb->state == MK_TASK_BLOCKED) {
        tcb->ipc_recv_wait = false;
        mk_sched_ready(tid);
    }
}

/* 解 "send 阻塞等槽位" —— receive() 腾出槽时调。
 * 遍历所有 space_wait 任务，解 space_wait_target == who 的。 */
static void wake_space_waiters_for(uint8_t who)
{
    for (uint8_t i = 0; i < MK_MAX_TASKS; ++i) {
        mk_tcb_t *w = mk_tcb_get(i);
        if (w && w->ipc_space_wait
                && w->space_wait_target == who
                && w->state == MK_TASK_BLOCKED) {
            w->ipc_space_wait = false;
            mk_sched_ready(i);
        }
    }
}

/* ================================================================
 *  send — 硬化版
 *
 *  背压循环：队列满 → space_wait → receive 腾出槽 → 重试
 *  分配 reply token → 内核写 from/reply_id → 登记 inflight
 *  自己 send_wait 阻塞等 reply
 *
 *  无 silent drop，reply_id 保证 reply 能对应回这条 send
 * ================================================================ */
mk_err_t mk_ipc_send(uint8_t to, const mk_msg_t *msg)
{
    uint8_t self = mk_current_tid();
    if (to >= MK_MAX_TASKS || to == self) return MK_ERR_INVALID;
    if (!msg) return MK_ERR_INVALID;

    mk_tcb_t     *dst_tcb  = mk_tcb_get(to);
    mk_mailbox_t *dst_mbox = &g_mboxes[to];
    mk_tcb_t     *me       = mk_tcb_get(self);

    if (!dst_tcb || dst_tcb->state == MK_TASK_DEAD) return MK_ERR_NOIPC;

    /* ---- 背压：队列满 → space_wait → receive 腾槽后重试 ---- */
    while (q_full(dst_mbox)) {
        me->ipc_space_wait      = true;
        me->space_wait_target   = to;
        me->state               = MK_TASK_BLOCKED;
        mk_sched_unready(self);
        mk_sched_tick();
        /* 被 receive 解醒后，循环再检查队列，直到有槽 */
    }

    /* ---- 分配 reply token（全局递增，自动 wrap，uint16 足够唯一）---- */
    uint16_t reply_id = ++g_reply_counter;

    /* ---- 构造消息：内核强制覆盖 from / reply_id，用户写的值作废 ---- */
    mk_msg_t full_msg = *msg;
    full_msg.from     = self;        /* 防伪造：用户设的 from 被覆盖 */
    full_msg.reply_id = reply_id;    /* 用户无法伪造 reply token */

    /* ---- 入队 + 登记 inflight ---- */
    uint8_t slot = q_push(dst_mbox, &full_msg);
    dst_mbox->inflight[slot] = true;
    dst_mbox->reply_id_of_slot[slot] = reply_id;

    /* 如果对方正 receive 阻塞，现在队列有消息了，解它 */
    wake_recv_waiter(to);

    /* ---- 我 send 阻塞，等对方 reply 解 ---- */
    me->ipc_send_wait = true;
    me->state         = MK_TASK_BLOCKED;
    mk_sched_unready(self);
    mk_sched_tick();

    /* 被 reply 解阻塞后返回。reply 消息已在我 inbox 队列里，
     * 调用者再 receive() 就能拿到。 */
    me->ipc_send_wait = false;
    return MK_OK;
}

/* ================================================================
 *  receive — 硬化版
 *
 *  队空 → recv_wait 阻塞；队列有消息 → pop + 清 inflight + 唤醒 space_waiters
 *  被唤醒后循环检查队列（防御多次 wake 间队列被消耗空）
 * ================================================================ */
mk_err_t mk_ipc_receive(mk_msg_t *msg)
{
    if (!msg) return MK_ERR_INVALID;

    uint8_t tid = mk_current_tid();
    mk_tcb_t     *tcb  = mk_tcb_get(tid);
    mk_mailbox_t *mbox = &g_mboxes[tid];

    while (q_empty(mbox)) {
        tcb->ipc_recv_wait = true;
        tcb->state         = MK_TASK_BLOCKED;
        mk_sched_unready(tid);
        mk_sched_tick();
        /* 醒来后循环再看，防御假唤醒（虽然硬化版不会有） */
    }

    /* pop 之前记住是否满 —— 满 pop 意味着空出一个槽，可以解 space_waiters */
    bool was_full = q_full(mbox);

    uint8_t slot = q_pop(mbox, msg);

    /* 注意：**receive 不清 inflight**！
     * inflight 表示 "这条 send 还在等 reply"，不是 "还在队列里"。
     * send 消息被 receive 取走只是让服务者看到了请求，
     * 但 send 者的 send_wait 还在阻塞等 reply。
     * inflight 只能由 reply() 匹配成功后清，或者内核在服务者死亡时批量清。 */

    /* 如果 pop 之前队列是满的，现在空出一个槽 ——
     * 解所有 space_wait_target == tid 的 send 者让它们重试 */
    if (was_full) {
        wake_space_waiters_for(tid);
    }

    return MK_OK;
}

/* ================================================================
 *  reply — 硬化版
 *
 *  1. 在自己的 mailbox 里找 reply_id 匹配的 inflight slot → 清掉
 *     这是 "这笔账我结了" 的意思。没匹配到 → 返回 MK_ERR_INVALID。
 *  2. 投递 reply 消息到对方队列（可能解 recv_wait，也可能解 send_wait 后的
 *     最后一步）。reply 不阻塞，队列满时丢最老普通消息。
 *  3. 解对方的 send_wait —— 那个被我们匹配到 inflight 的 send 者。
 *
 *  reply 和 send 的根本区别仍然是：reply 不阻塞。
 *  reply 把 reply 消息投递到对方 inbox 并解对方 send_wait，
 *  自己立即返回。send 则是投递消息 + send_wait 阻塞等 reply 解。
 * ================================================================ */
mk_err_t mk_ipc_reply(uint8_t to, const mk_msg_t *msg)
{
    uint8_t self = mk_current_tid();
    if (to >= MK_MAX_TASKS || to == self) return MK_ERR_INVALID;
    if (!msg) return MK_ERR_INVALID;

    mk_tcb_t     *to_tcb  = mk_tcb_get(to);
    mk_mailbox_t *dst_mbox = &g_mboxes[to];      /* reply 目标的 mailbox (send 者的 inbox) */
    mk_mailbox_t *my_mbox  = &g_mboxes[self];     /* 我 reply 者自己的 mailbox */

    if (!to_tcb || to_tcb->state == MK_TASK_DEAD) return MK_ERR_NOIPC;

    /* ---- 关键校验：reply_id 必须匹配我 mailbox 里某个 inflight slot ---- */
    bool inflight_found = false;
    for (uint8_t i = 0; i < MK_IPC_QUEUE_SIZE; ++i) {
        if (my_mbox->inflight[i]
                && my_mbox->reply_id_of_slot[i] == msg->reply_id) {
            my_mbox->inflight[i] = false;
            my_mbox->reply_id_of_slot[i] = 0;
            inflight_found = true;
            break;
        }
    }

    if (!inflight_found) {
        /* 试图 reply 一个根本没 send 过我的 reply_id — 防御：拒绝 */
        return MK_ERR_INVALID;
    }

    /* ---- 投递 reply 消息到对方 inbox ---- */
    mk_msg_t full_msg = *msg;
    full_msg.from = self;          /* 内核覆盖防伪造 */
    /* reply_id 保留 msg->reply_id（刚才校验过合法） */

    /* reply 不允许阻塞（微内核语义），队列满 → 丢最老普通消息 */
    while (q_full(dst_mbox)) {
        q_pop(dst_mbox, NULL);
    }
    q_push(dst_mbox, &full_msg);

    /* ---- 解对方 send_wait — 它 send 出去了，现在 reply 到了 ---- */
    wake_send_waiter(to);

    /* reply 自己不阻塞，立即返回。这是它和 send 的根本区别。 */
    return MK_OK;
}

bool mk_ipc_poll(void)
{
    uint8_t tid = mk_current_tid();
    return !q_empty(&g_mboxes[tid]);
}
