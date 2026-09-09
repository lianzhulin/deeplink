/*
 * main.c — 内核启动 & demo
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

/* ---- idler：tick 驱动 + 永不退出 ---- */
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

/* ---- clock_task：下半部，poll pending irq ---- */
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

/* ---- demo_task_a：sleep 验证时钟唤醒 ---- */
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

/* ---- demo_task_b：IPC + 内存管理 ---- */
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

/* ---- echo_task：IPC 往返测试 ---- */
static void echo_task(void *arg)
{
    (void)arg;
    printf("[echo] started\n");

    mk_msg_t msg, reply;
    msg.tag = MK_MM_TAG_QUERY;
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
