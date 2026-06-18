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

.equ TRAP_FRAME_SIZE, 552
.equ SSTATUS_SPP,    0x100

.section .text
.align 4                    /* stvec 直接模式要求 4 字节对齐 */
.global trap_vector
.extern handle_exception
.extern sched_check_and_yield_from_trap
.extern __global_pointer$
.extern g_current_user_satp

trap_vector:
    /*
     * 使用 sscratch=0 约定判断来源模式（与 Linux RISC-V 内核一致）：
     *   sscratch=0   : 从 S 态（内核）陷入
     *   sscratch≠0   : 从 U 态（用户）陷入，值为内核栈指针
     *
     * 原子交换 t0 ↔ sscratch：
     *   进入后: t0 = 旧 sscratch, sscratch = 旧 t0
     *
     * 这样在任何分支保存 x5(t0) 之前，t0 和 t1 都不会被污染，
     * 修复了旧代码 S→S 路径中 csrr t0,sstatus/andi 覆盖 t0 的问题。
     */
    csrrw  t0, sscratch, t0
    bnez   t0, .Lfrom_user

.Lfrom_kernel:
    /* S → S: t0=0（旧 sscratch），sscratch=原始 t0
     * 恢复原始 t0，同时将 sscratch 清零（维持"内核态=0"约定） */
    csrrw  t0, sscratch, x0      /* t0=原始t0, sscratch=0 */
    addi   sp, sp, -TRAP_FRAME_SIZE
    sd     x5,  5*8(sp)          /* 原始 t0（正确！） */
    sd     x6,  6*8(sp)          /* 原始 t1（未被修改，正确！） */
    addi   t1, sp, TRAP_FRAME_SIZE
    sd     t1,  2*8(sp)          /* 原始 sp */
    j      .Lsave_rest

.Lfrom_user:
    /* U → S: t0=kernel_sp（旧 sscratch），sscratch=原始 t0。
     * 先用 t0 作为临时 trap_frame 指针保存原始 sp/t1；不能先
     * mv t1, sp，否则会把用户 t1 污染成 user_sp 并在返回时恢复回去。 */
    addi   t0, t0, -TRAP_FRAME_SIZE
    sd     x2,  2*8(t0)          /* user_sp */
    sd     x6,  6*8(t0)          /* 用户原始 t1 */
    csrrw  t1, sscratch, x0      /* t1=用户原始 t0, sscratch=0 */
    sd     t1,  5*8(t0)          /* 用户原始 t0 */
    mv     sp, t0                /* sp = trap_frame */

