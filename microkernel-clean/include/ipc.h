/*
 * ============================================================================
 *   ipc.h — IPC 三原语硬化版：send / receive / reply
 * ============================================================================
 *
 *   【架构角色】内核消息传递子系统。提供 send / receive / reply 三原语，
 *              让独立任务通过 mailbox 环形队列通信。依赖 kernel.h；
 *              被 task.c / main.c / mm.c 依赖。
 *   【硬化要点】reply_id per slot token 机制：send 分配唯一 reply token，
 *              reply 必须带匹配 token 才能解 send 阻塞；
 *              两种独立阻塞原因 ipc_send_wait / ipc_recv_wait，精确隔离，
 *              零互相污染；from / reply_id 内核强制覆盖，用户写的作废；
 *              inflight 状态由 reply_id_of_slot[i]==0 编码，省掉独立 bool[N]。
 *   【性能要点】环形队列用静态 inline q_push / q_pop，
 *              取模换成位与 (tail = (slot+1) & (QUEUE_SIZE-1))；
 *              三原语全部是任务上下文函数，无锁、无 atomics —— 单线程调度器天然串行。
 *   【不变量】send 是同步的 → 每任务最多 1 条 inflight send；
 *              QUEUE_SIZE = MK_MAX_TASKS → 一个 mailbox 最多积压 N-1 条，永不溢出；
 *              reply_id 从 1 开始递增，0 永远是 "未 inflight" 哨兵。
 *   【约束】reply 必须携带 req.reply_id，否则返回 MK_ERR_INVALID；
 *              reply_id==0 被直接拒绝 (不会扫描 32 个 slot)；
 *              send 目标不能是自己。
 *   【数据结构】mk_msg_t = 24B (tag 2 + reply_id 2 + from 1 + flags 1 + pad 2 + data[4] 16)；
 *              mk_mailbox_t 含 32 slot + head/tail/count + reply_id_of_slot[32] ≈ 850B。
 *   【线程安全】全部原语只在任务上下文调用，调度器串行化访问。

 * ============================================================================
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
 * reply_id 语义：
 *   发送前由内核分配 (g_reply_counter 递增，从 1 开始)。
 *   0 永远不是合法 token —— 被复用为 "slot 没有 inflight" 的哨兵值。 */
typedef struct mk_msg {
    uint16_t  tag;
    uint16_t  reply_id;   /* 内核生成；reply 必须带匹配 token 才能解 send 阻塞 */
    uint8_t   from;       /* 内核填；用户设的值会被覆盖 */
    uint8_t   flags;
    int32_t   data[4];
} mk_msg_t;

/* ---- 每个任务一个 mailbox：环形队列 + per-slot reply_id ----
 *
 * MK_IPC_QUEUE_SIZE = MK_MAX_TASKS (32)：
 *   不变量：send 是同步的 → 每个任务同时最多 1 条 inflight send。
 *   一个 mailbox 最多积压 MAX_TASKS-1 = 31 条（其他所有任务各 1 条）。
 *   32 槽 ≥ 31 积压 → 队列永不溢出。不需要背压阻塞。
 *
 * inflight 状态由 reply_id_of_slot 本身编码：
 *   reply_id_of_slot[i] == 0   → slot i 没有 inflight send
 *   reply_id_of_slot[i] == tok → slot i 正在等 reply token == tok */
#define MK_IPC_QUEUE_SIZE MK_MAX_TASKS

typedef struct {
    mk_msg_t  slots[MK_IPC_QUEUE_SIZE];
    uint8_t   head;
    uint8_t   tail;
    uint8_t   count;
    uint16_t  reply_id_of_slot[MK_IPC_QUEUE_SIZE]; /* 0=未 inflight, >0=reply token */
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

/* 内核内部用：服务者死亡时清 inflight + 解所有阻塞 client。
 * 必须在 mk_task_exit 里调，否则 client 会永久 BLOCKED。
 * 普通任务不应直接调 —— 由 task 模块在 exit 时自动触发。 */
void     mk_ipc_cleanup_dead_service(uint8_t dead_tid);


#endif /* MK_IPC_H */
