

    .text

    .globl mk_ctx_swap
    .type  mk_ctx_swap, @function
mk_ctx_swap:
    
    movq    %rbx,    0(%rdi)
    movq    %rbp,    8(%rdi)
    movq    %r12,   16(%rdi)
    movq    %r13,   24(%rdi)
    movq    %r14,   32(%rdi)
    movq    %r15,   40(%rdi)
    
    movq    %rsp,   48(%rdi)

    
    movq     0(%rsi), %rbx
    movq     8(%rsi), %rbp
    movq    16(%rsi), %r12
    movq    24(%rsi), %r13
    movq    32(%rsi), %r14
    movq    40(%rsi), %r15
    movq    48(%rsi), %rsp          

    ret                              
    .size mk_ctx_swap, .-mk_ctx_swap

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

    .globl mk_ctx_init
    .type  mk_ctx_init, @function
mk_ctx_init:
    
    movq    $0,     0(%rdi)      
    movq    $0,     8(%rdi)      
    movq    $0,    16(%rdi)      
    movq    $0,    24(%rdi)      
    movq    $0,    32(%rdi)      
    movq    $0,    40(%rdi)      

    
    addq    %rcx,   %rdx         

    
    subq    $8,     %rdx         
    andq    $~15,   %rdx         

    
    movq    %rsi,  (%rdx)        

    
    movq    %rdx,  48(%rdi)

    ret
    .size mk_ctx_init, .-mk_ctx_init

    .section .note.GNU-stack,"",@progbits