.Lsave_rest:

    /* ── 保存通用寄存器 x0..x31 ─────────────────────────────────── */
    sd    x0,   0*8(sp)     /* x0 始终为 0，保存供调试使用       */
    sd    x1,   1*8(sp)     /* ra                                 */
    /* x2(sp) 已在分支路径保存原始值，此处跳过                      */
    sd    x3,   3*8(sp)     /* gp                                 */
    sd    x4,   4*8(sp)     /* tp                                 */
    /* x5(t0) 已在分支路径正确保存，此处跳过                        */
    /* x6(t1) 已在分支路径正确保存，此处跳过                        */
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

    /* ── 保存 CSR ────────────────────────────────────────────────── */
    csrr  t0, sepc
    sd    t0, 32*8(sp)

    csrr  t0, scause
    sd    t0, 33*8(sp)

    csrr  t0, stval
    sd    t0, 34*8(sp)

    csrr  t0, sstatus
    sd    t0, 35*8(sp)

    /* ── 保存 FP 寄存器 f0..f31 + fcsr ─────────────────────────────── */
    fsd   f0,  36*8(sp)
    fsd   f1,  37*8(sp)
    fsd   f2,  38*8(sp)
    fsd   f3,  39*8(sp)
    fsd   f4,  40*8(sp)
    fsd   f5,  41*8(sp)
    fsd   f6,  42*8(sp)
    fsd   f7,  43*8(sp)
    fsd   f8,  44*8(sp)
    fsd   f9,  45*8(sp)
    fsd   f10, 46*8(sp)
    fsd   f11, 47*8(sp)
    fsd   f12, 48*8(sp)
    fsd   f13, 49*8(sp)
    fsd   f14, 50*8(sp)
    fsd   f15, 51*8(sp)
    fsd   f16, 52*8(sp)
    fsd   f17, 53*8(sp)
    fsd   f18, 54*8(sp)
    fsd   f19, 55*8(sp)
    fsd   f20, 56*8(sp)
    fsd   f21, 57*8(sp)
    fsd   f22, 58*8(sp)
    fsd   f23, 59*8(sp)
    fsd   f24, 60*8(sp)
    fsd   f25, 61*8(sp)
    fsd   f26, 62*8(sp)
    fsd   f27, 63*8(sp)
    fsd   f28, 64*8(sp)
    fsd   f29, 65*8(sp)
    fsd   f30, 66*8(sp)
    fsd   f31, 67*8(sp)
    frcsr t0
    sd    t0,  68*8(sp)

    /*
     * 简化方案：不在exception入口切换页表！
     * 原因：1. 用户页表已完整复制内核映射（L1[0x100], L1[0x102]）
     *       2. 内核可在用户页表下安全运行
     *       3. syscall可同时访问内核和用户空间
     *       4. 避免页表切换的复杂性和性能开销
     */

    /*
     * 重要：从 U 态陷入时，x3(gp) 仍是用户态 gp。
     * C 代码（handle_exception/sched_check_and_yield）依赖内核 gp 访问
     * 小数据段；若不切回内核 gp，会导致陷阱处理静默异常。
     *
     * 必须用 .option norelax 禁止 linker relaxation，否则链接器会把
     * "la t0, __global_pointer$" 优化为 "mv t0, gp"（假设 gp 已正确），
     * 在 U→S 陷阱路径上 gp 仍是用户态值，导致内核全局变量访问错误。
     */
    .option push
    .option norelax
    la    t0, __global_pointer$
    .option pop
    mv    gp, t0

    /* ── 调用 C 处理函数 ─────────────────────────────────────────── */
    mv    a0, sp            /* 第一个参数：trap_frame_t * */
    call  handle_exception

    /* ── 检查是否需要重调度（IRQ 触发的抢占）─────────────────────── */
    mv    a0, sp            /* 参数：trap_frame_t * */
    call  sched_check_and_yield_from_trap

    /* ── 恢复 CSR ───────────────────────────────────────────────── */
    ld    t0, 32*8(sp)
    csrw  sepc, t0

    ld    t0, 35*8(sp)
    csrw  sstatus, t0

    /* ── 恢复 FP 寄存器 f0..f31 + fcsr ─────────────────────────────── */
    ld    t1, 68*8(sp)
    fscsr t1
    fld   f0,  36*8(sp)
    fld   f1,  37*8(sp)
    fld   f2,  38*8(sp)
    fld   f3,  39*8(sp)
    fld   f4,  40*8(sp)
    fld   f5,  41*8(sp)
    fld   f6,  42*8(sp)
    fld   f7,  43*8(sp)
    fld   f8,  44*8(sp)
    fld   f9,  45*8(sp)
    fld   f10, 46*8(sp)
    fld   f11, 47*8(sp)
    fld   f12, 48*8(sp)
    fld   f13, 49*8(sp)
    fld   f14, 50*8(sp)
    fld   f15, 51*8(sp)
    fld   f16, 52*8(sp)
    fld   f17, 53*8(sp)
    fld   f18, 54*8(sp)
    fld   f19, 55*8(sp)
    fld   f20, 56*8(sp)
    fld   f21, 57*8(sp)
    fld   f22, 58*8(sp)
    fld   f23, 59*8(sp)
    fld   f24, 60*8(sp)
    fld   f25, 61*8(sp)
    fld   f26, 62*8(sp)
    fld   f27, 63*8(sp)
    fld   f28, 64*8(sp)
    fld   f29, 65*8(sp)
    fld   f30, 66*8(sp)
    fld   f31, 67*8(sp)

    /*
     * 根据返回目的地更新 sscratch：
     *   返回 U 态（SPP=0）: sscratch = 内核栈顶（下次 U→S 陷阱使用）
     *   返回 S 态（SPP=1）: sscratch = 0（维持"内核态=0"约定）
     * 
     * 注意：不再切换页表，内核始终在用户页表下运行（用户页表已包含内核映射）
     */
    andi  t1, t0, SSTATUS_SPP
    bnez  t1, 3f
    /* 返回 U 态：
     * 清除 hstatus.SPV(bit7) + SPVP(bit8)，防止 sret 进入 VU-mode。
     * 当 VMM guest（VS-mode）陷入 HS-mode 后调度到用户线程时，
     * hstatus.SPV 由硬件置 1 且未被清零；若不在此处清零，
     * sret(SPP=0) 会进入 VU-mode → vsatp 无 0x10000 映射 → Inst PF。
     * SG2002/C906 无 H 扩展，访问 hstatus 会触发非法指令。 */
#if !defined(PLATFORM_SG2002)
    csrr  t1, hstatus
    li    t2, ~0x180         /* 清 SPV(bit7) + SPVP(bit8) */
    and   t1, t1, t2
    csrw  hstatus, t1
#endif
#if defined(PLATFORM_SG2002)
    /* T-Head C906: make freshly written user mappings visible to I-fetch. */
    .long 0x0100000b         /* ICACHE.IALL */
    .long 0x01a0000b         /* SYNC.I */
#endif
    /* 设置 sscratch = kernel_sp */
    addi  t1, sp, TRAP_FRAME_SIZE
    csrw  sscratch, t1
    j     4f
3:
    /* 返回 S 态：sscratch 清零 */
    csrw  sscratch, x0
4:

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
