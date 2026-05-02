/*
 * boot/x86_64/exception.S — x86_64 IDT 中断/异常入口存根
 *
 * 每个 IDT 向量对应一个存根（stub），负责：
 *   1. 压入 error_code（若 CPU 未自动压，则压 0 占位）
 *   2. 压入 vector 号
 *   3. 保存所有通用寄存器
 *   4. 调用 C 分发函数 handle_exception(trap_frame_t *)
 *   5. 恢复寄存器，iretq
 *
 * trap_frame_t 布局（栈从高到低）：
 *   [CPU 自动]  ss, rsp, rflags, cs, rip
 *   [可选]      error_code  (无则存根压 0)
 *   [存根压]    vector
 *   [存根压]    r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax
 *   <- 此时 rsp 指向 rax，即 trap_frame_t 的首地址
 */

.section .text
.code64
.extern handle_exception
.extern sched_check_and_yield

/* ── 宏：无错误码的存根 ─────────────────────────────────────── */
.macro ISR_NOERR vec
.global isr_stub_\vec
isr_stub_\vec:
    pushq   $0                  /* 压占位 error_code = 0 */
    pushq   $\vec               /* 压 vector 号          */
    jmp     isr_common_stub
.endm

/* ── 宏：有错误码的存根（CPU 已自动压 error_code）────────────── */
.macro ISR_ERR vec
.global isr_stub_\vec
isr_stub_\vec:
    pushq   $\vec               /* 压 vector 号（error_code 已在栈上） */
    jmp     isr_common_stub
.endm

/* ── CPU 异常（0–31）────────────────────────────────────────── */
ISR_NOERR  0    /* #DE Divide Error             */
ISR_NOERR  1    /* #DB Debug                    */
ISR_NOERR  2    /* NMI                          */
ISR_NOERR  3    /* #BP Breakpoint               */
ISR_NOERR  4    /* #OF Overflow                 */
ISR_NOERR  5    /* #BR Bound Range Exceeded     */
ISR_NOERR  6    /* #UD Invalid Opcode           */
ISR_NOERR  7    /* #NM Device Not Available     */
ISR_ERR    8    /* #DF Double Fault     (ec)    */
ISR_NOERR  9    /* Coprocessor Segment Overrun  */
ISR_ERR   10    /* #TS Invalid TSS      (ec)    */
ISR_ERR   11    /* #NP Segment Not Present(ec)  */
ISR_ERR   12    /* #SS Stack-Segment Fault(ec)  */
ISR_ERR   13    /* #GP General Protection (ec)  */
ISR_ERR   14    /* #PF Page Fault        (ec)   */
ISR_NOERR 15    /* Reserved                     */
ISR_NOERR 16    /* #MF x87 FPU Error            */
ISR_ERR   17    /* #AC Alignment Check   (ec)   */
ISR_NOERR 18    /* #MC Machine Check            */
ISR_NOERR 19    /* #XF SIMD FPU Error           */
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30    /* #SX Security Exception (ec)  */
ISR_NOERR 31

/* ── 外部中断（32–63）──────────────────────────────────────── */
ISR_NOERR 32    /* LAPIC Timer (IRQ0)           */
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/* ── Spurious（255）─────────────────────────────────────────── */
ISR_NOERR 255   /* LAPIC Spurious               */

/* ── 公共存根：保存寄存器，调用 C handler，恢复 ─────────────── */
.align 16
isr_common_stub:
    /* 保存通用寄存器（按 trap_frame_t 布局：rax 最后 push = 最低地址） */
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    /* 传递 trap_frame_t * 作为第一个参数（System V ABI: rdi） */
    movq    %rsp, %rdi
    call    handle_exception

    /* 检查是否需要重调度（IRQ 触发的抢占）──────────────────────── */
    call    sched_check_and_yield

    /* 恢复通用寄存器 */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax

    /* 弹掉 vector 和 error_code */
    addq    $16, %rsp

    iretq

/* ── ISR 地址表（供 exception.c 批量填充 IDT 使用）────────────
 * 注意：这里只导出被安装到 IDT 的向量，其余用默认 stub 覆盖。
 * 为了简洁，exception.c 直接用 extern 声明需要的 stub 地址。
 */
.global isr_stub_table
isr_stub_table:
    .quad isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3
    .quad isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7
    .quad isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11
    .quad isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15
    .quad isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19
    .quad isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23
    .quad isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27
    .quad isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31
    .quad isr_stub_32
