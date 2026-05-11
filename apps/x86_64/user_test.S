/*
 * apps/x86_64/user_test.S - x86_64 用户态测试程序（Ring 3）
 *
 * 测试：
 *   1. SYSCALL 指令能否正常工作
 *   2. Ring 3 → Ring 0 → Ring 3 切换
 *   3. 系统调用：SYS_WRITE, SYS_GETPID, SYS_YIELD
 *
 * x86_64 SYSCALL 调用约定：
 *   - RAX: 系统调用号
 *   - RDI, RSI, RDX, R10, R8, R9: 参数 0-5
 *   - 返回值: RAX
 */

/* Linux x86_64 系统调用号（与内核 x86_translate_syscall 映射一致） */
#define SYS_WRITE        1   /* write(fd, buf, count) → fd=1 for stdout */
#define SYS_GETPID       39  /* getpid() */
#define SYS_YIELD        24  /* sched_yield() */
#define SYS_EXIT         60  /* exit(code) */

.section .text.user
.global user_test_program

#if APP_ELF
.global _start
_start:
#endif
user_test_program:
    /* === 用户程序入口（Ring 3） === */
    
    /* DEBUG: 如果能执行到这里，说明 IRETQ 成功了 */
    /* 由于 Ring 3 无法直接 OUT，我们只能通过系统调用来输出 */

    /* 输出欢迎消息 */
    /* write(1, msg_hello, 31) */
    movq    $1, %rdi                /* fd = 1 (stdout) */
    leaq    msg_hello(%rip), %rsi   /* buf */
    movq    $31, %rdx               /* count */
    movq    $SYS_WRITE, %rax
    syscall                         /* 触发 Ring 3 → Ring 0 */

    /* 测试 getpid 系统调用 */
    movq    $SYS_GETPID, %rax
    syscall
    movq    %rax, %r12              /* 保存 PID 到 R12 */

    /* 输出 PID 信息：write(1, msg_pid, 20) */
    movq    $1, %rdi
    leaq    msg_pid(%rip), %rsi
    movq    $20, %rdx
    movq    $SYS_WRITE, %rax
    syscall

    /* 输出 PID 数字（简单转换为 ASCII）*/
    movq    %r12, %rax
    addq    $'0', %rax
    leaq    msg_buf(%rip), %rsi
    movb    %al, (%rsi)
    movq    $1, %rdi
    movq    $1, %rdx
    movq    $SYS_WRITE, %rax
    syscall

    /* 输出换行：write(1, msg_newline, 1) */
    movq    $1, %rdi
    leaq    msg_newline(%rip), %rsi
    movq    $1, %rdx
    movq    $SYS_WRITE, %rax
    syscall

    /* === 主循环：定期输出消息 === */
    xorq    %r13, %r13              /* 计数器 R13 = 0 */

loop:
    /* 等待一段时间（空循环延时） */
    movq    $900000, %rcx
delay:
    decq    %rcx
    jnz     delay

    /* 增加计数器 */
    incq    %r13

    /* 输出 "[user_test] #\n"：write(1, msg_loop, 14) */
    movq    $1, %rdi
    leaq    msg_loop(%rip), %rsi
    movq    $14, %rdx
    movq    $SYS_WRITE, %rax
    syscall

    /* 让出 CPU（测试调度） */
    movq    $SYS_YIELD, %rax
    syscall

    /* 继续循环 */
    jmp     loop

    /* 永远不应该到达这里 */
exit:
    movq    $SYS_EXIT, %rax
    xorq    %rdi, %rdi              /* exit code = 0 */
    syscall

    /* 死循环（以防万一） */
1:  jmp     1b

/* === 数据：字符串常量（嵌入代码段，确保复制时不丢失） === */

msg_hello:
    .ascii "[user_test] Starting in Ring 3\n"

msg_pid:
    .ascii "[user_test] My PID: "

msg_buf:
    .byte 0

msg_newline:
    .ascii "\n"

msg_loop:
    .ascii "[user_test] #\n"

/* 用户程序结束标记 */
.global user_test_program_end
user_test_program_end:
    nop
