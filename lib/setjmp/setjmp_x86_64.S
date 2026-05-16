/* lib/setjmp/setjmp_x86_64.S — x86-64 setjmp/longjmp for Lua freestanding
 *
 * jmp_buf layout (8 × uint64_t = 64 bytes):
 *  [0] rbx
 *  [1] rbp
 *  [2] r12
 *  [3] r13
 *  [4] r14
 *  [5] r15
 *  [6] rsp  (caller's rsp = rsp+8 at call site, after push of return address)
 *  [7] rip  (return address)
 */

    .section .text, "ax"
    .balign 16

/* int setjmp(jmp_buf env)  — rdi = &env */
    .global setjmp
    .type   setjmp, @function
setjmp:
    mov  %rbx,    0(%rdi)
    mov  %rbp,    8(%rdi)
    mov  %r12,   16(%rdi)
    mov  %r13,   24(%rdi)
    mov  %r14,   32(%rdi)
    mov  %r15,   40(%rdi)
    /* Save caller's rsp (= current rsp + 8, because ret address is on stack) */
    lea  8(%rsp), %rax
    mov  %rax,   48(%rdi)
    /* Save return address */
    mov  (%rsp),  %rax
    mov  %rax,   56(%rdi)
    xor  %eax,   %eax
    ret
    .size setjmp, .-setjmp


/* void longjmp(jmp_buf env, int val)  — rdi = &env, esi = val */
    .global longjmp
    .type   longjmp, @function
longjmp:
    mov   0(%rdi), %rbx
    mov   8(%rdi), %rbp
    mov  16(%rdi), %r12
    mov  24(%rdi), %r13
    mov  32(%rdi), %r14
    mov  40(%rdi), %r15
    mov  48(%rdi), %rsp
    /* Return value: val if nonzero, else 1 */
    mov  %esi,     %eax
    test %eax,     %eax
    jnz  1f
    mov  $1,       %eax
1:
    /* Jump to saved rip */
    jmp  *56(%rdi)
    .size longjmp, .-longjmp
