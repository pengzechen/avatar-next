/*
 * kernel/task/aarch64/switch.S — AArch64 内核上下文切换
 *
 * void arch_task_switch(uintptr_t *prev_sp, uintptr_t next_sp,
 *                       uint64_t **prev_pgd_ptr, uint64_t *next_pgd)
 *   x0 = &prev->sp        （输出：保存当前 SP）
 *   x1 = next_sp          （输入：下一任务的 SP）
 *   x2 = &prev->pgd       （输出：保存当前页表，可为NULL）
 *   x3 = next->pgd        （输入：下一任务页表，NULL表示不切换）
 *
 * 保存被调用者保存寄存器 x19-x30 与 FP 状态 d8-d15 + FPCR/FPSR
 * （12 × 8 + 8 × 8 + 2 × 8 = 176 字节），
 * 将 SP 写入 *x0，加载 x1 为新 SP，恢复寄存器，ret。
 *
 * 保存帧布局（saved_sp 起，偏移从低到高）：
 *   [0]   x19   [8]   x20
 *   [16]  x21   [24]  x22
 *   [32]  x23   [40]  x24
 *   [48]  x25   [56]  x26
 *   [64]  x27   [72]  x28
 *   [80]  x29   [88]  x30 ← ret 跳转目标
 *   [96]  d8    [104] d9
 *   [112] d10   [120] d11
 *   [128] d12   [136] d13
 *   [144] d14   [152] d15
 *   [160] fpcr  [168] fpsr
 *
 * ⚠️ 帧大小 176（22 个字）与布局被 kernel/task/switch.h 里三处构造伪帧的
 * 代码硬编码引用：arch_init_task_stack / arch_init_user_stack /
 * arch_init_fork_child_stack。改这里必须同时改那三处，否则首次调度该任务
 * 时 SP 会落到内核栈顶之外。
 */

.section .text
.align  4
.global arch_task_switch

