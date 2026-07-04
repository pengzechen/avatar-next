/*
 * apps/riscv64/hello.S - 简单用户态 Hello 程序（RISC-V 64 位）
 *
 * 最小化测试：只做一次 write + exit，验证 ecall 最短路径可用。
 */

#define SYS_EXIT    0
#define SYS_WRITE   0x4000

.section .text.user
.global hello_program

hello_program:
    .option push
    .option norelax

    /* write(msg, len) */
    la      a0, hello_msg
    li      a1, 19              /* "[rv] Hello, World!\n" = 19 bytes */
    li      a7, SYS_WRITE
    ecall

    /* exit(0) */
    li      a0, 0
    li      a7, SYS_EXIT
    ecall

1:  j       1b

    .option pop

hello_msg:
    .ascii  "[rv] Hello, World!\n"

    .align  3
