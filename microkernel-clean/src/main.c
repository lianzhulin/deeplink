/*
 * ============================================================================
 *   main.c — 微内核原型的启动入口 + 全部 demo 任务定义
 * ============================================================================
 *
 *   【实现要点】mk_kernel_start 按顺序初始化各子系统 (task → irq → ipc)，
 *              然后创建 6 个演示任务：idler / clock / mm / demo_a / demo_b / echo，
 *              最后调 mk_sched_run 启动调度器永不返回。main() 只是壳，
 *              真正内核逻辑全在 mk_kernel_start 里。
 *   【硬化实现】setvbuf 关掉 stdout/stderr 缓冲，保证各任务 printf 不互相干扰；
 *              启动顺序严格：子系统 init → 任务 create → scheduler run，
 *              中间任何一步失败会显式打印 (mm 创建失败 fprintf)。
 *   【热路径】idler_task 是主循环，每 gettimeofday 1ms 模拟一次硬件时钟 tick，
 *              调 mk_irq_simulate + mk_ticks++ + yield —— 这是调度器驱动源。
 *              clock_task poll 被置位的 IRQ_CLOCK 位，触发 mk_clock_tick_process
 *              扫描 SLEEPING 任务唤醒。echo_task 做 5000 次 IPC 往返 benchmark。
 *   【错误处理】全部任务都是 while(1) 循环，异常不会自杀（除了 demo_a/b/echo
 *              显式 mk_task_exit）；idler 和 clock 永不退出保证系统不会调度空。

 * ============================================================================
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>

#include "kernel.h"
#include "task.h"
#include "irq.h"
#include "ipc.h"
#include "mm.h"

/* ---- idler（tid=0）：每 1ms 模拟一次硬件时钟中断，驱动整个调度器的时间推进
 * 存在原因：宿主机没有真正的硬件中断控制器，我们用 gettimeofday 检测
 * 时间差 ≥ 1ms 时主动置位 IRQ_CLOCK + 递增 mk_ticks，相当于
 * "时钟中断上半部"，然后 yield 让出 CPU 让调度器循环下一个任务。
 * 永不退出 → 调度器永远有一个 READY 任务，不会 mask 为空崩。 */
static void idler_task(void *arg)
{
    (void)arg;
    struct timeval last = {0};

    while (1) {
        struct timeval now;
        gettimeofday(&now, NULL);
        if (last.tv_sec == 0) last = now;

        long diff_us = (now.tv_sec - last.tv_sec) * 1000000L
                     + (now.tv_usec - last.tv_usec);
        if (diff_us >= 1000L) {
            mk_irq_simulate(MK_IRQ_CLOCK);
            mk_ticks++;
            last = now;
        }
        mk_task_yield();
    }
}

/* ---- clock_task（tid=1）：中断下半部，poll pending irq + 唤醒 sleeping 任务
 * 存在原因：idler 置位了 mk_global_irq_pending.MK_IRQ_CLOCK 只是一个 bit，
 * 真正要做的"扫描所有 TCB、把 wake_tick <= mk_ticks 的 SLEEPING 任务改成 READY"
 * 在这里完成。这就是"上半部置位、下半部处理"的分工。 */
extern void mk_clock_tick_process(void);

static void clock_task(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t pending = mk_irq_poll_clear(MK_IRQ_CLOCK);
        if (pending & MK_IRQ_CLOCK) {
            mk_clock_tick_process();
        }
        mk_task_yield();
    }
}

/* ---- demo_task_a（tid=3，name="demo_a"）：sleep API 的功能验证
 * 做什么：start → 连续 5 次 printf "beat" + mk_task_sleep(30)
 *       → 每次睡 30 tick（≈30ms）被 clock_task 唤醒 → 最后 exit。
 * 存在原因：证明 mk_task_sleep 能正常把自己从 READY → SLEEPING，
 *       clock_task 扫描 wake_tick 时能正确把它从 SLEEPING 拉回 READY，
 *       退出后 mk_task_exit 的 cleanup + canary + 状态机转换都是通的。 */
static void demo_task_a(void *arg)
{
    const char *name = (const char *)arg;
    printf("[%s] started\n", name);

    for (int i = 0; i < 5; ++i) {
        printf("[%s] beat #%d  tick=%lu\n",
               name, i, (unsigned long)mk_ticks);
        mk_task_sleep(30);   /* 30 ms 后再醒 */
    }

    printf("[%s] done, exiting\n", name);
    mk_task_exit();
}

