# Microkernel — 极简硬化微内核核心（x86-64）

一个教学型微内核原型，**纯 C + 71 行汇编**，剔除一切非核心代码后 **819 行**。
硬化目标：TCB canary 防野指针、状态机白名单防非法转换、reply-token IPC 防 silent message loss、上下文完整性校验。

> GitHub: https://github.com/lianzhulin/microkernel

---

## 指标

| 项 | 值 |
|---|---|
| 总代码 | **819 行**（10 个文件） |
| C 源码 (.c) | 564 行 |
| 头文件 (.h) | 204 行 |
| 汇编 (.S) | 71 行 |
| 上下文原语 | 3 条（56B 轻量 ctx，glibc ucontext_t 的 1/17） |
| IPC round-trip | ~180 ns（echo benchmark，5000 次均值） |
| ASAN/UBSAN | **零内存错误** |

---

## 架构

```
┌─────────────────────────────────────────────────────────┐
│                    用户任务（例：idler）                   │
│  mk_task_yield()   mk_task_sleep()   mk_ipc_send()       │
└─────────────────────┬───────────────────────────────────┘
                      │ 调度点 / IPC 原语
┌─────────────────────▼───────────────────────────────────┐
│                   内核 API 层                             │
│  ┌─────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐    │
│  │  task   │  │   ipc    │  │   irq    │  │  kernel │    │
│  │ 调度器  │  │ send/recv│  │ 上半部置位│  │ 常量类型│    │
│  └────┬────┘  └────┬─────┘  └────┬─────┘  └─────────┘    │
│       │            │             │                       │
│  ┌────▼────────────▼─────────────▼───────────────────┐    │
│  │              静态内核对象（单线程串行）               │    │
│  │  g_tasks[32]  g_ready_mask  g_mboxes[32]  mk_ticks│    │
│  └──────────────────────────┬────────────────────────┘    │
│                             │ ctx_swap                    │
│  ┌──────────────────────────▼────────────────────────┐    │
│  │        ctx.S — 轻量上下文原语（x86-64 SysV ABI）     │    │
│  │  mk_ctx_swap / mk_ctx_load / mk_ctx_init            │    │
│  └────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

---

## 目录结构

```
microkernel/
├── Makefile               # 编译成静态库 libmicrokernel.a
│
├── include/               # 公共头文件（5 个）
│   ├── kernel.h           # 常量 + 类型 + 错误码（"宪法"）
│   ├── task.h             # TCB + 调度器 API + ctx 声明
│   ├── ipc.h              # IPC 三原语 + msg / mailbox
│   └── irq.h              # 中断上半部置位 + poll
│
└── src/                   # 实现文件（4 个）
    ├── task.c             # 调度器 / TCB 硬化 / ctx_integrity (315)
    ├── ipc.c              # send / receive / reply / cleanup_dead_service (195)
    ├── irq.c              # 时钟 tick 处理 + sleep 唤醒 (54)
    └── ctx.S              # mk_ctx_swap / load / init (71)
