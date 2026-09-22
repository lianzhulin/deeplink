/*
 * ============================================================================
 *   ipc.c — IPC send / receive / reply 硬化版实现
 * ============================================================================
 *
 *   【实现要点】静态 mk_mailbox_t g_mboxes[32]，每任务一个环形队列。
 *              三原语 send / receive / reply + 两个精确唤醒 helper
 *              wake_send_waiter / wake_recv_waiter（每种只解对应的阻塞）。
 *              新增 cleanup_dead_service：服务者死亡时必须清 inflight +
 *              给每个 client 推一条 synthetic error reply，否则永久 BLOCKED。
 *   【硬化实现】P0: reply_id per slot token —— send 分配唯一 token，
 *              reply 必须带匹配 token；reply_id==0 直接拒不扫 32 slot。
 *              P0: 两种独立阻塞原因 ipc_send_wait / ipc_recv_wait，
 *              精确隔离，零互相污染（旧版一个 bool 互相污染）。
 *              P1: 状态转换合法性 —— BLOCKED → READY 只在 wake_* 里走
 *              mk_sched_ready（内部含 mk_tcb_set_state）。
 *              P2: from / reply_id 内核强制覆盖。
 *              inflight 合并进 reply_id_of_slot：==0 表示未 inflight，>0 就是 token。
 *   【热路径】mk_ipc_send / mk_ipc_receive / mk_ipc_reply —— 每对通信 2 次调度切换，
 *              echo benchmark 就是这个路径。q_push / q_pop 是 static inline，
 *              取模换成位与。
 *   【错误处理】reply 目标死了 → 对方会收到 reply_id 匹配不到 → MK_ERR_INVALID；
 *              服务者死亡 → mk_task_exit 里调 cleanup_dead_service 推合成 error reply，
 *              client 收到后 send_wait 被解，recv 到的 data[0]=MK_ERR_NOIPC；
 *              所有无效输入 (to >= MAX_TASKS / msg == NULL / reply_id == 0) → 返回 MK_ERR_INVALID。

 * ============================================================================
 */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "kernel.h"
#include "task.h"
#include "ipc.h"

/* ---- mailbox 数组 + 全局 reply token 生成器 ---- */
static mk_mailbox_t g_mboxes[MK_MAX_TASKS];
static uint16_t     g_reply_counter;   /* 从 1 开始；0 是 "未 inflight" 哨兵 */

void mk_ipc_init(void)
{
    memset(g_mboxes, 0, sizeof(g_mboxes));
    g_reply_counter = 0;
}

/* ================================================================
 *  环形队列 —— 返回值硬化
 *    q_push: 返回写入的 slot index（后续要登记 reply token）
 *    q_pop:  返回弹出的 slot index
 *
 *  不变量（由同步 send 语义保证）：
 *    每个任务同时最多 1 条 inflight send → 一个 mailbox 最多积压 N-1 条消息。
 *    QUEUE_SIZE = N → q_push 永远不会溢出。
 * ================================================================ */
static inline bool q_empty(const mk_mailbox_t *m) { return m->count == 0; }

/* L1 优化：static inline + 位与代替模（QUEUE_SIZE=32=2^5 → mask=31） */
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

/* ================================================================
 *  两种精确唤醒 —— 每种只解对应的那一种阻塞
 * ================================================================ */

/* 解 "send 阻塞等 reply" —— 只有 reply() 能调 */
static inline void wake_send_waiter(uint8_t tid)
{
    mk_tcb_t *tcb = mk_tcb_get(tid);
    if (tcb->ipc_send_wait && tcb->state == MK_TASK_BLOCKED) {
        tcb->ipc_send_wait = false;
        mk_sched_ready(tid);
    }
}

/* 解 "receive 阻塞等消息" —— send() 或 reply() 投递消息时调 */
static inline void wake_recv_waiter(uint8_t tid)
{
    mk_tcb_t *tcb = mk_tcb_get(tid);
    if (tcb->ipc_recv_wait && tcb->state == MK_TASK_BLOCKED) {
        tcb->ipc_recv_wait = false;
        mk_sched_ready(tid);
    }
}

/* ================================================================
 *  send — 硬化版
 *
 *  分配 reply token → 内核写 from/reply_id → 登记 reply_id_of_slot → 入队
 *  自己 send_wait 阻塞等 reply
 *
 *  不变量：同步 send + QUEUE_SIZE = N → q_push 永远不溢出。
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

    /* 分配 token；从 1 开始递增，0 永远是 "未 inflight" 哨兵 */
    uint16_t reply_id = (uint16_t)(++g_reply_counter);
    if (reply_id == 0) reply_id = 1;   /* 防 wrap 到 0 */

    mk_msg_t full_msg = *msg;
    full_msg.from     = self;
    full_msg.reply_id = reply_id;

    uint8_t slot = q_push(dst_mbox, &full_msg);
    dst_mbox->reply_id_of_slot[slot] = reply_id;   /* 登记 inflight */

    wake_recv_waiter(to);

    me->ipc_send_wait = true;
    mk_tcb_set_state(me, MK_TASK_BLOCKED);
    mk_sched_unready(self);

    mk_sched_tick();

    me->ipc_send_wait = false;
    return MK_OK;
}

