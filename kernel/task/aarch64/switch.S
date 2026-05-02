/*
 * kernel/task/aarch64/switch.S — AArch64 内核上下文切换
 *
 * void arch_task_switch(uintptr_t *prev_sp, uintptr_t next_sp,
 *                       uint64_t **prev_pgd_ptr, uint64_t *next_pgd)
 *   x0 = &prev->sp        （输出：保存当前 SP）
 *   x1 = next->sp         （输入：下一任务的 SP）
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

    /* ── 切换到下一任务的页表（如果 x3 != NULL）────────────────── */
    cbz     x3, 2f
    msr     ttbr0_el1, x3
    isb                         /* 同步上下文切换 */
    tlbi   vmalle1is            /* 刷新 TLB */
    dsb    sy
    isb
2:

    /* ── 加载下一任务的 SP（x1）──────────────────────────── */
    mov     sp, x1

    /* ── 恢复被调用者保存寄存器 ──────────────────────────── */
    ldp     x29, x30, [sp, #80]
    ldp     x27, x28, [sp, #64]
    ldp     x25, x26, [sp, #48]
    ldp     x23, x24, [sp, #32]
    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #96

    /* ── 返回（跳转到 x30 = 下一任务的返回地址）─────────── */
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
 * 我们需要：
 *   1. 开启中断
 *   2. 调用 arch_switch_to_user(user_entry, user_sp, kernel_sp)
 */
.global task_trampoline_user
task_trampoline_user:
    /* ── 开启中断──────────────────────────────────────────────── */
    msr     daifclr, #2

    /* ── 准备参数并调用 arch_switch_to_user ──────────────────────── */
    mov     x0, x19          /* x0 = user_entry */
    mov     x1, x20          /* x1 = user_sp */
    mov     x2, sp           /* x2 = kernel_sp (当前 sp) */

    /* 跳转到 arch_switch_to_user（不返回） */
    b       arch_switch_to_user
