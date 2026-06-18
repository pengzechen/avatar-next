/*
 * kernel/task/riscv64/switch.S — RISC-V 64-bit 内核上下文切换
 *
 * void arch_task_switch(uintptr_t *prev_sp, uintptr_t next_sp)
 *   a0 = &prev->sp  （输出：保存当前 SP）
 *   a1 = next->sp   （输入：下一任务的 SP）
 *
 * 保存被调用者保存寄存器 ra + s0-s11（13 × 8 = 104 字节）
 * + FP callee-saved fs0-fs11 + fcsr（13 × 8 = 104 字节）
 * 总帧 = 208 字节
 *
 * 将 SP 写入 *a0，加载 a1 为新 SP，恢复寄存器，ret。
 *
 * 保存帧布局（saved_sp 起，偏移从低到高）：
 *   [0]   ra  ← ret 跳转目标
 *   [8]   s0    [16]  s1
 *   [24]  s2    [32]  s3
 *   [40]  s4    [48]  s5
 *   [56]  s6    [64]  s7
 *   [72]  s8    [80]  s9
 *   [88]  s10   [96]  s11
 *   [104] fs0   [112] fs1
 *   [120] fs2   [128] fs3
 *   [136] fs4   [144] fs5
 *   [152] fs6   [160] fs7
 *   [168] fs8   [176] fs9
 *   [184] fs10  [192] fs11
 *   [200] fcsr
 */

.section .text
.align  2
.global arch_task_switch
.extern arch_user_entry_debug

arch_task_switch:
    /* ── 分配帧（26 × 8 = 208 字节）─────────────────────── */
    addi    sp,  sp,  -208

    /* ── 保存被调用者保存寄存器 ──────────────────────────── */
    sd      ra,    0(sp)
    sd      s0,    8(sp)
    sd      s1,   16(sp)
    sd      s2,   24(sp)
    sd      s3,   32(sp)
    sd      s4,   40(sp)
    sd      s5,   48(sp)
    sd      s6,   56(sp)
    sd      s7,   64(sp)
    sd      s8,   72(sp)
    sd      s9,   80(sp)
    sd      s10,  88(sp)
    sd      s11,  96(sp)

    /* ── 保存 FP callee-saved 寄存器 fs0-fs11 + fcsr ────── */
    fsd     fs0,  104(sp)
    fsd     fs1,  112(sp)
    fsd     fs2,  120(sp)
    fsd     fs3,  128(sp)
    fsd     fs4,  136(sp)
    fsd     fs5,  144(sp)
    fsd     fs6,  152(sp)
    fsd     fs7,  160(sp)
    fsd     fs8,  168(sp)
    fsd     fs9,  176(sp)
    fsd     fs10, 184(sp)
    fsd     fs11, 192(sp)
    frcsr   t0
    sd      t0,   200(sp)

    /* ── 保存当前 SP 到 *prev_sp（a0）────────────────────── */
    sd      sp, 0(a0)

    /* ── 保存当前 satp 到 *prev_pgd_ptr（a2，可为 NULL）────── */
    beqz    a2, 1f
    csrr    t0, satp
    li      t1, 0x0fffffffffff  /* SATP.PPN[43:0] */
    and     t0, t0, t1
    slli    t0, t0, 12          /* -> 物理地址 */
    sd      t0, 0(a2)
1:

    /* ── 切换页表（a3 = next_pgd，物理地址；NULL 表示保持当前页表） */
    beqz    a3, 3f
    srli    t0, a3, 12          /* PPN */
    li      t1, (8 << 60)       /* MODE=8 (Sv39), ASID=0 */
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma
3:

    /* ── 加载下一任务的 SP（a1）──────────────────────────── */
    mv      sp, a1

    /* ── 恢复被调用者保存寄存器 ──────────────────────────── */
    ld      ra,    0(sp)
    ld      s0,    8(sp)
    ld      s1,   16(sp)
    ld      s2,   24(sp)
    ld      s3,   32(sp)
    ld      s4,   40(sp)
    ld      s5,   48(sp)
    ld      s6,   56(sp)
    ld      s7,   64(sp)
    ld      s8,   72(sp)
    ld      s9,   80(sp)
    ld      s10,  88(sp)
    ld      s11,  96(sp)

    /* ── 恢复 FP callee-saved 寄存器 fs0-fs11 + fcsr ────── */
    ld      t0,   200(sp)
    fscsr   t0
    fld     fs0,  104(sp)
    fld     fs1,  112(sp)
    fld     fs2,  120(sp)
    fld     fs3,  128(sp)
    fld     fs4,  136(sp)
    fld     fs5,  144(sp)
    fld     fs6,  152(sp)
    fld     fs7,  160(sp)
    fld     fs8,  168(sp)
    fld     fs9,  176(sp)
    fld     fs10, 184(sp)
    fld     fs11, 192(sp)

    addi    sp,   sp,  208

    /* ── 返回（跳转到 ra = 下一任务的返回地址）─────────── */
    ret
    
    /* 永远不应该执行到这里 */
    ebreak

