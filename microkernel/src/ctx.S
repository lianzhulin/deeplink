/*
 * ctx.S — L2 轻量上下文原语 (x86-64 SysV ABI)
 *
 * mk_light_ctx_t 布局 (56 bytes):
 *   offset 0:  rbx
 *   offset 8:  rbp
 *   offset 16: r12
 *   offset 24: r13
 *   offset 32: r14
 *   offset 40: r15
 *   offset 48: rsp
 *
 * 只保存 callee-saved 寄存器 + rsp。
 * 返回地址通过 ret 指令从栈弹出，不单独存。
 * 省掉了 glibc ucontext_t 的 FPU/SSE 状态、signal mask、uc_link、uc_stack 等。
 */

    .text

/* ================================================================
 * void mk_ctx_swap(mk_light_ctx_t *old_ctx, mk_light_ctx_t *new_ctx);
 *
 * SysV ABI: rdi = old_ctx, rsi = new_ctx
 *
 * 语义：保存当前上下文到 old_ctx，然后恢复 new_ctx 并跳转过去。
 *       等价于 swapcontext(old_ctx, new_ctx)。
 *
 * 关键：保存 rsp 时用 leaq 8(%rsp), ...
 *       因为 call 指令已经 push 了返回地址，当前 rsp 指向它；
 *       我们要保存的是 "call 之前" 的栈顶。
 *       恢复时直接 mov 48(%rsi), %rsp，然后 ret 弹出新栈上的返回地址。
 * ================================================================ */
    .globl mk_ctx_swap
    .type  mk_ctx_swap, @function
mk_ctx_swap:
    /* ---- 保存 callee-saved 寄存器到 old_ctx (rdi) ---- */
    movq    %rbx,    0(%rdi)
    movq    %rbp,    8(%rdi)
    movq    %r12,   16(%rdi)
    movq    %r13,   24(%rdi)
    movq    %r14,   32(%rdi)
    movq    %r15,   40(%rdi)
    /* 关键：直接存 rsp（call 已经 push 了 return address）。
     * ret 会从 [ctx->rsp] 弹出 → 我们需要 ctx->rsp 指向 return address 本身。 */
    movq    %rsp,   48(%rdi)

    /* ---- 从 new_ctx (rsi) 恢复 callee-saved 寄存器 ---- */
    movq     0(%rsi), %rbx
    movq     8(%rsi), %rbp
    movq    16(%rsi), %r12
    movq    24(%rsi), %r13
    movq    32(%rsi), %r14
    movq    40(%rsi), %r15
    movq    48(%rsi), %rsp          /* 切换栈！此时栈顶是目标返回地址 */

    ret                              /* 弹出目标地址，开始执行 */
    .size mk_ctx_swap, .-mk_ctx_swap


/* ================================================================
 * void mk_ctx_load(mk_light_ctx_t *ctx);
 *
 * SysV ABI: rdi = ctx
 *
 * 语义：只恢复 ctx 并跳转，不保存当前任何东西。
 *       等价于 setcontext(ctx)。
 *       用于调度器首次启动 —— 没有 "prev" 上下文可保存。
 * ================================================================ */
    .globl mk_ctx_load
    .type  mk_ctx_load, @function
mk_ctx_load:
    movq     0(%rdi), %rbx
    movq     8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp

    ret
    .size mk_ctx_load, .-mk_ctx_load


