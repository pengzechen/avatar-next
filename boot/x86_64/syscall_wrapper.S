/*
 * boot/x86_64/syscall_wrapper.S — x86_64 快速系统调用入口
 *
 * 使用 SYSCALL/SYSRET 指令（比 INT 0x80 快得多）
 *
 * 寄存器约定（System V AMD64 ABI）:
 *   RAX: 系统调用号
 *   RDI, RSI, RDX, R10, R8, R9: 参数 1-6
 *   返回值: RAX
 *
 * SYSCALL 指令行为（CPU 自动）:
 *   RCX ← RIP（返回地址）
 *   R11 ← RFLAGS
 *   CS  ← MSR_STAR[47:32]（内核代码段）
 *   SS  ← MSR_STAR[47:32] + 8（内核数据段）
 *   RFLAGS &= ~MSR_FMASK（清除中断标志）
 *   RIP ← MSR_LSTAR（此函数地址）
 *
 * SYSRET 指令行为（CPU 自动）:
 *   RIP ← RCX
 *   RFLAGS ← R11
 *   CS  ← MSR_STAR[63:48] + 16（用户代码段 + RPL=3）
 *   SS  ← MSR_STAR[63:48] + 8（用户数据段 + RPL=3）
 *
 * trap_frame_t 布局（从高到低）:
 *   用户 SS, RSP, RFLAGS, CS, RIP  （SYSRET 需要）
 *   error_code=0, vector=0x80       （标识为系统调用）
 *   R15, R14, R13, R12, R11, R10, R9, R8
 *   RBP, RDI, RSI, RDX, RCX, RBX, RAX
 */

.section .text
.code64
.global syscall_entry
.extern syscall_handler
.extern g_current_task
.extern g_x86_tss_rsp0

.section .bss
.align 8
syscall_user_rsp_tmp:
    .quad 0

.section .text

syscall_entry:
    /*
     * 1. 保存用户上下文
     *    此时 RCX=用户RIP, R11=用户RFLAGS, RSP=用户栈
     */
    
    /* 切换到内核栈（使用当前任务的内核栈） */
    swapgs                          /* GS ← 内核 GS */
    
    /* 暂存用户 RSP 到内存，避免破坏用户 r15 */
    movq    %rsp, syscall_user_rsp_tmp(%rip)
    
    /* 使用调度器维护的 TSS.RSP0（每任务独立内核栈顶） */
    movq    g_x86_tss_rsp0(%rip), %rsp
    
    /*
     * 2. 构建 trap_frame_t（从低到高压栈）
     */
    
    /* 保存用户段和栈指针（SYSRET 需要） */
    pushq   $0x23                   /* SS: 用户数据段 (GDT[4] + RPL=3) */
    pushq   syscall_user_rsp_tmp(%rip) /* RSP: 用户栈指针 */
    pushq   %r11                    /* RFLAGS: 保存的用户标志 */
    pushq   $0x2B                   /* CS: 用户代码段 (GDT[5] + RPL=3) */
    pushq   %rcx                    /* RIP: 返回地址 */
    
    /* 保存 error_code 和 vector（标识为系统调用） */
    pushq   $0                      /* error_code = 0 */
    pushq   $0x80                   /* vector = 0x80（系统调用魔数） */
    
    /* 保存通用寄存器（顺序与 trap_frame_t 一致） */
    pushq   %rax                    /* 系统调用号（也是返回值） */
    pushq   %rbx
    pushq   %rcx                    /* 原始 RCX（被 SYSCALL 覆盖，重复保存） */
    pushq   %rdx                    /* 参数3 */
    pushq   %rsi                    /* 参数2 */
    pushq   %rdi                    /* 参数1 */
    pushq   %rbp
    pushq   %r8                     /* 参数5 */
    pushq   %r9                     /* 参数6 */
    pushq   %r10                    /* 参数4（注意：SYSCALL 约定用 R10 代替 RCX） */
    pushq   %r11                    /* 原始 R11（被 SYSCALL 覆盖，重复保存） */
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    
    /*
     * 3. 调用 C 系统调用处理函数
     */
    movq    %rsp, %rdi              /* 第一个参数：trap_frame_t * */
    call    syscall_handler
    
    /* 返回值已在 RAX 中，会被恢复到用户态 */
    
    /*
     * 4. 恢复用户上下文
     */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11                    /* 恢复原始 R11 */
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx                    /* 恢复原始 RCX */
    popq    %rbx
    popq    %rax                    /* 返回值（已被 syscall_handler 设置） */
    
    /* 跳过 vector 和 error_code */
    addq    $16, %rsp
    
    /* 从栈恢复 SYSRET 需要的寄存器 */
    popq    %rcx                    /* RCX ← 用户 RIP */
    addq    $8, %rsp                /* 跳过 CS（SYSRET 自动设置） */
    popq    %r11                    /* R11 ← 用户 RFLAGS */
    /* 用户 RSP 在栈顶，直接装载到 RSP；SS 由 SYSRET 自动设置 */
    movq    (%rsp), %rsp
    
    /* 恢复用户 GS */
    swapgs
    
    /*
     * 5. 返回用户态
     */
    sysretq
    
    /* 不会到达这里 */
    ud2

