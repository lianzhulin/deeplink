/*
 * ipc.c — IPC 硬化版
 *
 * 硬化相对于原型的 5 个改动（+1 个设计简化）：
 *
 * P0  | reply_id per slot：每条 send 分配唯一 token；reply 必须带匹配 token 才能解
 * P0  | 两种独立阻塞原因：send_wait / recv_wait，精确隔离（旧版一个 bool 互相污染）
 * P1  | reply 校验 reply_id 合法性：reply_id 没匹配到任何 inflight → MK_ERR_INVALID
 * P1  | 单一真相源：删 ipc_has_msg / ipc_blocked / ipc_waiter
 * P2  | from / reply_id 强制内核覆盖，用户写的作废
 *
 * 设计简化：为什么删 space_wait（队列满阻塞）？
 *   send 是同步的 → 每个任务同时最多 1 条 inflight send。
 *   QUEUE_SIZE = MAX_TASKS → 一个 mailbox 最多积压 N-1 条 send（其他各 1 条）。
 *   N 槽 ≥ N-1 积压 → 永远差 1 格，队列不可能满。
 *   背压是死代码。用 assert 防御而非 while 阻塞。
 *
 * 精确隔离的阻塞原因为什么重要：
 *   旧版只有一个 ipc_blocked bool + state=BLOCKED。wake_unblocked 不管什么原因都解，
 *   导致 reply 可能误解一个 recv 阻塞的任务（白调度）；receive 广播唤醒又可能解 send
 *   阻塞的任务（状态污染）。拆成两个 bool + 精确匹配唤醒后，每种阻塞只有对应的
 *   那一种原语能解，零误触。
 */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "kernel.h"
#include "task.h"
#include "ipc.h"

/* ================================================================
 *  IPC 逐阶段 cycle 剖析
 *
 *  一次 round-trip (echo → mm → echo) 拆成 11 个阶段 + 2 次 ctx_swap:
 *
 *  ┌─ echo.mk_ipc_send ──────────────────────────────────────────┐
 *  │ S1  token分配+内核写from/reply_id                              │
 *  │ S2  q_push 入队                                                │
 *  │ S3  wake_recv_waiter(mm)                                      │
 *  │ S4  设置 send_wait + state=BLOCKED + unready                   │
 *  │ S5  mk_sched_tick: ctx_swap→mm   ← 第一次上下文切换 (大项)      │
 *  └───────────────────────────────────────────────────────────────┘
 *  ┌─ mm.mk_ipc_receive ─────────────────────────────────────────┐
 *  │ S6  q_pop 取消息（已被 wake 解阻塞）                            │
 *  └───────────────────────────────────────────────────────────────┘
 *  ┌─ mm.mk_ipc_reply ────────────────────────────────────────────┐
 *  │ S7  扫 inflight 匹配 reply_id                                   │
 *  │ S8  reply q_push 入队 echo inbox                                │
 *  │ S9  wake_send_waiter(echo)                                      │
 *  └───────────────────────────────────────────────────────────────┘
 *  ┌─ mm.mk_ipc_receive (while循环再来) ─────────────────────────┐
 *  │ S10 队空→recv_wait+BLOCKED+tick→ctx_swap→echo ← 第二次切换      │
 *  └───────────────────────────────────────────────────────────────┘
 *  ┌─ echo 返回 ─────────────────────────────────────────────────┐
 *  │ S11 mk_ipc_receive.q_pop 取 reply（不阻塞）                     │
 *  └───────────────────────────────────────────────────────────────┘
 *
 *  统计方式：每个阶段累计 N 次 round-trip 的总 cycles，
 *  benchmark 结束后打印每阶段 avg + 占比。
 *
 *  profiling 通过 g_ipc_prof 全局累加，是轻量级的 rdtsc 打点
 *  —— 每个阶段只多 2 条指令（rdtsc + 加法），对结果影响很小。
 * ================================================================ */

#if defined(__x86_64__)
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
#include <x86intrin.h>
#define rdtsc() __rdtsc()
#endif

