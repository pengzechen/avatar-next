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
 * 调用约定（来自 task_create）:
 *   栈顶存放: [user_entry, user_sp, 0 (返回地址占位)]
 *
 * 使用 iretq 切换到 Ring 3（用户态）:
 *   1. 准备 iretq 栈帧（SS, RSP, RFLAGS, CS, RIP）
 *   2. 清空通用寄存器（安全）
 *   3. 执行 iretq 跳转到用户态
 *
 * 段选择子（from boot.S GDT）:
 *   0x2B: 用户代码段（0x28 + RPL=3）
 *   0x23: 用户数据段（0x20 + RPL=3）
 */
.global task_trampoline_user
task_trampoline_user:
    /* DEBUG: 输出 'T' 到 COM1 (0x3F8)，证明到达这里 */
    // movb    $'T', %al
    // movw    $0x3F8, %dx
    // outb    %al, %dx
    
    /* 从栈获取用户入口和栈指针（arch_task_switch 压入） */
    popq    %rdi                /* user_entry（用户入口点）*/
    popq    %rsi                /* user_sp（用户栈指针）*/
    
    /* DEBUG: 输出 'R' 表示准备执行 IRETQ */
    // movb    $'R', %al
    // movw    $0x3F8, %dx
    // outb    %al, %dx
    
    /* 保存用户入口和栈到非易失寄存器（便于调试） */
    movq    %rdi, %r12          /* user_entry */
    movq    %rsi, %r13          /* user_sp */
    
    /* 验证 CR3 和 PML4[0] */
    movq    %cr3, %rax
    andq    $-4096, %rax        /* 提取 PML4 物理地址（清除低 12 位）*/
    movabsq $0xFFFF800000000000, %rbx
    addq    %rbx, %rax          /* 转换为虚拟地址 */
    movq    (%rax), %rdx        /* 读取 PML4[0] */
    
    /* 检查 PML4[0] 是否存在（Present bit） */
    testq   $1, %rdx
    jz      .Lpml4_missing
    
    /* 输出 'P' - PML4[0] 存在 */
    // movb    $'P', %al
    // movw    $0x3F8, %dx
    // outb    %al, %dx
    jmp     .Lpml4_ok
    
.Lpml4_missing:
    /* PML4[0] 不存在！输出 'X' 并挂起 */
    // movb    $'X', %al
    // movw    $0x3F8, %dx
    // outb    %al, %dx
    cli
1:  hlt
    jmp     1b
    
.Lpml4_ok:
    /* 准备 iretq 栈帧（从高到低：SS, RSP, RFLAGS, CS, RIP）*/
    pushq   $0x23               /* SS: 用户数据段（0x20 + RPL=3）*/
    pushq   %r13                /* RSP: 用户栈指针 */
    
    /* 设置 RFLAGS: 设置 IF=1, IOPL=0, 清除 VM/RF/NT, 其他位清零以确保干净状态 */
    movq    $0x202, %rax        /* IF=1 (bit 9), Reserved=1 (bit 1) */
    pushq   %rax                /* RFLAGS */
    
    pushq   $0x2B               /* CS: 用户代码段（0x28 + RPL=3）*/
    pushq   %r12                /* RIP: 用户入口点 */
    
    /* DEBUG: 输出栈帧内容前 5 个 QWORD */
    // movq    %rsp, %rbx
    // movb    $'F', %al           /* 'F' = Frame dump */
    // movw    $0x3F8, %dx
    // outb    %al, %dx
    
    /* 输出 RIP (栈顶) */
    // movq    (%rbx), %rax
    // movb    $':', %al
    // outb    %al, %dx
    
    /* DEBUG: 输出 'Q' 表示即将执行 IRETQ */
    // movb    $'Q', %al
    // movw    $0x3F8, %dx
    // outb    %al, %dx
    
    /* 清空所有通用寄存器（防止信息泄露）*/
    xorq    %rax, %rax
    xorq    %rbx, %rbx
    xorq    %rcx, %rcx
    xorq    %rdx, %rdx
    xorq    %rsi, %rsi
    xorq    %rdi, %rdi
    xorq    %rbp, %rbp
    xorq    %r8,  %r8
    xorq    %r9,  %r9
    xorq    %r10, %r10
    xorq    %r11, %r11
    xorq    %r12, %r12
    xorq    %r13, %r13
    xorq    %r14, %r14
    xorq    %r15, %r15
    
    /* 跳转到用户态（Ring 3）*/
    iretq
    
    /* 不会到达这里 */
    ud2
