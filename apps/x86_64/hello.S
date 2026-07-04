/*
 * apps/x86_64/hello.S - 第二个 x86_64 用户态测试程序（Ring 3）
 *
 * 与 user_test.S 不同，这个程序：
 *   - 输出不同的消息字符
 *   - 用于测试多进程调度
 *   - 不停输出 "[hello] *" 字符串
 *
 * x86_64 SYSCALL 调用约定：
 *   - RAX: 系统调用号
 *   - RDI, RSI, RDX, R10, R8, R9: 参数 0-5
 *   - 返回值: RAX
 */

/* 系统调用号定义 */
#define SYS_EXIT          0
#define SYS_YIELD         1
#define SYS_WRITE   0x4000
#define HELLO_USE_YIELD   0

.section .text.user
.global hello_program

#if APP_ELF
.global _start
_start:
#endif
hello_program:
    /* === 用户程序入口（Ring 3） === */

    /* 输出欢迎消息 */
    leaq    msg_welcome(%rip), %rdi /* arg0 = 字符串地址 */
    movq    $30, %rsi               /* arg1 = 长度 */
    movq    $SYS_WRITE, %rax        /* syscall nr = 20 */
    syscall                         /* 触发系统调用 */

    /* 主循环：输出带标记的消息 */
    xorq    %r13, %r13              /* 计数器 R13 = 0 */

loop:
    /* 等待一段时间（与 user_test 相同的延时） */
    movq    $5000, %rcx
delay:
    decq    %rcx
    jnz     delay

    /* 增加计数器 */
    incq    %r13

    /* 输出 "[hello] *\n" */
    leaq    msg_hello(%rip), %rdi
    movq    $10, %rsi               /* "[hello] *\n" = 10 字节 */
    movq    $SYS_WRITE, %rax
    syscall

    /* 让出 CPU（测试调度，可开关） */
#if HELLO_USE_YIELD
    movq    $SYS_YIELD, %rax
    syscall
#endif

    /* 继续循环 */
    jmp     loop

    /* 永远不应该到达这里 */
exit:
    movq    $SYS_EXIT, %rax
    xorq    %rdi, %rdi              /* exit code = 0 */
    syscall
    
    /* 死循环（以防万一） */
1:  jmp     1b

/* === 数据：字符串常量（嵌入代码段） === */

msg_welcome:
    .ascii "[hello] Starting in Ring 3...\n"

msg_hello:
    .ascii "[hello] *\n"

/* 用户程序结束标记 */
.global hello_program_end
hello_program_end:
    nop