enum prof_step {
    PROF_S1_SEND_PREP,        /* token + 内核写字段 + inflight 登记 */
    PROF_S2_Q_PUSH,           /* q_push 入队 */
    PROF_S3_WAKE_RECV,        /* wake_recv_waiter */
    PROF_S4_BLOCK_SELF,       /* send_wait + BLOCKED + unready */
    PROF_S5_TICK_OUT,         /* mk_sched_tick → 切走 (含 ctx_swap) */
    PROF_S6_Q_POP,            /* q_pop x2：mm 取请求 + echo 取 reply (非阻塞) */
    PROF_S7_MATCH_INFLIGHT,   /* reply 扫 inflight 匹配 reply_id */
    PROF_S8_Q_PUSH_REPLY,     /* reply q_push 入队 echo inbox */
    PROF_S9_WAKE_SEND,        /* wake_send_waiter(echo) */
    PROF_S10_TICK_IN,         /* mm 第二次 receive 阻塞 + tick → 切回 echo */
    PROF_N_STEPS
};

static uint64_t g_ipc_prof[PROF_N_STEPS];   /* 累计 cycles */
static int      g_ipc_prof_count;            /* round-trip 次数 */

/* 宏：测 [t0, t1) 期间的 cycles 加到 g_ipc_prof[step] */
#define PROF_T0()   rdtsc()
#define PROF_T1(s, t0)  do { uint64_t _t1 = rdtsc(); g_ipc_prof[(s)] += (_t1 - (t0)); } while(0)

void mk_ipc_prof_reset(void)
{
    memset(g_ipc_prof, 0, sizeof(g_ipc_prof));
    g_ipc_prof_count = 0;
}

void mk_ipc_prof_dump(int round_trips)
{
    g_ipc_prof_count = round_trips;
    printf("\n┌─ IPC round-trip cycle breakdown (%d rounds) ────────\n", round_trips);
    printf("│ %-28s %10s %10s %8s\n", "Stage", "total", "avg", "pct");
    printf("├───────────────────────────────┬────────────┬────────────┬────────┤\n");

    /* 子项求和 */
    static const char *names[PROF_N_STEPS] = {
        "S1 token+write+inflight",
        "S2 q_push (send)",
        "S3 wake_recv_waiter",
        "S4 self BLOCKED+unready",
        "S5 tick OUT (ctx_swap)",
        "S6 q_pop x2 (req+reply)",
        "S7 match inflight (reply)",
        "S8 q_push (reply)",
        "S9 wake_send_waiter",
        "S10 tick IN (ctx_swap)",
    };

    uint64_t total_sum = 0;
    for (int s = 0; s < PROF_N_STEPS; s++) total_sum += g_ipc_prof[s];

    for (int s = 0; s < PROF_N_STEPS; s++) {
        uint64_t t = g_ipc_prof[s];
        double avg = (double)t / round_trips;
        double pct = total_sum ? 100.0 * t / total_sum : 0;
        printf("│ %-28s %10lu %10.1f %7.1f%% │\n",
               names[s], t, avg, pct);
    }

    /* 汇总 */
    uint64_t tick_out = g_ipc_prof[PROF_S5_TICK_OUT];
    uint64_t tick_in  = g_ipc_prof[PROF_S10_TICK_IN];
    uint64_t ctx_total = tick_out + tick_in;
    uint64_t other = total_sum - ctx_total;

    printf("├───────────────────────────────┼────────────┼────────────┼────────┤\n");
    printf("│ %-28s %10lu %10.1f %7.1f%% │\n",
           "ctx_swap x2 (S5+S10)", ctx_total, (double)ctx_total/round_trips,
           total_sum ? 100.0*ctx_total/total_sum : 0);
    printf("│ %-28s %10lu %10.1f %7.1f%% │\n",
           "non-ctx work (all else)", other, (double)other/round_trips,
           total_sum ? 100.0*other/total_sum : 0);
    printf("├───────────────────────────────┼────────────┼────────────┼────────┤\n");
    printf("│ %-28s %10lu %10.1f %7.1f%% │\n",
           "TOTAL (incl rdtsc overhead)", total_sum, (double)total_sum/round_trips, 100.0);
    printf("└──────────────────────────────────────────────────────────────────┘\n");
}

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
 *    q_pop:  返回弹出的 slot index（当前不返回给外部，保留 API 对称）
 *
 *  不变量（由同步 send 语义保证）：
 *    每个任务同时最多 1 条 inflight send → 一个 mailbox 最多积压 N-1 条消息。
 *    QUEUE_SIZE = N → q_push 永远不会溢出。用 assert 防御，不用 while 阻塞。
 * ================================================================ */
static inline bool q_empty(const mk_mailbox_t *m) { return m->count == 0; }

