/*
 * apps/user_hello.S - 最简单的 RISC-V 用户态测试程序
 *
 * 测试：
 *   1. write(1, "Hello\n", 6)  - 写字符串
 *   2. exit(0)                 - 正常退出
 */

.section .text
.global _start

_start:
    /* write(1, "Hello\n", 6) - syscall nr=64 */
    li      a7, 64          /* sys_write */
    li      a0, 1           /* fd = stdout */
    la      a1, msg         /* buf = msg */
    li      a2, 6           /* count = 6 */
    ecall

    /* exit(0) - syscall nr=93 */
    li      a7, 93          /* sys_exit */
    li      a0, 0           /* status = 0 */
    ecall

    /* 永远不应该到达这里 */
1:  j       1b

.section .rodata
msg:
    .ascii "Hello\n"