arch_task_switch:
    /* ── 保存被调用者保存寄存器 + FP 状态（176 字节）──────── */
    stp     x19, x20, [sp, #-176]!
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]

    /*
     * ── FP/SIMD：AAPCS64 的被调用者保存部分 ──────────────────────
     * d8-d15（只存低 64 位，AAPCS64 就只要求这些）+ FPCR/FPSR
     * （两者按 AAPCS64 是全局态，被调用者不得破坏）。
     *
     * 这里**不需要**保存 q0-q31：切换点是一次普通 C 函数调用，caller-saved
     * 的 v0-v7/v16-v31 在调用点本来就是死的。用户态的完整 q0-q31 由陷阱帧
     * 负责（boot/aarch64/exception.S 的 SAVE_REGS）—— 两者分工不能混。
     * x9/x10 在函数入口按 AAPCS64 是死的，借来做临时寄存器。
     */
    stp     d8,  d9,  [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]
    mrs     x9, fpcr
    mrs     x10, fpsr
    stp     x9, x10, [sp, #160]

    /* ── 保存当前 SP 到 *prev_sp（x0）────────────────────── */
    mov     x9, sp
    str     x9, [x0]

    /* ── 保存当前页表到 *prev_pgd_ptr（x2，如果非NULL）────────── */
    cbz     x2, 1f
    mrs     x9, ttbr0_el1
    str     x9, [x2]            /* 将 TTBR0_EL1 保存到 prev->pgd */
1:

    /* ── 切换页表（如果下一任务有独立页表）──────────────────── */
    cbz     x3, 2f
    msr     ttbr0_el1, x3       /* 用户进程：切换到独立页表 */
    b       3f
2:
    msr     ttbr0_el1, xzr      /* 内核任务：TTBR0_EL1 = 0（内核页表） */
3:
    isb                         /* 同步上下文切换 */
    tlbi    vmalle1is           /* 刷新 TLB */
    dsb     sy
    isb

    /* ── 加载下一任务的 SP 并恢复寄存器 ──────────────────── */
    mov     sp, x1

    /* FP 状态先恢复（FPCR/FPSR 先于 d8-d15，对照 riscv64 的 fscsr 先于 fld） */
    ldp     x9, x10, [sp, #160]
    msr     fpcr, x9
    msr     fpsr, x10
    ldp     d8,  d9,  [sp, #96]
    ldp     d10, d11, [sp, #112]
    ldp     d12, d13, [sp, #128]
    ldp     d14, d15, [sp, #144]

    ldp     x29, x30, [sp, #80]
    ldp     x27, x28, [sp, #64]
    ldp     x25, x26, [sp, #48]
    ldp     x23, x24, [sp, #32]
    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #176
    ret

/*
 * arch_switch_to_user - 从内核跳转到用户态
 * x0 = user_entry  （用户态入口点虚拟地址）
 * x1 = user_sp     （用户栈指针虚拟地址）
 * x2 = kernel_sp   （内核栈指针物理地址，未使用）
 *
 * 使用 eret 跳转到用户态，不返回。
 *
 * 说明：
 *   - SP_EL0: 用户态栈指针（设置为 user_sp）
 *   - SP_EL1: 内核栈指针（当前的 sp，异常时自动切换）
 *   - 当从 EL0 发生异常时，硬件自动从 SP_EL0 切换到 SP_EL1
 */
.global arch_switch_to_user
arch_switch_to_user:
    /* ── 设置用户栈指针（SP_EL0）──────────────────────────────── */
    msr     sp_el0, x1

    /* ── 设置异常返回地址到用户入口点────────────────────────── */
    msr     elr_el1, x0

    /* ── 设置 SPSR 到 EL0t（用户态，IRQ使能）─────────────────── */
    mov     x9, #0x340           /* EL0t, IRQ/FIQ未屏蔽 */
    msr     spsr_el1, x9

    /*
     * ── 清空 FP/SIMD，再进用户态 ────────────────────────────────
     *
     * 新进程首次进 EL0 时，d8-d15/fpcr/fpsr 由 arch_init_user_stack 构造的
     * 切换帧（全零）恢复，但 v0-v7/v16-v31 是 caller-saved、不在切换帧里 ——
     * 它们此刻仍是**上一个任务或内核代码**留下的值。不清就是把这些 FP 值
     * 泄漏给新进程的寄存器组（Linux 上新进程的初始 FP 状态同样是全零）。
     *
     * 只在 exec/首次进入用户态这条路径上执行一次，代价可忽略。
     */
    msr     fpcr, xzr
    msr     fpsr, xzr
    movi    v0.16b,  #0
    movi    v1.16b,  #0
    movi    v2.16b,  #0
    movi    v3.16b,  #0
    movi    v4.16b,  #0
    movi    v5.16b,  #0
    movi    v6.16b,  #0
    movi    v7.16b,  #0
    movi    v8.16b,  #0
    movi    v9.16b,  #0
    movi    v10.16b, #0
    movi    v11.16b, #0
    movi    v12.16b, #0
    movi    v13.16b, #0
    movi    v14.16b, #0
    movi    v15.16b, #0
    movi    v16.16b, #0
    movi    v17.16b, #0
    movi    v18.16b, #0
    movi    v19.16b, #0
    movi    v20.16b, #0
    movi    v21.16b, #0
    movi    v22.16b, #0
    movi    v23.16b, #0
    movi    v24.16b, #0
    movi    v25.16b, #0
    movi    v26.16b, #0
    movi    v27.16b, #0
    movi    v28.16b, #0
    movi    v29.16b, #0
    movi    v30.16b, #0
    movi    v31.16b, #0

    /* ── 跳转到用户态──────────────────────────────────────────── */
    eret

/*
 * task_trampoline_user - 用户进程首次调度时的跳转板
 *
 * 当 arch_task_switch "返回"到这个函数时：
 *   x19 = user_entry  （用户态入口点）
 *   x20 = user_sp     （用户栈指针）
 *
 * 调用 arch_switch_to_user(user_entry, user_sp, kernel_sp)。
 *
 * 注意：不在此处开中断。内核态（EL1）始终关中断；用户态中断由
 * arch_switch_to_user 中 SPSR_EL1=0x340（I=0）经 eret 原子恢复，
 * 避免在 eret 之前于 EL1 留下 IRQ 窗口。
 */
.global task_trampoline_user
task_trampoline_user:
    /* ── 准备参数并调用 arch_switch_to_user ──────────────────────── */
    mov     x0, x19          /* x0 = user_entry */
    mov     x1, x20          /* x1 = user_sp */
    mov     x2, sp           /* x2 = kernel_sp (当前 sp) */

    /* 跳转到 arch_switch_to_user（不返回） */
    b       arch_switch_to_user

/*
 * arch_fork_resume_user - fork 子进程首次进入用户态
 *
 * 由 arch_init_fork_child_stack 设置，首次调度时通过 arch_task_switch 的
 * ret 跳转到此。
 *   x19 = 指向子进程 trap_frame_t 的内核虚拟地址
 *
 * 从 trap_frame_t 完整恢复所有用户寄存器，然后 eret 到用户态。
 * x0 已经在 arch_init_fork_child_stack 中被置为 0（fork 子进程返回值）。
 *
 * 注意：不在此处开中断。中断状态由 frame 中的 spsr（用户态 I=0）
 * 经 eret 原子恢复，避免在 eret 之前于 EL1 留下 IRQ 窗口。
 */
.global arch_fork_resume_user
arch_fork_resume_user:
    /* x19 = &trap_frame_t, 布局同 SAVE_REGS/RESTORE_REGS:
     *   [0*8]=x0, [1*8]=x1, ..., [30*8]=x30,
     *   [31*8]=usp, [32*8]=elr, [33*8]=spsr, [34*8]=tpidr_el0,
     *   [36*8..]=q0..q31（每 16 字节一个）, [100*8]=fpcr, [101*8]=fpsr
     */
    mov     x9, x19               /* x9 = frame pointer (x19会被下面覆盖) */

    /* 恢复 elr_el1, spsr_el1, sp_el0 */
    ldr     x10, [x9, #32*8]      /* elr */
    ldr     x11, [x9, #33*8]      /* spsr */
    ldr     x12, [x9, #31*8]      /* usp */
    msr     elr_el1,  x10
    msr     spsr_el1, x11
    msr     sp_el0,   x12

    /*
     * ── 恢复 FP/SIMD（q0-q31 + FPCR/FPSR）───────────────────────────
     * 子进程的 FP 状态来自 arch_init_fork_child_stack 拷贝的父进程陷阱帧
     * （fork 语义：子进程继承父进程的 FP 状态）。
     * 用 x10/x11 做临时寄存器：它们稍后会从帧里重新加载（#10*8 / #11*8）。
     */
    ldr     x10, [x9, #100*8]
    ldr     x11, [x9, #101*8]
    msr     fpcr, x10
    msr     fpsr, x11
    ldp     q0,  q1,  [x9, #36*8]
    ldp     q2,  q3,  [x9, #40*8]
    ldp     q4,  q5,  [x9, #44*8]
    ldp     q6,  q7,  [x9, #48*8]
    ldp     q8,  q9,  [x9, #52*8]
    ldp     q10, q11, [x9, #56*8]
    ldp     q12, q13, [x9, #60*8]
    ldp     q14, q15, [x9, #64*8]
    ldp     q16, q17, [x9, #68*8]
    ldp     q18, q19, [x9, #72*8]
    ldp     q20, q21, [x9, #76*8]
    ldp     q22, q23, [x9, #80*8]
    ldp     q24, q25, [x9, #84*8]
    ldp     q26, q27, [x9, #88*8]
    ldp     q28, q29, [x9, #92*8]
    ldp     q30, q31, [x9, #96*8]

    /* 恢复 x1-x30 */
    ldp     x1,  x2,  [x9, #1*8]
    ldp     x3,  x4,  [x9, #3*8]
    ldp     x5,  x6,  [x9, #5*8]
    ldp     x7,  x8,  [x9, #7*8]
    ldp     x10, x11, [x9, #10*8]
    ldp     x12, x13, [x9, #12*8]
    ldp     x14, x15, [x9, #14*8]
    ldp     x16, x17, [x9, #16*8]
    ldp     x18, x19, [x9, #18*8]
    ldp     x20, x21, [x9, #20*8]
    ldp     x22, x23, [x9, #22*8]
    ldp     x24, x25, [x9, #24*8]
    ldp     x26, x27, [x9, #26*8]
    ldp     x28, x29, [x9, #28*8]
    ldr     x30,      [x9, #30*8]

    /* 最后恢复 x0 (fork 返回 0，已在 frame 中设好) */
    ldr     x0, [x9, #0*8]

    /* 恢复 tpidr_el0 (线程 TLS 指针, 不用 x9 因为 x9 已用于 frame ptr) */
    ldr     x10, [x9, #34*8]
    msr     tpidr_el0, x10

    /*
     * 最后恢复 x9：本函数全程拿 x9 当帧指针，只能等所有帧访问结束后覆盖。
     * （原先漏了这一条，fork 子进程会带着内核地址 &trap_frame 回到 EL0——
     *  既是寄存器状态错误，也是把内核地址泄漏给用户态。）
     */
    ldr     x9, [x9, #9*8]

    eret
