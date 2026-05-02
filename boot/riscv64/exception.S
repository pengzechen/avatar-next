/*
 * boot/riscv64/exception.S — RISC-V 64-bit S-mode 陷阱/中断入口
 *
 * trap_frame_t 布局（TRAP_FRAME_SIZE = 288 字节）:
 *   偏移  0 .. 31*8 : x0..x31  (x2=sp 保存原始值)
 *   偏移 32*8 = 256 : sepc
 *   偏移 33*8 = 264 : scause
 *   偏移 34*8 = 272 : stval
 *   偏移 35*8 = 280 : sstatus
 */

.equ TRAP_FRAME_SIZE, 288

.section .text
.align 4                    /* stvec 直接模式要求 4 字节对齐 */
.global trap_vector
.extern handle_exception
.extern sched_check_and_yield

trap_vector:
    /* ── 分配陷阱帧 ─────────────────────────────────────────────── */
    addi  sp,  sp, -TRAP_FRAME_SIZE

    /* ── 保存通用寄存器 x0..x31 ─────────────────────────────────── */
    sd    x0,   0*8(sp)     /* x0 始终为 0，保存供调试使用       */
    sd    x1,   1*8(sp)     /* ra                                 */
    /* x2(sp) 稍后保存原始值                                        */
    sd    x3,   3*8(sp)     /* gp                                 */
    sd    x4,   4*8(sp)     /* tp                                 */
    sd    x5,   5*8(sp)     /* t0                                 */
    sd    x6,   6*8(sp)     /* t1                                 */
    sd    x7,   7*8(sp)     /* t2                                 */
    sd    x8,   8*8(sp)     /* s0/fp                              */
    sd    x9,   9*8(sp)     /* s1                                 */
    sd    x10, 10*8(sp)     /* a0                                 */
    sd    x11, 11*8(sp)     /* a1                                 */
    sd    x12, 12*8(sp)     /* a2                                 */
    sd    x13, 13*8(sp)     /* a3                                 */
    sd    x14, 14*8(sp)     /* a4                                 */
    sd    x15, 15*8(sp)     /* a5                                 */
    sd    x16, 16*8(sp)     /* a6                                 */
    sd    x17, 17*8(sp)     /* a7                                 */
    sd    x18, 18*8(sp)     /* s2                                 */
    sd    x19, 19*8(sp)     /* s3                                 */
    sd    x20, 20*8(sp)     /* s4                                 */
    sd    x21, 21*8(sp)     /* s5                                 */
    sd    x22, 22*8(sp)     /* s6                                 */
    sd    x23, 23*8(sp)     /* s7                                 */
    sd    x24, 24*8(sp)     /* s8                                 */
    sd    x25, 25*8(sp)     /* s9                                 */
    sd    x26, 26*8(sp)     /* s10                                */
    sd    x27, 27*8(sp)     /* s11                                */
    sd    x28, 28*8(sp)     /* t3                                 */
    sd    x29, 29*8(sp)     /* t4                                 */
    sd    x30, 30*8(sp)     /* t5                                 */
    sd    x31, 31*8(sp)     /* t6                                 */

    /* ── 保存进入陷阱前的原始 sp ────────────────────────────────── */
    addi  t0,  sp, TRAP_FRAME_SIZE
    sd    t0,  2*8(sp)

    /* ── 保存 CSR ────────────────────────────────────────────────── */
    csrr  t0, sepc
    sd    t0, 32*8(sp)

    csrr  t0, scause
    sd    t0, 33*8(sp)

    csrr  t0, stval
    sd    t0, 34*8(sp)

    csrr  t0, sstatus
    sd    t0, 35*8(sp)

    /* ── 调用 C 处理函数 ─────────────────────────────────────────── */
    mv    a0, sp            /* 第一个参数：trap_frame_t * */
    call  handle_exception

    /* ── 检查是否需要重调度（IRQ 触发的抢占）─────────────────────── */
    call  sched_check_and_yield

    /* ── 恢复 CSR ───────────────────────────────────────────────── */
    ld    t0, 32*8(sp)
    csrw  sepc, t0

    ld    t0, 35*8(sp)
    csrw  sstatus, t0

    /* ── 恢复通用寄存器（跳过 x2=sp，最后恢复）─────────────────── */
    ld    x1,   1*8(sp)
    ld    x3,   3*8(sp)
    ld    x4,   4*8(sp)
    ld    x5,   5*8(sp)
    ld    x6,   6*8(sp)
    ld    x7,   7*8(sp)
    ld    x8,   8*8(sp)
    ld    x9,   9*8(sp)
    ld    x10, 10*8(sp)
    ld    x11, 11*8(sp)
    ld    x12, 12*8(sp)
    ld    x13, 13*8(sp)
    ld    x14, 14*8(sp)
    ld    x15, 15*8(sp)
    ld    x16, 16*8(sp)
    ld    x17, 17*8(sp)
    ld    x18, 18*8(sp)
    ld    x19, 19*8(sp)
    ld    x20, 20*8(sp)
    ld    x21, 21*8(sp)
    ld    x22, 22*8(sp)
    ld    x23, 23*8(sp)
    ld    x24, 24*8(sp)
    ld    x25, 25*8(sp)
    ld    x26, 26*8(sp)
    ld    x27, 27*8(sp)
    ld    x28, 28*8(sp)
    ld    x29, 29*8(sp)
    ld    x30, 30*8(sp)
    ld    x31, 31*8(sp)

    /* ── 恢复 sp，返回 ──────────────────────────────────────────── */
    addi  sp, sp, TRAP_FRAME_SIZE
    sret
