/*
 * kernel/task/riscv64/switch.S — RISC-V 64-bit 内核上下文切换
 *
 * void arch_task_switch(uintptr_t *prev_sp, uintptr_t next_sp)
 *   a0 = &prev->sp  （输出：保存当前 SP）
 *   a1 = next->sp   （输入：下一任务的 SP）
 *
 * 保存被调用者保存寄存器 ra + s0-s11（13 × 8 = 104 字节），
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
 */

.section .text
.align  2
.global arch_task_switch

arch_task_switch:
    /* ── 分配帧（13 × 8 = 104 字节）─────────────────────── */
    addi    sp,  sp,  -104

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

    /* ── 保存当前 SP 到 *prev_sp（a0）────────────────────── */
    sd      sp, 0(a0)

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

    addi    sp,   sp,  104

    /* ── 返回（跳转到 ra = 下一任务的返回地址）─────────── */
    ret

/*
 * task_trampoline_user - 用户进程首次调度时的跳转板（RISC-V 64）
 *
 * TODO: 实现 RISC-V 用户态跳转
 * 目前为空存根，防止链接错误
 */
.global task_trampoline_user
task_trampoline_user:
1:  j       1b          /* 无限循环：尚未实现 */
