/*
 * mm.c — 内存管理，独立任务
 *
 * 一个静态 arena（1MB） + first-fit free list。
 * 所有分配/释放请求由 mm 任务通过 IPC 响应 —— 策略在任务里，机制在 arena 里。
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"
#include "task.h"
#include "ipc.h"
#include "mm.h"

/* ---- Arena ---- */
#define MK_MM_ARENA_SIZE  (1024 * 1024)   /* 1 MB */
static unsigned char g_arena[MK_MM_ARENA_SIZE] __attribute__((aligned(16)));

typedef struct mk_block {
    uint32_t            size;      /* 块数据区大小（不含本 block 头） */
    uint8_t             inuse;
    struct mk_block    *next;      /* 物理顺序（first-fit 方便） */
} mk_block_t;

#define MK_BLOCK_HEADER_SIZE  sizeof(mk_block_t)

/* ---- Arena 初始化（惰性，第一次用的时候初始化） ---- */
static bool g_arena_ready = false;

static void arena_init(void)
{
    if (g_arena_ready) return;
    mk_block_t *first = (mk_block_t *)g_arena;
    first->size = MK_MM_ARENA_SIZE - MK_BLOCK_HEADER_SIZE;
    first->inuse = 0;
    first->next = NULL;
    g_arena_ready = true;
}

static void *arena_alloc(size_t n)
{
    arena_init();
    /* 16 字节对齐 */
    n = (n + 15) & ~(size_t)15;
    if (n == 0) n = 16;

    mk_block_t *cur = (mk_block_t *)g_arena;
    while (cur) {
        if (!cur->inuse && cur->size >= n) {
            /* 如果剩下的空间还够放一个 header + 16 字节数据，就 split */
            if (cur->size >= n + MK_BLOCK_HEADER_SIZE + 16) {
                mk_block_t *split = (mk_block_t *)
                    ((unsigned char *)cur + MK_BLOCK_HEADER_SIZE + n);
                split->size   = cur->size - n - MK_BLOCK_HEADER_SIZE;
                split->inuse  = 0;
                split->next   = cur->next;
                cur->size     = n;
                cur->next     = split;
            }
            cur->inuse = 1;
            return (void *)((unsigned char *)cur + MK_BLOCK_HEADER_SIZE);
        }
        cur = cur->next;
    }
    return NULL;
}

static void arena_free(void *p)
{
    if (!p) return;
    mk_block_t *b = (mk_block_t *)((unsigned char *)p - MK_BLOCK_HEADER_SIZE);
    if ((unsigned char *)b < g_arena || (unsigned char *)b >= g_arena + MK_MM_ARENA_SIZE) {
        return;   /* 越界，忽略 */
    }
    b->inuse = 0;

    /* 向后合并 */
    if (b->next && !b->next->inuse) {
        b->size += MK_BLOCK_HEADER_SIZE + b->next->size;
        b->next = b->next->next;
    }
    /* 向前合并 —— 需要找前驱 */
    mk_block_t *cur = (mk_block_t *)g_arena;
    while (cur && cur->next != b) cur = cur->next;
    if (cur && !cur->inuse) {
        cur->size += MK_BLOCK_HEADER_SIZE + b->size;
        cur->next = b->next;
    }
}

static int32_t arena_total_used(void)
{
    arena_init();
    int32_t used = 0;
    mk_block_t *cur = (mk_block_t *)g_arena;
    while (cur) {
        if (cur->inuse) used += cur->size;
        cur = cur->next;
    }
    return used;
}

/* ---- mm 任务入口 ---- */
static void mm_task(void *arg)
{
    (void)arg;
    arena_init();

    printf("[mm] task started (tid=%u), arena=%u bytes\n",
           (unsigned)mk_current_tid(), MK_MM_ARENA_SIZE);

    mk_msg_t req, reply;
    while (1) {
        if (mk_ipc_receive(&req) != MK_OK) continue;

        switch (req.tag) {
            case MK_MM_TAG_ALLOC: {
                size_t sz = (size_t)req.data[0];
                void *p = arena_alloc(sz);
                reply.tag = MK_MM_TAG_ALLOC;
                /* 把 64-bit 指针拆成两个 int32 存 */
                uint64_t v = (uint64_t)(uintptr_t)p;
                reply.data[0] = (int32_t)(v & 0xFFFFFFFF);
                reply.data[1] = (int32_t)((v >> 32) & 0xFFFFFFFF);
                reply.data[2] = 0;
                mk_ipc_reply(req.from, &reply);
                break;
            }
            case MK_MM_TAG_FREE: {
                uint64_t v = (uint64_t)(uint32_t)req.data[0] |
                             ((uint64_t)(uint32_t)req.data[1] << 32);
                void *p = (void *)(uintptr_t)v;
                arena_free(p);
                break;
            }
            case MK_MM_TAG_QUERY: {
                reply.tag = MK_MM_TAG_QUERY;
                reply.data[0] = MK_MM_ARENA_SIZE;
                reply.data[1] = arena_total_used();
                reply.data[2] = MK_MM_ARENA_SIZE - reply.data[1];
                mk_ipc_reply(req.from, &reply);
                break;
            }
            default:
                /* 未知 tag，丢弃 */
                break;
        }
    }
}

void mk_mm_start(void)
{
    int tid = mk_task_create("mm", mm_task, NULL, 0, 0);
    if (tid < 0) {
        fprintf(stderr, "[mm] failed to create mm task: %d\n", tid);
    }
}

/* ---- 便捷封装 ---- */
void *mk_alloc(size_t size)
{
    if (mk_current_tid() == MK_TID_MM) {
        /* 自己直接调 arena，不走 IPC */
        return arena_alloc(size);
    }
    mk_msg_t req, reply;
    req.tag = MK_MM_TAG_ALLOC;
    req.data[0] = (int32_t)size;
    mk_ipc_send(MK_TID_MM, &req);
    /* send 不阻塞，receive 会阻塞等 reply */
    if (mk_ipc_receive(&reply) != MK_OK) return NULL;
    uint64_t v = (uint64_t)(uint32_t)reply.data[0] |
                 ((uint64_t)(uint32_t)reply.data[1] << 32);
    return (void *)(uintptr_t)v;
}

void mk_free(void *ptr)
{
    if (mk_current_tid() == MK_TID_MM) {
        arena_free(ptr);
        return;
    }
    mk_msg_t req;
    req.tag = MK_MM_TAG_FREE;
    uint64_t v = (uint64_t)(uintptr_t)ptr;
    req.data[0] = (int32_t)(v & 0xFFFFFFFF);
    req.data[1] = (int32_t)((v >> 32) & 0xFFFFFFFF);
    mk_ipc_send(MK_TID_MM, &req);
    /* free 不需要 reply（当前设计里 reply 只是给 alloc 用） */
    (void)mk_ipc_poll;
}

mk_err_t mk_mm_query(int32_t out_buf[3])
{
    mk_msg_t req, reply;
    req.tag = MK_MM_TAG_QUERY;
    mk_ipc_send(MK_TID_MM, &req);
    if (mk_ipc_receive(&reply) != MK_OK) return MK_ERR_NOIPC;
    out_buf[0] = reply.data[0];
    out_buf[1] = reply.data[1];
    out_buf[2] = reply.data[2];
    return MK_OK;
}