/*
 * task_trampoline_user - 用户进程首次调度时的跳转板（RISC-V 64）
 *
 * 约定（由 arch_init_user_stack 构造）：
 *   s0 = user_entry
 *   s1 = user_sp
 */
.global task_trampoline_user
task_trampoline_user:
    /* arch_switch_to_user(user_entry, user_sp, kernel_sp) */
    mv      a0, s0
    mv      a1, s1
    mv      a2, sp
    mv      a3, s2
    j       arch_switch_to_user

/*
 * arch_switch_to_user - 从内核跳转到用户态
 * a0 = user_entry
 * a1 = user_sp
 * a2 = kernel_sp（当前内核栈）
 * a3 = user_pgd_phys（用户页表物理地址，0 表示保持当前 satp）
 */
.global arch_switch_to_user
arch_switch_to_user:
    /* 保存参数到栈上（调试函数会用到） */
    addi    sp, sp, -32
    sd      a0, 0(sp)
    sd      a1, 8(sp)
    sd      a2, 16(sp)
    sd      a3, 24(sp)
    
    /* 调用调试函数 arch_user_entry_debug(a0, a1, a2, a3) */
    call    arch_user_entry_debug
    
    /* 恢复参数 */
    ld      a0, 0(sp)
    ld      a1, 8(sp)
    ld      a2, 16(sp)
    ld      a3, 24(sp)
    addi    sp, sp, 32

     /* 首次用户态进入在这里切换 satp，避免在 arch_task_switch 中断映射 */
     beqz    a3, 9f
     srli    t0, a3, 12          /* PPN */
    li      t1, (8 << 60)       /* MODE=8 (Sv39), ASID=0 */
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma
9:

    /* 设置返回地址到用户入口 */
    csrw    sepc, a0

    /* 设置 sstatus：SPP=0 (返回 U 模式)，SPIE=1，SIE=0
     * 对齐 AArch64 设计：内核态 SIE=0，sret 通过 SPIE→SIE 在用户态使能中断。
     * 不能在 sret 前设 SIE=1，否则定时器可在 S 态窗口触发，破坏 t0/t1。 */
    csrr    t0, sstatus
    li      t1, ~0x102          /* 清 SPP (bit 8) 和 SIE (bit 1) */
    and     t0, t0, t1
    li      t1, ((1 << 23) | 0x20)  /* 设置平台要求 bit23，同时设 SPIE(bit5) */
    or      t0, t0, t1
    csrw    sstatus, t0

    /* 清除 hstatus.SPV (bit 7) + hstatus.SPVP (bit 8)
     *
     * 若 VMM guest 陷入后立刻调度到该用户进程，hstatus.SPV 可能仍为 1
     * （硬件在 VS-mode 陷阱时设置，hext_trap_vector 不负责恢复）。
     * 若 sret 时 hstatus.SPV=1 + sstatus.SPP=0，CPU 会进入 VU-mode 而非
     * U-mode，导致用 vsatp（kernel satp）翻译用户 VA 0x10000 → 未映射 → Inst PF。
     * 此处强制清零，确保 sret 总是进入普通 U-mode。
     */
#if !defined(PLATFORM_SG2002)
    csrr    t0, hstatus
    li      t1, ~0x180          /* 清 SPV (bit 7) 和 SPVP (bit 8) */
    and     t0, t0, t1
    csrw    hstatus, t0
#endif

    /* 用户栈 */
    mv      sp, a1

    /* Fresh exec entry must not inherit the kernel/previous task TLS pointer. */
    mv      tp, zero

    /* 同步 I-cache，确保刚写入的用户代码可执行 */
    fence.i
#if defined(PLATFORM_SG2002)
    .long 0x0100000b         /* ICACHE.IALL */
    .long 0x01a0000b         /* SYNC.I */
