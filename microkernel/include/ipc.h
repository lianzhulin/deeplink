/*
 * ipc.h — IPC 三原语：send / receive / reply
 *
 * 微内核风格的同步消息传递：
 *   send    - 发送一条消息到目标任务；同步阻塞等对方 reply
 *   receive - 阻塞自己直到某个 sender 给我发消息
 *   reply   - 给等我 reply 的 sender 回一条；自己不阻塞
 *
 * 消息体 = 20 字节：tag(2) + from(1) + flags(1) + data[4](16)。
 * 微内核哲学：IPC 传小消息，大消息走页共享（当前原型不做共享页）。
 */
#ifndef MK_IPC_H
#define MK_IPC_H

#include "kernel.h"

/* ---- mk_msg_t：20 字节，无 padding ----
 *
 * 布局：offset 0-3 = tag(2) + from(1) + flags(1)，天然对齐到 int32_t data[0]
 *       offset 4-19 = data[4] = 16 字节 payload
 *       sizeof = 20（尾部 int32_t 对齐补 0，刚好）
 *
 * 删掉了 to 字段：send()/reply() 调用者已知目标 tid；
 * 内核只填 from（send 者身份，receive 方要它来 reply）。 */
typedef struct mk_msg {
    uint16_t  tag;           /* 用户路由标签 */
    uint8_t   from;          /* 内核填：发送者 tid */
    uint8_t   flags;         /* 预留（如：是否需要 reply、优先级等） */
    int32_t   data[4];       /* 4 个 word = 16 字节 payload */
} mk_msg_t;

/* ---- 每个任务一个 mailbox：环形队列 + reply waiter ----
 *
 * 队列大小 = MK_MAX_TASKS（32），意味着"其他所有任务同时 send 到我"
 * 这种最坏情况也能容纳，不会丢消息。
 * reply_to 是单槽：记录最近一个 send 等谁 reply。
 */
#define MK_IPC_QUEUE_SIZE MK_MAX_TASKS

typedef struct {
    mk_msg_t   slots[MK_IPC_QUEUE_SIZE];
    uint8_t    head;       /* 下一个 receive 的位置 */
    uint8_t    tail;       /* 下一个 send 写入的位置 */
    uint8_t    count;      /* 当前有多少条消息在队列里 */
    int16_t    reply_to;   /* 最近一个 send 者的 tid（它在等我 reply）；-1 空闲 */
} mk_mailbox_t;

/* ---- API ---- */
void    mk_ipc_init(void);

/* 阻塞式 send：把 msg 发给 to。
 * 如果对方正 receiveing，直接投递并返回 MK_OK；
 * 否则 sender 不会被阻塞（简化了！当前原型 send 不阻塞）。
 * 想让 send 阻塞等 reply 的任务可以用 reply_to 机制自己实现。 */
mk_err_t mk_ipc_send(uint8_t to, const mk_msg_t *msg);

/* receive：阻塞自己直到 inbox 有消息。
 * 返回时 msg 里 from/to 都填好了。*/
mk_err_t mk_ipc_receive(mk_msg_t *msg);

/* reply：给 reply_to 的 tid 回消息；当前调用者自己不阻塞。 */
mk_err_t mk_ipc_reply(uint8_t to, const mk_msg_t *msg);

/* 非阻塞探测，返回 true 表示有消息可收（可让任务轮询自己） */
bool     mk_ipc_poll(void);

#endif /* MK_IPC_H */
