/*
 * apps/test_exec.S - 从文件系统加载的测试程序
 *
 * 这是一个简单的用户程序，用于测试 execve 系统调用。
 * 程序入口在 0x10000（虚拟地址）。
 */

/* 系统调用号定义 */
#define SYS_WRITE   0x4000
#define SYS_EXIT         0

.section .text
.global _start

_start:
    /* 输出欢迎消息 */
    adr     x0, msg_welcome
    mov     x1, #45
    mov     x8, #SYS_WRITE
    svc     #0

    /* 主循环 */
    mov     x19, #0
loop:
    /* 计数循环 */
    mov     x20, #10000
    mov     x21, #0
delay:
    add     x21, x21, #1
    cmp     x21, x20
    b.ne    delay

    /* 增加主计数器 */
    add     x19, x19, #1

    /* 输出消息 */
    adr     x0, msg_loop
    mov     x1, #40
    mov     x8, #SYS_WRITE
    svc     #0

    /* 让出 CPU */
    /* 暂时用汇编直接调用 yield */
    /* mov x8, #1 */
    /* svc #0 */

    /* 继续循环 */
    b       loop

    /* 退出 */
    mov     x0, #0
    mov     x8, #SYS_EXIT
    svc     #0

    /* 不应该到达这里 */
    b       .

/* 字符串数据 */
msg_welcome:
    .asciz "[test_exec] Program loaded from filesystem!\n"
msg_loop:
    .asciz "[test_exec] Running from filesystem...\n"
    .align  3
