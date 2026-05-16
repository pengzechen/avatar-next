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

/*
 * arch_fork_resume_user - fork 子进程首次进入用户态（x86_64）
 *
 * 由 arch_task_switch 的 ret 跳转到此处。
 * 约定：rbx = trap_frame_t*（arch_init_fork_child_stack 中设置）
 *
 * trap_frame_t 偏移（见 include/x86_64/exception.h）:
 *   r15=0x00  r14=0x08  r13=0x10  r12=0x18
 *   r11=0x20  r10=0x28  r9=0x30   r8=0x38
 *   rbp=0x40  rdi=0x48  rsi=0x50  rdx=0x58
 *   rcx=0x60  rbx=0x68  rax=0x70
 *   vector=0x78  error_code=0x80
 *   rip=0x88  cs=0x90  rflags=0x98  rsp=0xa0  ss=0xa8
 */
.global arch_fork_resume_user
arch_fork_resume_user:
    /* 从 trap_frame 构建 iretq 栈帧 */
    pushq   0xa8(%rbx)          /* SS */
    pushq   0xa0(%rbx)          /* RSP（用户栈指针）*/
    pushq   0x98(%rbx)          /* RFLAGS */
    pushq   0x90(%rbx)          /* CS */
    pushq   0x88(%rbx)          /* RIP（用户返回地址）*/

    /* 恢复通用寄存器（最后恢复 rbx 因为它是帧指针）*/
    movq    0x00(%rbx), %r15
    movq    0x08(%rbx), %r14
    movq    0x10(%rbx), %r13
    movq    0x18(%rbx), %r12
    movq    0x20(%rbx), %r11
    movq    0x28(%rbx), %r10
    movq    0x30(%rbx), %r9
    movq    0x38(%rbx), %r8
    movq    0x40(%rbx), %rbp
    movq    0x48(%rbx), %rdi
    movq    0x50(%rbx), %rsi
    movq    0x58(%rbx), %rdx
    movq    0x60(%rbx), %rcx
    /* fork 子进程 rax = 0 */
    xorq    %rax, %rax
    movq    0x68(%rbx), %rbx    /* 最后恢复 rbx */

    iretq
    ud2