```

---

## 模块详解

### 模块 1：kernel.h — 宪法文件

定义所有模块共享的常量、类型、错误码。**被 task/ipc/mm/irq 全部 include，本身不依赖任何内核模块**。

#### 常量

| 宏 | 值 | 用途 |
|---|---|---|
| `MK_MAX_TASKS` | 32 | 硬约束：最多 32 个任务 |
| `MK_TID_BITS` | 5 | 2^5 = 32，bitmask 调度器需要 |
| `MK_TID_MASK` | 31 (= 0x1F) | tid 掩码 |
| `MK_TID_IDLER` | 0 | 空闲任务，**永远在 ready mask 里** |
| `MK_TID_CLOCK` | 1 | 时钟下半部任务 |
| `MK_TID_MM` | 2 | （原版 mm 服务的约定 tid，clean 内核不包含） |
| `MK_TID_FIRST_USER` | 3 | 第一个用户任务起始 |
| `MK_DEFAULT_STACK_SIZE` | 65536 | 64 KB |
| `MK_IRQ_CLOCK` | bit 0 | 时钟中断位 |
| `MK_IRQ_IOBASE` | bit 1 | 预留设备中断位 |

#### 枚举

**`mk_task_state_t`** — 5 态：
```c
MK_TASK_DEAD     = 0   // 未分配或已自杀
MK_TASK_READY    = 1   // 在 ready mask 里，等调度
MK_TASK_RUNNING  = 2   // 正在跑（g_current）
MK_TASK_BLOCKED  = 3   // 等 IPC / 等事件（只被 IPC 设置）
MK_TASK_SLEEPING = 4   // 等时钟 tick（只被 mk_task_sleep 设置）
```

**`mk_err_t`** — 7 个错误码（< 0 为失败）：
```c
MK_OK         =  0
MK_ERR_INVALID= -1    // 参数非法
MK_ERR_NOMEM  = -2    // 栈分配失败 / tid 槽满
MK_ERR_NOIPC  = -3    // 对方已死
MK_ERR_TIMEOUT= -4
MK_ERR_BUSY   = -5
MK_ERR_EXIST  = -6
```

#### 全局变量

```c
extern volatile uint64_t mk_ticks;   // 时钟 tick 计数
```

---

### 模块 2：task.h / task.c — 调度器 + TCB 硬化

内核的**心脏模块**。负责创建任务、调度切换、状态管理、上下文完整性校验。

#### 核心结构体

**`mk_light_ctx_t`** — L2 轻量上下文，**56 字节**：
```c
typedef struct {
    uint64_t rbx, rbp, r12, r13, r14, r15;   // SysV ABI callee-saved 寄存器
    uint64_t rsp;                              // 栈指针
} mk_light_ctx_t;
```
对比 glibc `ucontext_t` 的 **968 字节** —— 省掉了 FPU/SSE 状态、signal mask、uc_link/uc_stack 等内核根本不关心的字段。

**`mk_tcb_t`** — 任务控制块，前后 canary 保护：
```c
typedef struct mk_tcb {
    uint64_t           canary_head;        // 魔数 0xCAFEBABEDEADBEEF
    uint8_t            tid;                // 0~31
    uint8_t            prio;               // 预留优先级
    volatile mk_task_state_t state;
    char               name[16];
    mk_light_ctx_t     ctx;                // 56B 轻量上下文
    void              *stack;              // posix_memalign 分配的栈
    size_t             stack_size;         // 栈大小
    uint64_t           wake_tick;          // SLEEPING 时的唤醒 tick
    volatile bool      ipc_send_wait;      // send 阻塞等 reply
    volatile bool      ipc_recv_wait;      // receive 阻塞等消息
    volatile uint32_t  irq_pending;
    uint64_t           canary_tail;        // 魔数 0x0BADF00DCAFEFACE
} mk_tcb_t;
```

#### 硬化设计

1. **双 canary 防野指针**：TCB 前后各放一个 8B 魔数，任何方向的栈溢出/野指针覆盖都会破坏其中一个，`tcb_check_canary()` 立即 `abort()`。
2. **状态机白名单**：`g_valid_trans[5][5]` 二维查表，`mk_tcb_set_state()` 所有 state 写点必走，非法转换直接 abort。DEAD 是 escape hatch（任何状态都能转 DEAD）。
3. **ctx 完整性**：`tcb_check_ctx_integrity()` 校验 `ctx.rsp ∈ [stack, stack + stack_size)`，每次 `mk_sched_tick` 切换前 + `mk_sched_run` 首次启动前都会调。
4. **`mk_tcb_set_state` 单点入口**：整个内核 **所有** state 写点都走它 → canary/状态机覆盖无遗漏。

#### 调度算法

**bitmask 就绪集** `g_ready_mask`（32-bit uint32_t）：
- 入队：`g_ready_mask |= (1u << tid)` — 一条位指令
- 出队：`g_ready_mask &= ~(1u << tid)` — 一条位指令
- 选下一个：`__builtin_ctz()` — x86 上是 tzcnt/bsf 硬件指令

**`pick_next()`** — 从 g_current+1 起找下一个 READY（循环调度）：
```c
if (cur + 1 >= 32) return ctz(mask);          // cur==31 wrap
uint32_t after = mask & ~((1u << (cur+1)) - 1);
if (after) return ctz(after);
return ctz(mask);                             // 后半段空，wrap 到最低位
```

**不变量**：`tid=0 idler 永远在 mask 里` → mask 永不为 0，`pick_next` 永不返回无效值。

#### 公开 API

| 函数 | 签名 | 用途 |
|---|---|---|
| `mk_task_init` | `void(void)` | 静态分配 32 个 TCB，初始化 canary/state，清全局 |
| `mk_task_create` | `int(name, entry, arg, stack_size, prio)` | 找 DEAD 槽 → posix_memalign 栈 → `mk_ctx_init` → `mk_sched_ready`。成功返回 tid (>=0)，失败返回负 `mk_err_t` |
| `mk_task_exit` | `void(void)` `noreturn` | **先** `mk_ipc_cleanup_dead_service`（给所有等待的 client 推 error reply 解阻塞）→ 清 entry/arg → 从 ready mask 移除 → `mk_sched_tick()` |
| `mk_task_yield` | `void(void)` | 主动让出 CPU = `mk_sched_tick()` |
| `mk_task_sleep` | `void(uint64_t ticks)` | 设 `wake_tick = mk_ticks + ticks` → `RUNNING→SLEEPING` → 从 mask 移除 → tick |
| `mk_current_tid` | `uint8_t(void)` | 返回 g_current |
| `mk_current` | `mk_tcb_t*(void)` | 返回 `&g_tasks[g_current]` |
| `mk_tcb_get` | `mk_tcb_t*(uint8_t tid)` | tid>=32 返回 NULL |
| `mk_tcb_set_state` | `void(tcb, new_state)` | **唯一 state 写入口**：canary 校验 + 状态机查表 + 写入 |
| `mk_ctx_init` | `void(ctx, entry, stack, size)` | 清零 callee-saved 寄存器，ret addr 压栈到 0 mod 16 位置 |
| `mk_ctx_swap` | `void(old, new)` | 汇编原语：保存当前到 old，从 new 恢复并跳转 |
| `mk_ctx_load` | `void(ctx)` | 汇编原语：只恢复不保存（= setcontext） |
| `mk_sched_ready` | `void(uint8_t tid)` | `g_ready_mask |= (1<<tid)` + `state != RUNNING` 时 → READY |
| `mk_sched_unready` | `void(uint8_t tid)` | `g_ready_mask &= ~(1<<tid)` |
| `mk_sched_tick` | `void(void)` | 调度热路径：idler canary → pick_next → prev state → next state(RUNNING) → ctx_integrity → ctx_swap |
| `mk_sched_run` | `void(void)` `noreturn` | 启动调度器：找 mask 最低位 → RUNNING → canary+ctx_integrity → `mk_ctx_load` 跳进去 |

---

### 模块 3：ipc.h / ipc.c — send / receive / reply 硬化版

微内核的**消息传递子系统**。每个任务一个 mailbox 环形队列（32 槽 = MAX_TASKS）。

#### 核心结构体

**`mk_msg_t`** — 24 字节消息体（微内核哲学：IPC 传小消息，大消息走页共享）：
```c
typedef struct mk_msg {
    uint16_t  tag;           // 用户路由
    uint16_t  reply_id;      // 内核生成的 reply token（reply 必须带匹配 token 才能解）
    uint8_t   from;          // 内核覆盖；用户写的作废
    uint8_t   flags;
    int32_t   data[4];       // 16 字节 payload
} mk_msg_t;
```

**`mk_mailbox_t`** — 每任务一个：
```c
typedef struct {
    mk_msg_t  slots[32];                      // 环形队列槽
    uint8_t   head, tail, count;              // 队列指针
    uint16_t  reply_id_of_slot[32];          // 0=未 inflight，>0=reply token
} mk_mailbox_t;
```

#### 硬化设计

1. **reply_id per slot token 机制**：send 时内核分配唯一 token（`g_reply_counter` 从 1 递增，wrap 后强制回 1，0 是"未 inflight"哨兵），reply 必须带匹配 token。没匹配到 → `MK_ERR_INVALID`（**拒绝 silent drop**）。
2. **两种独立阻塞原因**：`ipc_send_wait` / `ipc_recv_wait`，精确隔离。旧版一个 bool 互相污染，reply 可能误解 recv 阻塞的任务。每种阻塞只被对应的原语解：reply → send_wait，send/reply 投递 → recv_wait。
3. **inflight 合并**：`reply_id_of_slot[i] == 0` 表示 slot 没 inflight，`>0` 就是 token。省掉 `bool inflight[32]` 数组，判断条件更简洁。
4. **服务者死亡清理**：`mk_ipc_cleanup_dead_service` 扫自己 mailbox 所有 inflight slot，给每个 client 推一条 synthetic error reply（`tag=0, data[0]=MK_ERR_NOIPC`）+ `wake_send_waiter`。**必须在 `mk_task_exit` 里调**，否则 client 永久 BLOCKED。
5. **队列永不溢出**：同步 send → 每任务同时最多 1 条 inflight；Q=32 ≥ N-1=31 积压 → 不变量自洽。

#### 核心辅助函数

| 函数 | 用途 |
|---|---|
| `q_empty` | 静态 inline，检查 m->count == 0 |
| `q_push` | 静态 inline，写入 tail 位置，tail 位与前进，count++ |
| `q_pop` | 静态 inline，从 head 弹出，head 位与前进，count-- |
| `wake_send_waiter` | 解"send 阻塞等 reply" —— 只被 reply() 调 |
| `wake_recv_waiter` | 解"receive 阻塞等消息" —— 只被 send() 或 reply() 投递消息时调 |

#### 公开 API

| 函数 | 签名 | 用途 |
|---|---|---|
| `mk_ipc_init` | `void(void)` | 清零所有 mailbox，g_reply_counter=0 |
| `mk_ipc_send` | `mk_err_t(to, msg)` | 校验边界 → 分配 reply token → 内核写 from/reply_id → **入队 + 登记 inflight** → wake_recv_waiter(to) → **自己 send_wait 阻塞等 reply 解** |
| `mk_ipc_receive` | `mk_err_t(msg)` | 队空 → recv_wait 阻塞循环；队不空 → q_pop |
| `mk_ipc_reply` | `mk_err_t(to, msg)` | reply_id==0 直接拒 → **在自己 mailbox 扫匹配 token** → 清 reply_id_of_slot=0 → 投递到对方 inbox → wake_send_waiter(to)。**不阻塞，立即返回** |
| `mk_ipc_poll` | `bool(void)` | 非阻塞探测：自己 mailbox 队是否空 |
| `mk_ipc_cleanup_dead_service` | `void(dead_tid)` | 服务者死亡时：扫 inflight slot 推 synthetic error reply + wake 阻塞 client → 清空残留队列 |

#### reply_id 匹配流程图

```
reply() 匹配自己 mailbox 里的 slot:
┌──────────────────────────────────────┐
│ for i in [0, 32):                    │
│   if reply_id_of_slot[i] == token:   │   ← reply_id_of_slot[i]==0 天然跳
│     reply_id_of_slot[i] = 0          │
│     wake_send_waiter(client_of_slot) │
│     push synthetic reply → client    │
│     break                            │
│                                       │
│ if 没找到 → MK_ERR_INVALID            │
└──────────────────────────────────────┘
```

---

### 模块 4：irq.h / irq.c — 中断上半部置位

设计原则：**上半部只有几行 C**，不做任何处理逻辑，不投递 IPC，不唤醒任务。

#### 上半部本体

```c
static inline void mk_irq_trap_top(uint32_t irq_bit) {
    extern volatile uint32_t mk_global_irq_pending;
    mk_global_irq_pending |= irq_bit;
}
```

真实硬件上这就是汇编 ISR 入口里调的那一个 C 函数。中断响应时间 = 置位 pending 这一条 OR 指令。

#### 下半部处理

- **时钟 tick 处理**（`mk_clock_tick_process`）：扫描所有 SLEEPING 任务，`wake_tick <= mk_ticks` 的 → `mk_sched_ready`。
- **poll 模式**：任务自己 poll `irq_poll_clear(MK_IRQ_CLOCK)`，不是中断主动投递。

#### 公开 API

| 函数 | 签名 | 用途 |
|---|---|---|
| `mk_irq_simulate` | `void(uint32_t)` | 宿主机模拟硬件中断（测试用） |
| `mk_irq_trap_top` | `static inline void(uint32_t)` | 上半部置位 pending |
| `mk_irq_poll_clear` | `uint32_t(mask)` | 下半部任务 poll：返回并清 mask 命中的位 |
| `mk_irq_init` | `void(void)` | 清 pending，清重入计数 |
| `mk_clock_tick_process` | `void(void)` | 扫 SLEEPING → 唤醒（被 poll_clear 后调用） |

---

### 模块 5：ctx.S — 轻量上下文原语（x86-64）

SysV ABI：rdi=第1参、rsi=第2参、rdx=第3参、rcx=第4参。

| 函数 | 签名 | 用途 |
|---|---|---|
| `mk_ctx_init` | `void(ctx, entry, stack, size)` | 清零 6 callee-saved 寄存器 → stack_top 16B 对齐 → `rsp-8` 位置存 entry 地址 → ctx.rsp 指向该位置。ret 后 rsp ≡ 8 mod 16 ✓ |
| `mk_ctx_swap` | `void(old, new)` | 保存当前 callee-saved + rsp 到 old → 从 new 恢复 → ret 跳转（= swapcontext） |
| `mk_ctx_load` | `void(ctx)` | 只恢复不保存（= setcontext），用于调度器首次启动 |

#### mk_ctx_swap 寄存器保存清单（严格匹配 mk_light_ctx_t 偏移）

```
movq  %rbx,    0(%rdi)      ┐
movq  %rbp,    8(%rdi)      │
movq  %r12,   16(%rdi)      │ old_ctx 保存
movq  %r13,   24(%rdi)      │
movq  %r14,   32(%rdi)      │
movq  %r15,   40(%rdi)      │
movq  %rsp,   48(%rdi)      ┘
movq   0(%rsi), %rbx        ┐
...                         │ new_ctx 恢复
movq  48(%rsi), %rsp        ┘ ← 此时栈顶是目标 return address
ret                              ← 弹出跳转
```

---

## 硬化设计总览

```
                    ┌──────────────────────────────────┐
                    │         mk_tcb_set_state         │
                    │  canary + 5x5 状态机查表 + 写入    │
                    │  所有 state 写点 → 唯一入口         │
                    └──────────────────────────────────┘
                                     │
        ┌────────────────────────────┼────────────────────────────┐
        │                            │                            │
   DEAD escape hatch            valid_trans 5x5 白名单        canary 双魔数
        │                            │                            │
  任何 state → DEAD          防非法转换                     野指针/栈溢出
                                                                立即 abort

   ctx 完整性检查（mk_sched_tick 每切换 + mk_sched_run 首次启动）
   ┌────────────────────────────────────────────────────┐
   │  tcb_check_ctx_integrity:                           │
   │    ctx.rsp ∈ [stack, stack + stack_size) ?          │
   │    不在 → abort（抓到野栈、use-after-free）          │
   └────────────────────────────────────────────────────┘

   IPC reply-token（防 silent message loss）
   ┌────────────────────────────────────────────────────┐
   │  send: 分配唯一 token → reply_id_of_slot[slot]=tok │
   │  reply: 必须携带匹配 token → 扫 32 slot 匹配       │
   │  reply_id==0 → 直接拒（哨兵值）                     │
   │  服务者死亡 → cleanup 推 synthetic error reply      │
   └────────────────────────────────────────────────────┘

   两种独立阻塞（精确隔离，零互相污染）
   ┌────────────────────────────────────────────────────┐
   │  ipc_send_wait ← 只有 reply 能解                     │
   │  ipc_recv_wait ← send/reply 投递消息能解            │
   │  旧版一个 bool 互相污染 → 现在两个 bool 精确匹配      │
   └────────────────────────────────────────────────────┘
```

---

## 构建

```bash
cd microkernel-clean
make            # 编译成 build/libmicrokernel.a
make clean      # 清理
```

在你的项目里链接：
```bash
gcc -o myapp main.c -I/path/to/microkernel-clean/include \
                   /path/to/microkernel-clean/build/libmicrokernel.a
```

启动入口：`mk_kernel_start()` — 你自己写 main，创建任务，然后调它。

---

## 许可证

MIT
