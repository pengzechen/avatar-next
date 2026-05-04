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
.equ SSTATUS_SPP,    0x100

.section .text
.align 4                    /* stvec 直接模式要求 4 字节对齐 */
.global trap_vector
.extern handle_exception
.extern sched_check_and_yield
.extern __global_pointer$

trap_vector:
    /*
     * 先根据 SPP 判断来源模式：
     *   SPP=0: 从 U 态陷入，切换到 sscratch 保存的内核栈
     *   SPP=1: 从 S 态陷入，继续使用当前内核栈
     *
     * 注意：这里会临时改写 t0/t1，后续会在 trap frame 中保存当前值。
     */
    csrr  t0, sstatus
    andi  t0, t0, SSTATUS_SPP
    bnez  t0, 1f

    /* U -> S: sp=user_sp, sscratch=kernel_sp */
    csrr  t0, sscratch       /* t0 = kernel_sp */
    mv    t1, sp             /* t1 = user_sp   */
    mv    sp, t0             /* 切换到内核栈   */
    addi  sp, sp, -TRAP_FRAME_SIZE
    j     2f

1:
    /* S -> S: 直接在当前内核栈建帧 */
    addi  sp, sp, -TRAP_FRAME_SIZE
    addi  t1, sp, TRAP_FRAME_SIZE

2:

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
    sd    t1,  2*8(sp)

    /* ── 保存 CSR ────────────────────────────────────────────────── */
    csrr  t0, sepc
    sd    t0, 32*8(sp)

    csrr  t0, scause
    sd    t0, 33*8(sp)

    csrr  t0, stval
    sd    t0, 34*8(sp)

    csrr  t0, sstatus
    sd    t0, 35*8(sp)

    /*
     * 重要：从 U 态陷入时，x3(gp) 仍是用户态 gp。
     * C 代码（handle_exception/sched_check_and_yield）依赖内核 gp 访问
     * 小数据段；若不切回内核 gp，会导致陷阱处理静默异常。
     */
    la    t0, __global_pointer$
    mv    gp, t0

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

    /*
     * 若将返回 U 态（SPP=0），更新 sscratch = 当前内核栈顶。
     * 返回 S 态时不修改 sscratch。
     */
    andi  t1, t0, SSTATUS_SPP
    bnez  t1, 3f
    addi  t1, sp, TRAP_FRAME_SIZE
    csrw  sscratch, t1
3:

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

    /* ── 恢复原始 sp（U/S 态各自原值），返回 ─────────────────────── */
    ld    x2,   2*8(sp)
    sret
