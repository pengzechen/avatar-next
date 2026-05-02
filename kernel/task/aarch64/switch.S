/*
 * kernel/task/aarch64/switch.S — AArch64 内核上下文切换
 *
 * void arch_task_switch(uintptr_t *prev_sp, uintptr_t next_sp)
 *   x0 = &prev->sp  （输出：保存当前 SP）
 *   x1 = next->sp   （输入：下一任务的 SP）
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