/* ================================================================
 * void mk_ctx_init(mk_light_ctx_t *ctx, void (*entry)(void),
 *                  void *stack, size_t stack_size);
 *
 * SysV ABI: rdi = ctx, rsi = entry, rdx = stack, rcx = stack_size
 *
 * 语义：初始化 ctx 为一个 "首次运行" 的上下文。
 *       callee-saved 寄存器清零，
 *       栈顶压入 entry 作为 ret 时的返回地址，
 *       rsp 指向 entry 的位置。
 *
 * 栈对齐：
 *   posix_memalign 返回 16B 对齐的栈底。
 *   stack_top = stack + stack_size 也是 16B 对齐（0 mod 16）。
 *   我们需要：ret 之后 rsp 满足 SysV ABI 的 8 mod 16。
 *   ret 做 [rip=*rsp; rsp+=8]，所以 ret 前 rsp 需要是 0 mod 16。
 *   即 ctx->rsp = stack_top - 8 (存入 entry 后)，stack_top 必须是 8 mod 16。
 *
 *   等等...SysV ABI 在函数入口处 rsp % 16 == 8（因为 call push 8B 后 rsp % 16 == 0）。
 *   我们用 ret 来跳转到 entry，ret 之后 rsp = ctx->rsp_saved + 8。
 *   要求 ctx->rsp_saved + 8 ≡ 8 (mod 16) → ctx->rsp_saved ≡ 0 (mod 16)。
 *   把 stack_top 先 & ~15 对齐到 0 mod 16，再 -8 存入 entry 地址，
 *   这样 ctx->rsp = stack_top_aligned - 8... 不对，让我再算：
 *
 *   ret 前 rsp = ctx->rsp（我们设的值）
 *   ret 后 rsp = ctx->rsp + 8，此时我们在 entry 函数入口
 *   要求 ctx->rsp + 8 ≡ 8 (mod 16) → ctx->rsp ≡ 0 (mod 16)
 *
 *   entry 存在 [ctx->rsp] 的位置。
 *   所以 ctx->rsp 必须是 0 mod 16 的地址，里面存 entry。
 *
 *   stack_top 初始是 0 mod 16（来自 posix_memalign 的 stack + size）。
 *   我们需要把 entry 放在 stack_top - 8 或 stack_top - 24...
 *   只要地址是 0 mod 16 就行。
 *   stack_top - 8 是 8 mod 16，不行；stack_top - 16 是 0 mod 16，可行。
 *
 *   但这样 stack_top - 8 这个位置就空了。问题不大。
 *
 *   更干净的做法：把 stack_top 对齐到 0 mod 16，然后 ctx->rsp = stack_top，
 *   entry 存 [stack_top]。这样 ret 后 rsp = stack_top + 8。
 *   stack_top 是 0 mod 16，所以 ret 后 rsp 是 8 mod 16 ✓
 * ================================================================ */
    .globl mk_ctx_init
    .type  mk_ctx_init, @function
mk_ctx_init:
    /* ---- callee-saved 寄存器全部清零（新任务从干净状态起跑） ---- */
    movq    $0,     0(%rdi)      /* rbx = 0 */
    movq    $0,     8(%rdi)      /* rbp = 0 */
    movq    $0,    16(%rdi)      /* r12 = 0 */
    movq    $0,    24(%rdi)      /* r13 = 0 */
    movq    $0,    32(%rdi)      /* r14 = 0 */
    movq    $0,    40(%rdi)      /* r15 = 0 */

    /* ---- 计算 stack_top = stack + stack_size ---- */
    addq    %rcx,   %rdx         /* rdx = stack + stack_size = stack_top */

    /* ---- 减去 8 字节（给 return address 留位），再 16B 向下对齐 ----
     *
     *  posix_memalign 返回的 stack + stack_size 是块尾后第一字节，
     *  写那里是越界。所以先 -8 得到最高可写字节，再 & ~15 向下对齐到
     *  0 mod 16 —— 保证 ret 之后 rsp = 0 mod 16 + 8 = 8 mod 16，
     *  符合 SysV ABI 在函数入口的栈对齐约定。 */
    subq    $8,     %rdx         /* stack_top - 8 = 最高可写字节 */
    andq    $~15,   %rdx         /* 向下对齐到 0 mod 16 */

    /* ---- 在对齐后的地址存入 entry 作为 return address ---- */
    movq    %rsi,  (%rdx)        /* [aligned_sp] = entry */

    /* ---- ctx->rsp = aligned_sp ---- */
    movq    %rdx,  48(%rdi)

    ret
    .size mk_ctx_init, .-mk_ctx_init

/* 标记栈为非可执行，消掉 linker 警告 */
    .section .note.GNU-stack,"",@progbits
