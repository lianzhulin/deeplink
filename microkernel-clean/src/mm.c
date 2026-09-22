
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"
#include "task.h"
#include "ipc.h"
#include "mm.h"

#define MK_MM_ARENA_SIZE  (1024 * 1024)   
static unsigned char g_arena[MK_MM_ARENA_SIZE] __attribute__((aligned(16)));

typedef struct mk_block {
    uint32_t            size;      
    uint8_t             inuse;
    struct mk_block    *next;      
} mk_block_t;

#define MK_BLOCK_HEADER_SIZE  sizeof(mk_block_t)

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
    
    n = (n + 15) & ~(size_t)15;
    if (n == 0) n = 16;

    mk_block_t *cur = (mk_block_t *)g_arena;
    while (cur) {
        if (!cur->inuse && cur->size >= n) {
            
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

    
    if (((uintptr_t)p & 0xF) != 0) {
        fprintf(stderr, "[mm] arena_free: pointer %p not 16-byte aligned, rejected\n", p);
        return;
    }

    mk_block_t *b = (mk_block_t *)((unsigned char *)p - MK_BLOCK_HEADER_SIZE);

    if ((unsigned char *)b < g_arena || (unsigned char *)b >= g_arena + MK_MM_ARENA_SIZE) {
        fprintf(stderr, "[mm] arena_free: pointer %p out of arena, rejected\n", p);
        return;   
    }

    
    if (!b->inuse) {
        fprintf(stderr, "[mm] arena_free: double-free at %p (block already free)\n", p);
        return;
    }

    b->inuse = 0;

    
    if (b->next && !b->next->inuse) {
        b->size += MK_BLOCK_HEADER_SIZE + b->next->size;
        b->next = b->next->next;
    }
    
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

void mm_task(void *arg)
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
                  
                  uint64_t v = (uint64_t)(uintptr_t)p;
                  reply.data[0] = (int32_t)(v & 0xFFFFFFFF);
                  reply.data[1] = (int32_t)((v >> 32) & 0xFFFFFFFF);
                  reply.data[2] = 0;
                  reply.reply_id = req.reply_id;
                  mk_ipc_reply(req.from, &reply);
                  break;
              }
              case MK_MM_TAG_FREE: {
                  uint64_t v = (uint64_t)(uint32_t)req.data[0] |
                               ((uint64_t)(uint32_t)req.data[1] << 32);
                  void *p = (void *)(uintptr_t)v;
                  arena_free(p);
                  
                  reply.tag = MK_MM_TAG_FREE;
                  reply.data[0] = 0;
                  reply.reply_id = req.reply_id;
                  mk_ipc_reply(req.from, &reply);
                  break;
              }
              case MK_MM_TAG_QUERY: {
                  reply.tag = MK_MM_TAG_QUERY;
                  reply.data[0] = MK_MM_ARENA_SIZE;
                  reply.data[1] = arena_total_used();
                  reply.data[2] = MK_MM_ARENA_SIZE - reply.data[1];
                  reply.reply_id = req.reply_id;
                  mk_ipc_reply(req.from, &reply);
                  break;
              }
              default:
                  
                  reply.tag = req.tag;
                  reply.data[0] = -1;
                  reply.reply_id = req.reply_id;
                  mk_ipc_reply(req.from, &reply);
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

void *mk_alloc(size_t size)
{
    if (mk_current_tid() == MK_TID_MM) {
        
        return arena_alloc(size);
    }
    mk_msg_t req, reply;
    req.tag = MK_MM_TAG_ALLOC;
    req.data[0] = (int32_t)size;
    mk_ipc_send(MK_TID_MM, &req);
    
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
    mk_msg_t req, dummy;
    req.tag = MK_MM_TAG_FREE;
    uint64_t v = (uint64_t)(uintptr_t)ptr;
    req.data[0] = (int32_t)(v & 0xFFFFFFFF);
    req.data[1] = (int32_t)((v >> 32) & 0xFFFFFFFF);
    
    mk_ipc_send(MK_TID_MM, &req);
    mk_ipc_receive(&dummy);
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