#endif

    /* 保存内核栈到 sscratch，必须尽量贴近 sret。
     * 在此之前仍处于 S-mode，若过早设置 sscratch，S 态异常会被
     * trap_vector 误判成 U 态陷入。 */
    csrw    sscratch, a2

    sret

/*
 * arch_fork_resume_user - 子进程首次调度恢复 trap_frame 并返回用户态
 * 约定：s0 = trap_frame_t*
 */
.global arch_fork_resume_user
arch_fork_resume_user:
    mv      t6, s0

    /* trap CSR */
    ld      t0, (32*8)(t6)      /* sepc */
    csrw    sepc, t0
    ld      t1, (35*8)(t6)      /* sstatus */
    li      t2, -259            /* clear SPP + SIE */
    and     t1, t1, t2
    ori     t1, t1, 0x20        /* SPIE */
    csrw    sstatus, t1

    /* 清除 hstatus.SPV + SPVP，确保 sret 进入 U-mode 而非 VU-mode。
     * SG2002/C906 无 H 扩展，访问 hstatus 会触发非法指令。 */
#if !defined(PLATFORM_SG2002)
    csrr    t0, hstatus
    li      t1, ~0x180
    and     t0, t0, t1
    csrw    hstatus, t0
#endif

    /* 保存当前内核栈到 sscratch */
    mv      t0, sp
    csrw    sscratch, t0

    /* 恢复 FP 寄存器 f0..f31 + fcsr（从 trap_frame） */
    ld      t0, (68*8)(t6)
    fscsr   t0
    fld     f0,  (36*8)(t6)
    fld     f1,  (37*8)(t6)
    fld     f2,  (38*8)(t6)
    fld     f3,  (39*8)(t6)
    fld     f4,  (40*8)(t6)
    fld     f5,  (41*8)(t6)
    fld     f6,  (42*8)(t6)
    fld     f7,  (43*8)(t6)
    fld     f8,  (44*8)(t6)
    fld     f9,  (45*8)(t6)
    fld     f10, (46*8)(t6)
    fld     f11, (47*8)(t6)
    fld     f12, (48*8)(t6)
    fld     f13, (49*8)(t6)
    fld     f14, (50*8)(t6)
    fld     f15, (51*8)(t6)
    fld     f16, (52*8)(t6)
    fld     f17, (53*8)(t6)
    fld     f18, (54*8)(t6)
    fld     f19, (55*8)(t6)
    fld     f20, (56*8)(t6)
    fld     f21, (57*8)(t6)
    fld     f22, (58*8)(t6)
    fld     f23, (59*8)(t6)
    fld     f24, (60*8)(t6)
    fld     f25, (61*8)(t6)
    fld     f26, (62*8)(t6)
    fld     f27, (63*8)(t6)
    fld     f28, (64*8)(t6)
    fld     f29, (65*8)(t6)
    fld     f30, (66*8)(t6)
    fld     f31, (67*8)(t6)

    /* 恢复通用寄存器（x0 固定为 0） */
    ld      ra, (1*8)(t6)
    ld      sp, (2*8)(t6)
    ld      gp, (3*8)(t6)
    ld      tp, (4*8)(t6)
    ld      t0, (5*8)(t6)
    ld      t1, (6*8)(t6)
    ld      t2, (7*8)(t6)
    ld      s0, (8*8)(t6)
    ld      s1, (9*8)(t6)
    ld      a0, (10*8)(t6)
    ld      a1, (11*8)(t6)
    ld      a2, (12*8)(t6)
    ld      a3, (13*8)(t6)
    ld      a4, (14*8)(t6)
    ld      a5, (15*8)(t6)
    ld      a6, (16*8)(t6)
    ld      a7, (17*8)(t6)
    ld      s2, (18*8)(t6)
    ld      s3, (19*8)(t6)
    ld      s4, (20*8)(t6)
    ld      s5, (21*8)(t6)
    ld      s6, (22*8)(t6)
    ld      s7, (23*8)(t6)
    ld      s8, (24*8)(t6)
    ld      s9, (25*8)(t6)
    ld      s10, (26*8)(t6)
    ld      s11, (27*8)(t6)
    ld      t3, (28*8)(t6)
    ld      t4, (29*8)(t6)
    ld      t5, (30*8)(t6)
    ld      t6, (31*8)(t6)

    /* 同步 I-cache，避免 fork 后子进程执行陈旧指令 */
    fence.i
#if defined(PLATFORM_SG2002)
    .long 0x0100000b         /* ICACHE.IALL */
    .long 0x01a0000b         /* SYNC.I */
#endif

    sret
