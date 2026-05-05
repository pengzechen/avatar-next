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

/* 系统调用号定义（与 kernel/syscall/syscall.h 一致） */
#define SYS_EXIT          0
#define SYS_YIELD         1
#define SYS_GETPID        2
#define SYS_SLEEP         3
#define SYS_EXECVE        4
#define SYS_BRK          10
#define SYS_SBRK         11
#define SYS_WRITE        20
#define SYS_READ         21
#define SYS_OPEN         22
#define SYS_CLOSE        23
#define SYS_GETTIMEOFDAY 30

.section .text.user
.global user_test_program

user_test_program:
    /* === 用户程序入口（Ring 3） === */
    
    /* DEBUG: 如果能执行到这里，说明 IRETQ 成功了 */
    /* 由于 Ring 3 无法直接 OUT，我们只能通过系统调用来输出 */

    /* 输出欢迎消息 */
    leaq    msg_hello(%rip), %rdi   /* arg0 = 字符串地址 */
    movq    $31, %rsi               /* arg1 = 长度 */
    movq    $SYS_WRITE, %rax        /* syscall nr = 20 */
    syscall                         /* 触发 Ring 3 → Ring 0 */

    /* 测试 getpid 系统调用 */
    movq    $SYS_GETPID, %rax       /* syscall nr = 2 */
    syscall
    movq    %rax, %r12              /* 保存 PID 到 R12 */

    /* 输出 PID 信息 */
    leaq    msg_pid(%rip), %rdi
    movq    $20, %rsi
    movq    $SYS_WRITE, %rax
    syscall

    /* 输出 PID 数字（简单转换为 ASCII） */
    movq    %r12, %rax
    addq    $'0', %rax
    leaq    msg_buf(%rip), %rdi
    movb    %al, (%rdi)
    leaq    msg_buf(%rip), %rdi
    movq    $1, %rsi
    movq    $SYS_WRITE, %rax
    syscall

    /* 输出换行 */
    leaq    msg_newline(%rip), %rdi
    movq    $1, %rsi
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

    /* 输出 "[user_test] #" */
    leaq    msg_loop(%rip), %rdi
    movq    $14, %rsi               /* "[user_test] #\n" = 14 字节 */
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
