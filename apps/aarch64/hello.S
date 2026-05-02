/*
 * apps/hello.S - 第二个用户态测试程序
 *
 * 与 user_test.S 不同，这个程序：
 *   - 输出不同的消息
 *   - 使用不同的计数逻辑
 *   - 用于测试多进程调度
 */

/* 系统调用号定义 */
#define SYS_EXIT          0
#define SYS_YIELD         1
#define SYS_WRITE        20

.section .text.user
.global hello_program

hello_program:
    /* === 用户程序入口 === */

    /* 输出欢迎消息 - 使用 adr 直接计算字符串地址（真正的 PC 相对） */
    adr     x0, msg_welcome
    mov     x1, #35             /* 字符串长度 */
    mov     x8, #SYS_WRITE
    svc     #0                  /* 触发系统调用 */

    /* 主循环：输出带计数器的消息 */
    mov     x19, #0             /* 计数器 */
1:
    /* 每 5000 次输出一次（与 user_test 相同频率） */
    mov     x20, #5000
    mov     x21, #0
2:
    add     x21, x21, #1
    cmp     x21, x20
    b.ne    2b

    /* 增加计数器 */
    add     x19, x19, #1

    /* 输出 "[hello] #" */
    adr     x0, msg_hello
    mov     x1, #10             /* "[hello] #" 长度 */
    mov     x8, #SYS_WRITE
    svc     #0

    /* 输出换行 */
    adr     x0, msg_newline
    mov     x1, #1
    mov     x8, #SYS_WRITE
    svc     #0

    /* 让出 CPU，给其他进程运行 */
    mov     x0, #0
    mov     x8, #SYS_YIELD
    svc     #0

    /* 继续循环 */
    b       1b

    /* 不应该到达这里 */
    b       .

/* 字符串数据放在代码之后，确保被拷贝到用户空间 */
msg_welcome:
    .asciz "[hello] Starting hello program...\n"
msg_hello:
    .asciz "[hello] #"
msg_newline:
    .asciz "\n"
    .align  3
