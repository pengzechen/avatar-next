/*
 * apps/x86_64/test_execve.S - 测试 execve 系统调用
 *
 * 这个程序测试 execve 系统调用，尝试执行 /hello
 *
 * x86_64 SYSCALL 调用约定：
 *   - RAX: 系统调用号
 *   - RDI, RSI, RDX, R10, R8, R9: 参数 0-5
 *   - 返回值: RAX
 */

/* 系统调用号定义（与 kernel/syscall/syscall.h 保持一致） */
#define SYS_EXIT          0
#define SYS_WRITE        20
#define SYS_EXECVE        4  /* execve 系统调用 */

.section .text.user
.global test_execve_program

#if APP_ELF
.global _start
_start:
#endif
test_execve_program:
    /* === 用户程序入口（Ring 3） === */

    /* 输出开始消息 */
    leaq    msg_start(%rip), %rdi    /* arg0 = 字符串地址 */
    movq    $43, %rsi                /* arg1 = 长度 */
    movq    $SYS_WRITE, %rax         /* syscall nr = 20 */
    syscall                          /* 触发系统调用 */

    /* === 准备 execve 参数 === */
    /* execve(pathname, argv, envp) */
    /* pathname = "/hello\0" */
    leaq    pathname(%rip), %rdi     /* arg0 = pathname */

    /* argv = [pathname, NULL] */
    leaq    argv(%rip), %rsi         /* arg1 = argv */

    /* envp = NULL */
    xorq    %rdx, %rdx               /* arg2 = envp = NULL */

    /* 调用 execve */
    movq    $SYS_EXECVE, %rax        /* syscall nr = 4 */
    syscall

    /* 检查返回值 */
    testq   %rax, %rax
    jge     exec_success             /* 如果返回值 >= 0，成功 */

    /* execve 失败：先保存 errno，再输出错误消息 */
    movq    %rax, %rbx               /* 保存负错误码（如 -ENOENT） */
    leaq    msg_error(%rip), %rdi    /* arg0 = 错误消息地址 */
    movq    $msg_error_len, %rsi     /* arg1 = 消息长度 */
    movq    $SYS_WRITE, %rax
    syscall

    /* 退出（错误码） */
exit_error:
    movq    %rbx, %rdi               /* exit code = execve errno */
    movq    $SYS_EXIT, %rax
    syscall

exec_success:
    /* execve 成功，这里不应该到达 */
    /* 如果到达了说明 execve 失败了 */
    leaq    msg_weird(%rip), %rdi
    movq    $40, %rsi
    movq    $SYS_WRITE, %rax
    syscall

    /* 正常退出 */
    movq    $SYS_EXIT, %rax
    xorq    %rdi, %rdi               /* exit code = 0 */
    syscall

    /* 死循环（以防万一） */
1:  jmp     1b

/* === 数据区 === */

pathname:
    .ascii "/hello\0"

/* argv 数组：[pathname, NULL] */
argv:
    .quad pathname
    .quad 0

msg_start:
    .ascii "[test_execve] Attempting to exec /hello...\n"

msg_error:
    .ascii "[test_execve] execve failed! Error code: \n"
msg_error_end:
.equ msg_error_len, msg_error_end - msg_error

msg_weird:
    .ascii "[test_execve] Returned from execve? (shouldn't happen)\n"

/* 用户程序结束标记 */
.global test_execve_program_end
test_execve_program_end:
    nop