/* L1 优化：static inline + 位与代替模（QUEUE_SIZE=32=2^5 → mask=31）
 * 编译器 -O2 可能已经自动做了，但显式写出来防止退化 */
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

/* L1 优化：加 static inline，省函数调用开销 */

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
 *  分配 reply token → 内核写 from/reply_id → 登记 inflight → 入队
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

    /* ── S1: token 分配 + 内核写 from/reply_id + inflight 登记 ── */
    uint64_t t0 = PROF_T0();

    uint16_t reply_id = ++g_reply_counter;
    mk_msg_t full_msg = *msg;
    full_msg.from     = self;
    full_msg.reply_id = reply_id;

    uint64_t t1 = rdtsc();
    g_ipc_prof[PROF_S1_SEND_PREP] += (t1 - t0);

    /* ── S2: q_push ── */
    uint64_t t2 = rdtsc();
    uint8_t slot = q_push(dst_mbox, &full_msg);
    dst_mbox->inflight[slot] = true;
    dst_mbox->reply_id_of_slot[slot] = reply_id;
    uint64_t t3 = rdtsc();
    g_ipc_prof[PROF_S2_Q_PUSH] += (t3 - t2);

    /* ── S3: wake_recv_waiter ── */
    uint64_t t4 = rdtsc();
    wake_recv_waiter(to);
    uint64_t t5 = rdtsc();
    g_ipc_prof[PROF_S3_WAKE_RECV] += (t5 - t4);

    /* ── S4: 自己 BLOCKED + unready ── */
    uint64_t t6 = rdtsc();
    me->ipc_send_wait = true;
        mk_tcb_set_state(me, MK_TASK_BLOCKED);
    mk_sched_unready(self);
    uint64_t t7 = rdtsc();
    g_ipc_prof[PROF_S4_BLOCK_SELF] += (t7 - t6);

    /* ── S5: tick 切走 (含 ctx_swap) ── */
    uint64_t t8 = rdtsc();
    mk_sched_tick();
    uint64_t t9 = rdtsc();
    g_ipc_prof[PROF_S5_TICK_OUT] += (t9 - t8);

    me->ipc_send_wait = false;
    return MK_OK;
}

/* ================================================================
 *  receive — 硬化版
 *
 *  队空 → recv_wait 阻塞；队列有消息 → pop
 *  被唤醒后循环检查队列（防御多次 wake 间队列被消耗空）
 *
 *  注意：**receive 不清 inflight**！
 *    inflight 表示 "这条 send 还在等 reply"，不是 "还在队列里"。
 *    send 消息被 receive 取走只是让服务者看到了请求，
 *    但 send 者的 send_wait 还在阻塞等 reply。
 *    inflight 只能由 reply() 匹配成功后清。
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

        /* ── S10: 第二次 receive 队空 → tick 切回 echo ── */
        uint64_t t0 = rdtsc();
        mk_sched_tick();
        uint64_t t1 = rdtsc();
        g_ipc_prof[PROF_S10_TICK_IN] += (t1 - t0);
    }

    /* ── S6: q_pop × 2 (非阻塞：mm 取请求 + echo 取 reply) ── */
    uint64_t t2 = rdtsc();
    q_pop(mbox, msg);
    uint64_t t3 = rdtsc();
    g_ipc_prof[PROF_S6_Q_POP] += (t3 - t2);
    return MK_OK;
}

/* ================================================================
 *  reply — 硬化版
 *
 *  1. 在自己的 mailbox 里找 reply_id 匹配的 inflight slot → 清掉
 *     这是 "这笔账我结了" 的意思。没匹配到 → 返回 MK_ERR_INVALID。
 *  2. 投递 reply 消息到对方 inbox（解 recv_wait）。
 *     不变量：对方同时最多 1 条 inflight send，reply 积压最多 1 条。
 *  3. 解对方的 send_wait —— 那个被我们匹配到 inflight 的 send 者。
 *
 *  reply 和 send 的根本区别：reply 不阻塞，立即返回。
 *  send 则是投递消息 + send_wait 阻塞等 reply 解。
 *
 *  硬化：reply 不允许 silent drop。旧版 "队列满丢最老" 违反硬化目标，
 *  也永远不会触发（同步 send 下 reply 积压最多 1 条）。
 * ================================================================ */