/* ================================================================
 *  receive — 硬化版
 *
 *  队空 → recv_wait 阻塞；队列有消息 → pop
 *  被唤醒后循环检查队列（防御多次 wake 间队列被消耗空）
 *
 *  注意：**receive 不清 reply_id_of_slot**！
 *    reply_id_of_slot != 0 表示 "这条 send 还在等 reply"，不是 "还在队列里"。
 *    send 消息被 receive 取走只是让服务者看到了请求，
 *    但 send 者的 send_wait 还在阻塞等 reply。
 *    reply_id_of_slot 只能由 reply() 匹配成功后清 0。
 * ================================================================ */
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

/* ================================================================
 *  reply — 硬化版
 *
 *  1. 在自己的 mailbox 里找 reply_id 匹配的 slot → 清 reply_id_of_slot=0
 *     这是 "这笔账我结了" 的意思。没匹配到 → 返回 MK_ERR_INVALID。
 *  2. 投递 reply 消息到对方 inbox（解 recv_wait）。
 *     不变量：对方同时最多 1 条 inflight send，reply 积压最多 1 条。
 *  3. 解对方的 send_wait —— 那个被我们匹配到的 send 者。
 *
 *  reply 和 send 的根本区别：reply 不阻塞，立即返回。
 *  send 则是投递消息 + send_wait 阻塞等 reply 解。
 * ================================================================ */
mk_err_t mk_ipc_reply(uint8_t to, const mk_msg_t *msg)
{
    uint8_t self = mk_current_tid();
    if (to >= MK_MAX_TASKS || to == self) return MK_ERR_INVALID;
    if (!msg) return MK_ERR_INVALID;

    /* P0-2 硬化：reply_id==0 直接拒，不扫 32 个 slot。
     * 开发者忘了写 reply.reply_id = req.reply_id 时会撞到这里。 */
    if (msg->reply_id == 0) return MK_ERR_INVALID;

    mk_tcb_t     *to_tcb   = mk_tcb_get(to);
    mk_mailbox_t *dst_mbox = &g_mboxes[to];      /* reply 目标的 mailbox (send 者的 inbox) */
    mk_mailbox_t *my_mbox  = &g_mboxes[self];     /* 我 reply 者自己的 mailbox */

    if (!to_tcb || to_tcb->state == MK_TASK_DEAD) return MK_ERR_NOIPC;

    /* ---- 关键校验：reply_id 必须匹配我 mailbox 里某个 inflight slot ---- */
    bool inflight_found = false;

    for (uint8_t i = 0; i < MK_IPC_QUEUE_SIZE; ++i) {
        /* reply_id_of_slot[i] == msg->reply_id 隐含了 != 0，所以既是 inflight 又是匹配 */
        if (my_mbox->reply_id_of_slot[i] == msg->reply_id) {
            my_mbox->reply_id_of_slot[i] = 0;   /* 清 inflight */
            inflight_found = true;
            break;
        }
    }

    if (!inflight_found) {
        return MK_ERR_INVALID;
    }

    /* ---- 投递 reply 消息到对方 inbox ---- */
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

/* ================================================================
 *  mk_ipc_cleanup_dead_service — 服务者死亡时清理它的 IPC 状态
 *
 *  必须在 mk_task_exit 调 —— 否则：
 *    client 正在 send_wait 等这个服务者 reply
 *    服务者死了 → reply_id_of_slot 永远不被清 → 永远不 wake_send_waiter
 *    → 那些 client 永久 BLOCKED，整个系统卡死
 *
 *  两步：
 *    1. 扫 reply_id_of_slot[] 里所有非零 slot —— 每个对应一个还在等 reply 的 client
 *       给每个 client 推一条 synthetic error reply（tag=0 表示"服务已死"）
 *       调 wake_send_waiter 解它的阻塞
 *    2. 清空 mailbox 队列 —— 时刻 A（send 刚入队还没被 receive）的残留
 * ================================================================ */
void mk_ipc_cleanup_dead_service(uint8_t dead_tid)
{
    if (dead_tid >= MK_MAX_TASKS) return;

    mk_mailbox_t *my = &g_mboxes[dead_tid];

    for (uint8_t i = 0; i < MK_IPC_QUEUE_SIZE; ++i) {
        if (my->reply_id_of_slot[i] == 0) continue;   /* 未 inflight，跳过 */

        uint8_t  client   = my->slots[i].from;
        uint16_t reply_id = my->reply_id_of_slot[i];

        /* 清 inflight（我死了，这笔账结不了） */
        my->reply_id_of_slot[i] = 0;

        /* 防御性边界：from 必须是合法 tid */
        if (client >= MK_MAX_TASKS) continue;

        /* 给 client 推 synthetic reply —— tag=0 约定"服务已死"
         * reply_id 必须是那个 client send 时内核分配的 token */
        mk_msg_t err;
        memset(&err, 0, sizeof(err));
        err.reply_id = reply_id;
        err.from     = dead_tid;
        err.data[0]  = MK_ERR_NOIPC;

        mk_mailbox_t *dst = &g_mboxes[client];
        q_push(dst, &err);

        /* 解 client 的 send_wait —— 相当于 reply() 做的事 */
        wake_send_waiter(client);
    }

    /* 清空残留队列 —— 时刻 A 的 send 还没被 receive */
    while (!q_empty(my)) {
        q_pop(my, NULL);
    }
}
