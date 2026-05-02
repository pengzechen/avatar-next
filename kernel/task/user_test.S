/*
 * kernel/task/user_test.S - 简单的用户态测试程序
 *
 * 这是一个运行在用户态（EL0）的简单程序，
 * 用于测试内核到用户态的跳转。
 *
 * 程序功能：
 *   1. 执行一些简单的计算
 *   2. 通过系统调用输出结果（后续实现）
 *   3. 无限循环
 */

/*
 * kernel/task/user_test.S - 用户态测试程序（带系统调用）
 *
 * 测试系统调用功能：
 *   - SYS_WRITE: 输出字符串
 *   - SYS_YIELD: 让出 CPU
 *   - SYS_EXIT: 退出进程
 *
 * 注意：字符串直接内嵌在代码中，避免数据段访问问题
 */

.section .text.user
.global user_test_program

user_test_program:
    /* === 用户程序入口 === */

    /* 输出欢迎消息 - 使用 adr 直接计算字符串地址（真正的 PC 相对） */
    adr     x0, msg_hello
    mov     x1, #30             /* 字符串长度 */
    mov     x8, #0              /* SYS_WRITE */
    svc     #0                  /* 触发系统调用 */

    /* 主循环：输出带进程标识的消息 */
    mov     x19, #0             /* 计数器 */
1:
    /* 每 5000 次输出一次 */
    mov     x20, #5000
    mov     x21, #0
2:
    add     x21, x21, #1
    cmp     x21, x20
    b.ne    2b

    /* 增加计数器 */
    add     x19, x19, #1

    /* 输出 "[user] #" */
    adr     x0, msg_user
    mov     x1, #8
    mov     x8, #0              /* SYS_WRITE */
    svc     #0

    /* 输出换行 */
    adr     x0, msg_newline
    mov     x1, #1
    mov     x8, #0              /* SYS_WRITE */
    svc     #0

    /* 主动让出 CPU */
    mov     x0, #0
    mov     x8, #2              /* SYS_YIELD */
    svc     #0

    /* 继续循环 */
    b       1b

    /* 不应该到达这里 */
    b       .

/* 字符串数据放在代码之后，确保被拷贝到用户空间 */
msg_hello:
    .asciz "[user] Hello from user space!\n"
msg_user:
    .asciz "[user] #"
msg_newline:
    .asciz "\n"
    .align  3
