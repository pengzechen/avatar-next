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
 * 保存被调用者保存寄存器 x19-x30（12 × 8 = 96 字节），
 * 将 SP 写入 *x0，加载 x1 为新 SP，恢复寄存器，ret。
 *
 * 保存帧布局（saved_sp 起，偏移从低到高）：
 *   [0]   x19   [8]   x20
 *   [16]  x21   [24]  x22
 *   [32]  x23   [40]  x24
 *   [48]  x25   [56]  x26
 *   [64]  x27   [72]  x28
 *   [80]  x29   [88]  x30 ← ret 跳转目标
 */

.section .text
.align  4
.global arch_task_switch

arch_task_switch:
    /* ── 保存被调用者保存寄存器（96 字节）────────────────── */
    stp     x19, x20, [sp, #-96]!
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]

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
    ldp     x29, x30, [sp, #80]
    ldp     x27, x28, [sp, #64]
    ldp     x25, x26, [sp, #48]
    ldp     x23, x24, [sp, #32]
    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #96
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
     *   [31*8]=usp, [32*8]=elr, [33*8]=spsr
     */
    mov     x9, x19               /* x9 = frame pointer (x19会被下面覆盖) */

    /* 恢复 elr_el1, spsr_el1, sp_el0 */
    ldr     x10, [x9, #32*8]      /* elr */
    ldr     x11, [x9, #33*8]      /* spsr */
    ldr     x12, [x9, #31*8]      /* usp */
    msr     elr_el1,  x10
    msr     spsr_el1, x11
    msr     sp_el0,   x12

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

    eret
