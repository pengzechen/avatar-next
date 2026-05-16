/* lib/setjmp/setjmp_aarch64.S — AArch64 setjmp/longjmp for Lua freestanding
 *
 * jmp_buf layout (24 × uint64_t = 192 bytes):
 *  [0..9]   x19-x28   (callee-saved GP registers)
 *  [10]     x29       (frame pointer)
 *  [11]     x30       (link register / return address)
 *  [12]     sp        (stack pointer)
 *  [13..20] d8-d15    (callee-saved FP registers)
 *  [21..23] reserved/padding
 */

    .section .text, "ax"
    .balign 4

/* int setjmp(jmp_buf env)  — x0 = &env */
    .global setjmp
    .type   setjmp, %function
setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]      /* fp, lr */
    mov x1,  sp
    str x1,  [x0, #96]
    stp d8,  d9,  [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov x0,  #0
    ret
    .size setjmp, .-setjmp


/* void longjmp(jmp_buf env, int val)  — x0 = &env, w1 = val */
    .global longjmp
    .type   longjmp, %function
longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]      /* fp, lr */
    ldr x2,  [x0, #96]
    mov sp,  x2
    ldp d8,  d9,  [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    /* Return val; if val == 0, return 1 (per POSIX) */
    cmp w1,  #0
    csinc w0, w1, wzr, ne
    ret                          /* jumps to saved x30 */
    .size longjmp, .-longjmp
