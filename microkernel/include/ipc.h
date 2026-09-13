/*
 * ipc.h — IPC 三原语硬化版：send / receive / reply
 *
 * 硬化要点：
 *   - 队列满 → send 阻塞（背压），不丢消息
 *   - reply_id per slot：每条 send 入队时内核分配唯一 reply token；
 *     reply 必须携带匹配 token 才能解阻塞，否则返回 MK_ERR_INVALID
 *   - 三种独立阻塞原因：send_wait / recv_wait / space_wait
 *   - 单一真相源：inbox 空/满统一看 mbox.count，不再有 ipc_has_msg
 *
 * 消息体 = 24 字节：tag(2) + reply_id(2) + from(1) + flags(1) + pad(2) + data[4](16)。
 * 微内核哲学：IPC 传小消息，大消息走页共享。
 */
#ifndef MK_IPC_H
#define MK_IPC_H

#include "kernel.h"

/* ---- mk_msg_t：24 字节 ----
 *
 * 布局 (天然对齐):
 *   offset  0-1: tag      uint16_t  用户路由
 *   offset  2-3: reply_id uint16_t 内核生成的 reply token (reply 必须携带才能解 send)
 *   offset  4-4: from     uint8_t  内核填发送者 tid (用户写的会被覆盖)
 *   offset  5-5: flags    uint8_t  预留
 *   offset  6-7: padding  2 bytes  让 data[0] 对齐到 int32_t
 *   offset  8-23: data[4] int32[4] 16 字节 payload
 *   sizeof = 24
 *
 * 硬化后 reply_to 从 mailbox 单槽变成 per-slot 的 reply_id_of_slot[]：
 *   每条 send 进队列时 reply_id_of_slot[slot] = 内核分配的 token
 *   reply 时必须 reply.reply_id 匹配某条 inflight slot 才能解阻塞 */
typedef struct mk_msg {
    uint16_t  tag;
    uint16_t  reply_id;   /* 内核生成；reply 必须带匹配 token 才能解 send 阻塞 */
    uint8_t   from;       /* 内核填；用户设的值会被覆盖 */
    uint8_t   flags;
    int32_t   data[4];
} mk_msg_t;

/* ---- 每个任务一个 mailbox：环形队列 + per-slot inflight ----
 *
 * MK_IPC_QUEUE_SIZE = MK_MAX_TASKS (32)：
 *   - 足够容纳所有其他任务同时 send 过来的极端情况
 *   - 队列永不溢出（send 阻塞等 receive 腾出槽 — 背压）
 *
 * 旧版单槽 reply_to 被拆成 per-slot 状态：
 *   inflight[slot]        = true 表示 slot 那条 send 还在等 reply
 *   reply_id_of_slot[slot] = 那条 send 的 reply token
 *   reply 时 reply_id 必须匹配才能解 send 阻塞 */
#define MK_IPC_QUEUE_SIZE MK_MAX_TASKS

typedef struct {
    mk_msg_t  slots[MK_IPC_QUEUE_SIZE];
    uint8_t   head;
    uint8_t   tail;
    uint8_t   count;
    bool      inflight[MK_IPC_QUEUE_SIZE];        /* slot i 是否在等 reply */
    uint16_t  reply_id_of_slot[MK_IPC_QUEUE_SIZE]; /* slot i 的 reply token */
} mk_mailbox_t;

/* ---- API ---- */
void    mk_ipc_init(void);

/* send: 投递消息到 to。to 的队列满则阻塞自己等 receive 腾槽。
 *       内核分配 reply_id 写入 msg.reply_id，reply 必须带它回来。 */
mk_err_t mk_ipc_send(uint8_t to, const mk_msg_t *msg);

/* receive: 阻塞自己等 send/reply 投递到自己 mailbox。
 *          返回时 msg 里 from / tag / reply_id 都填好。 */
mk_err_t mk_ipc_receive(mk_msg_t *msg);

/* reply: 给 reply_id 匹配的 inflight send 解阻塞。自己不阻塞。
 *        如果 reply_id 没匹配到任何 inflight slot，返回 MK_ERR_INVALID。 */
mk_err_t mk_ipc_reply(uint8_t to, const mk_msg_t *msg);

/* 非阻塞探测：自己的 mailbox 队是否空 */
bool     mk_ipc_poll(void);

#endif /* MK_IPC_H */