/* ---- demo_task_b（tid=4，name="demo_b"）：IPC + 内存管理两条链路的集成测试
 * 做什么：mk_mm_query → 分配 64/256/1024/4096 4 块 arena 内存 → memset 写脏
 *       → 再 query 观察 used 计数 → free 中间两块 → query → free 剩余 → query。
 * 存在原因：证明 mk_alloc / mk_free 通过 IPC 正常工作，mm 任务的
 *       receive/switch/reply 服务循环正常，arena 的 split / merge /
 *       指针传输（拆成 data[0..1] 两 32 位）都没 bug；mk_mm_query 的
 *       reply 里 data[0..2] 填 total/used/free 也正常。 */
static void demo_task_b(void *arg)
{
    const char *name = (const char *)arg;
    printf("[%s] started\n", name);

    int32_t st[3];
    mk_mm_query(st);
    printf("[%s] before: total=%d used=%d\n", name, st[0], st[1]);

    const size_t sizes[] = {64, 256, 1024, 4096};
    void *ptrs[4] = {0};
    for (int i = 0; i < 4; ++i) {
        ptrs[i] = mk_alloc(sizes[i]);
        printf("[%s] alloc(%4zu) = %p\n", name, sizes[i], ptrs[i]);
        if (ptrs[i]) memset(ptrs[i], 0xAB, sizes[i]);
    }

    mk_mm_query(st);
    printf("[%s] after : used=%d\n", name, st[1]);

    mk_free(ptrs[1]);
    mk_free(ptrs[2]);
    mk_mm_query(st);
    printf("[%s] mid   : used=%d\n", name, st[1]);

    mk_free(ptrs[0]);
    mk_free(ptrs[3]);
    mk_mm_query(st);
    printf("[%s] freed : used=%d (expect 0)\n", name, st[1]);

    printf("[%s] done, exiting\n", name);
    mk_task_exit();
}

/* ---- echo_task（tid=5，name="echo"）：IPC 往返性能 benchmark
 * 做什么：warmup 100 次 send+receive → 正式 5000 次循环
 *       (tag=MK_MM_TAG_QUERY，mm 收到后 reply) → clock_gettime 计时
 *       → 输出 ns/round-trip + 估算 @3GHz cycles。最后再 send/receive 一次
 *       打一下 mm 的 reply 内容。
 * 存在原因：作为整个内核的"烟测 + 基准值"——
 *       5000 round trips 覆盖 send 分配 reply token → 入队 → BLOCKED
 *       → 调度切到 mm → mm receive → switch QUERY → reply → 解 send_wait
 *       → 调度切回 echo → echo receive 这条完整路径。
 *       输出格式固定，方便 /workspace/microkernel 与 /workspace/microkernel-clean
 *       跑出来对比行为一致。 */
static void echo_task(void *arg)
{
    (void)arg;
    printf("[echo] IPC bench (5000 round trips)...\n");

    mk_msg_t msg, reply;
    msg.tag = MK_MM_TAG_QUERY;

    /* warmup 100 次 */
    for (int i = 0; i < 100; ++i) {
        mk_ipc_send(MK_TID_MM, &msg);
        mk_ipc_receive(&reply);
    }

    /* 重置两个 profiler，正式计时 */

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        mk_ipc_send(MK_TID_MM, &msg);
        mk_ipc_receive(&reply);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* dump 两个 breakdown */
    uint64_t total_ns  = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL
                       + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    uint64_t avg_ns    = total_ns / N;
    double   cycles    = (double)avg_ns * 3.0;

    printf("[echo] IPC bench result: %lu ns/round-trip (~%.0f cycles @3GHz)\n",
           avg_ns, cycles);
    printf("[echo] total: %lu ns for %d rounds\n", total_ns, N);

    /* 一次正常 query + 退出 */
    mk_ipc_send(MK_TID_MM, &msg);
    mk_ipc_receive(&reply);
    printf("[echo] mm reply: tag=0x%04X total=%d\n", reply.tag, reply.data[0]);
    printf("[echo] done, exiting\n");
    mk_task_exit();
}

void mk_kernel_start(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\n========== microkernel prototype booting ==========\n\n");

    mk_task_init();
    mk_irq_init();
    mk_ipc_init();

    mk_task_create("idler", idler_task, NULL, 0, 0);
    mk_task_create("clock", clock_task, NULL, 0, 0);
    mk_mm_start();
    mk_task_create("demo_a", demo_task_a, (void*)"demo_a", 0, 0);
    mk_task_create("demo_b", demo_task_b, (void*)"demo_b", 0, 0);
    mk_task_create("echo",   echo_task,   NULL,       0, 0);

    printf("6 tasks created, entering scheduler...\n\n");
    fflush(stdout);

    mk_sched_run();
}

int main(void)
{
    mk_kernel_start();
    return 0;
}
