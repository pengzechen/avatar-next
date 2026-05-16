/* lib/setjmp/setjmp_riscv64.S — RISC-V64 setjmp/longjmp for Lua freestanding
 *
 * jmp_buf layout (32 × uint64_t = 256 bytes):
 *  [0]      ra        (return address)
 *  [1]      sp        (stack pointer)
 *  [2..13]  s0-s11    (callee-saved GP registers)
 *  [14..25] fs0-fs11  (callee-saved FP registers)
 *  [26..31] reserved/padding
 */

    .section .text, "ax"
    .balign 4

/* int setjmp(jmp_buf env)  — a0 = &env */
    .global setjmp
    .type   setjmp, @function
setjmp:
    sd  ra,   0*8(a0)
    sd  sp,   1*8(a0)
    sd  s0,   2*8(a0)
    sd  s1,   3*8(a0)
    sd  s2,   4*8(a0)
    sd  s3,   5*8(a0)
    sd  s4,   6*8(a0)
    sd  s5,   7*8(a0)
    sd  s6,   8*8(a0)
    sd  s7,   9*8(a0)
    sd  s8,  10*8(a0)
    sd  s9,  11*8(a0)
    sd  s10, 12*8(a0)
    sd  s11, 13*8(a0)
    /* FP callee-saved registers */
    fsd fs0,  14*8(a0)
    fsd fs1,  15*8(a0)
    fsd fs2,  16*8(a0)
    fsd fs3,  17*8(a0)
    fsd fs4,  18*8(a0)
    fsd fs5,  19*8(a0)
    fsd fs6,  20*8(a0)
    fsd fs7,  21*8(a0)
    fsd fs8,  22*8(a0)
    fsd fs9,  23*8(a0)
    fsd fs10, 24*8(a0)
    fsd fs11, 25*8(a0)
    li  a0, 0
    ret
    .size setjmp, .-setjmp


/* void longjmp(jmp_buf env, int val)  — a0 = &env, a1 = val */
    .global longjmp
    .type   longjmp, @function
longjmp:
    ld  ra,   0*8(a0)
    ld  sp,   1*8(a0)
    ld  s0,   2*8(a0)
    ld  s1,   3*8(a0)
    ld  s2,   4*8(a0)
    ld  s3,   5*8(a0)
    ld  s4,   6*8(a0)
    ld  s5,   7*8(a0)
    ld  s6,   8*8(a0)
    ld  s7,   9*8(a0)
    ld  s8,  10*8(a0)
    ld  s9,  11*8(a0)
    ld  s10, 12*8(a0)
    ld  s11, 13*8(a0)
    fld fs0,  14*8(a0)
    fld fs1,  15*8(a0)
    fld fs2,  16*8(a0)
    fld fs3,  17*8(a0)
    fld fs4,  18*8(a0)
    fld fs5,  19*8(a0)
    fld fs6,  20*8(a0)
    fld fs7,  21*8(a0)
    fld fs8,  22*8(a0)
    fld fs9,  23*8(a0)
    fld fs10, 24*8(a0)
    fld fs11, 25*8(a0)
    /* Return value: a1 if nonzero, else 1 */
    seqz a0, a1
    add  a0, a0, a1     /* a0 = (a1==0) ? 1 : a1 */
    ret
    .size longjmp, .-longjmp