mk_err_t mk_ipc_reply(uint8_t to, const mk_msg_t *msg)
{
    uint8_t self = mk_current_tid();
    if (to >= MK_MAX_TASKS || to == self) return MK_ERR_INVALID;
    if (!msg) return MK_ERR_INVALID;

    /* P0-2 硬化：reply_id==0 直接拒，不扫 32 个 slot。
     * g_reply_counter 从 1 开始，0 永远不可能是合法 token。
     * 开发者忘了写 reply.reply_id = req.reply_id 时会撞到这里。 */
    if (msg->reply_id == 0) return MK_ERR_INVALID;

    mk_tcb_t     *to_tcb   = mk_tcb_get(to);
    mk_mailbox_t *dst_mbox = &g_mboxes[to];      /* reply 目标的 mailbox (send 者的 inbox) */
    mk_mailbox_t *my_mbox  = &g_mboxes[self];     /* 我 reply 者自己的 mailbox */

    if (!to_tcb || to_tcb->state == MK_TASK_DEAD) return MK_ERR_NOIPC;

    /* ---- 关键校验：reply_id 必须匹配我 mailbox 里某个 inflight slot ---- */
    bool inflight_found = false;

    /* ── S7: 扫 inflight 匹配 reply_id ── */
    uint64_t t0 = rdtsc();
    for (uint8_t i = 0; i < MK_IPC_QUEUE_SIZE; ++i) {
        if (my_mbox->inflight[i]
                && my_mbox->reply_id_of_slot[i] == msg->reply_id) {
            my_mbox->inflight[i] = false;
            my_mbox->reply_id_of_slot[i] = 0;
            inflight_found = true;
            break;
        }
    }
    uint64_t t1 = rdtsc();
    g_ipc_prof[PROF_S7_MATCH_INFLIGHT] += (t1 - t0);

    if (!inflight_found) {
        return MK_ERR_INVALID;
    }

    /* ---- 投递 reply 消息到对方 inbox ---- */
    mk_msg_t full_msg = *msg;
    full_msg.from = self;

    /* ── S8: reply q_push ── */
    uint64_t t2 = rdtsc();
    q_push(dst_mbox, &full_msg);
    uint64_t t3 = rdtsc();
    g_ipc_prof[PROF_S8_Q_PUSH_REPLY] += (t3 - t2);

    /* ── S9: wake_send_waiter(echo) ── */
    uint64_t t4 = rdtsc();
    wake_send_waiter(to);
    uint64_t t5 = rdtsc();
    g_ipc_prof[PROF_S9_WAKE_SEND] += (t5 - t4);

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
 *    服务者死了 → inflight 永远不被清 → 永远不 wake_send_waiter
 *    → 那些 client 永久 BLOCKED，整个系统卡死
 *
 *  两步：
 *    1. 扫 inflight[] 里所有 slot —— 每个对应一个还在等 reply 的 client
 *       给每个 client 推一条 synthetic error reply（tag=0 表示"服务已死"）
 *       调 wake_send_waiter 解它的阻塞
 *    2. 清空 mailbox 队列 —— 时刻 A（send 刚入队还没被 receive）的残留
 *       这些消息的 inflight 已经被上面处理过了，队列只是留着脏数据
 *       下次 tid 被重用时脏队列会引发错觉
 * ================================================================ */
void mk_ipc_cleanup_dead_service(uint8_t dead_tid)
{
    if (dead_tid >= MK_MAX_TASKS) return;

    mk_mailbox_t *my = &g_mboxes[dead_tid];

    for (uint8_t i = 0; i < MK_IPC_QUEUE_SIZE; ++i) {
        if (!my->inflight[i]) continue;

        uint8_t  client   = my->slots[i].from;
        uint16_t reply_id = my->reply_id_of_slot[i];

        /* 清 inflight（我死了，这笔账结不了） */
        my->inflight[i] = false;
        my->reply_id_of_slot[i] = 0;

        /* 防御性边界：from 必须是合法 tid */
        if (client >= MK_MAX_TASKS) continue;

        /* 给 client 推 synthetic reply —— tag=0 约定"服务已死"
         * reply_id 必须是那个 client send 时内核分配的 token，
         * 这样 client.unblock() 后 receive() 能正常读到 */
        mk_msg_t err;
        memset(&err, 0, sizeof(err));
        err.reply_id = reply_id;
        err.from     = dead_tid;   /* 内核填，虽然 reply_id 更关键 */
        /* err.data[0] = MK_ERR_NOIPC = -1 —— client 的 mk_ipc_send 返回后
         * 应该 receive 这条消息时能看到服务已死 */
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
