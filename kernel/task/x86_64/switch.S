/*
 * kernel/task/x86_64/switch.S — x86-64 内核上下文切换
 *
 * void arch_task_switch(uintptr_t *prev_sp, uintptr_t next_sp)
 *   rdi = &prev->sp  （输出：保存当前 RSP）
 *   rsi = next->sp   （输入：下一任务的 RSP）
 *
 * 保存被调用者保存寄存器 rbx, rbp, r12-r15（6 × pushq），
 * 将 RSP 写入 *rdi，加载 rsi 为新 RSP，
 * 弹出寄存器（r15,r14,r13,r12,rbp,rbx），ret。
 *
 * 保存帧布局（saved_rsp 起，偏移从低到高）：
 *   [0]  r15    [8]  r14
 *   [16] r13    [24] r12
 *   [32] rbp    [40] rbx
 *   [48] 返回地址  ← ret 弹出目标
 * 共 7 × 8 = 56 字节
 */

.section .text
.code64
.align  16
.global arch_task_switch

arch_task_switch:
    /* ── 保存被调用者保存寄存器 ──────────────────────────── */
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    /* ── 保存当前 RSP 到 *prev_sp（rdi）──────────────────── */
    movq    %rsp, (%rdi)

    /* ── 加载下一任务的 RSP（rsi）────────────────────────── */
    movq    %rsi, %rsp

    /* ── 恢复被调用者保存寄存器 ──────────────────────────── */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx

    /* ── 返回（从栈顶弹出返回地址并跳转）────────────────── */
    ret

/*
 * task_trampoline_user - 用户进程首次调度时的跳转板（x86-64）
 *
 * TODO: 实现 x86-64 用户态跳转
 * 目前为空存根，防止链接错误
 */
.global task_trampoline_user
task_trampoline_user:
1:  jmp     1b          /* 无限循环：尚未实现 */
