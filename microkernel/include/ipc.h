/*
 * ipc.h — IPC 三原语：send / receive / reply
 *
 * 微内核风格的同步消息传递：
 *   send    - 发送一条消息到目标任务；如果目标在等这条消息（receive），立即唤醒 sender
 *   receive - 阻塞自己直到某个 sender 给我发消息
 *   reply   - 给之前在等我 reply 的 sender 回一条
 *
 * 每条消息是 mk_msg_t，大小固定。大消息通过页共享（当前原型不做）。
 */
#ifndef MK_IPC_H
#define MK_IPC_H

#include "kernel.h"

#define MK_IPC_MSG_WORDS  16    /* 每条消息最多 16 个 32-bit word = 64 字节 */

typedef struct mk_msg {
    uint8_t  from;              /* 发送者 tid（内核填） */
    uint8_t  to;                /* 接收者 tid（内核填） */
    uint16_t tag;               /* 用户自定义标签，路由用 */
    int32_t  data[MK_IPC_MSG_WORDS];
} mk_msg_t;

/* ---- 每个任务一个 mailbox（极简化：一槽 + 一个 reply waiter） ---- */
typedef struct {
    mk_msg_t   inbox;           /* send 者写入这里；receive 者读出 */
    bool       inbox_has;
    int16_t    reply_to;        /* 当前任务阻塞在 reply 上时等待谁（-1 表示空闲） */
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
