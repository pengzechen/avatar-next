/*
 * apps/loop_test.S - 简单的用户态循环测试（RISC-V）
 * 用于验证定时器中断和任务切换
 */

.section .text
.global _start

_start:
    /* 初始化计数器 */
    li s0, 0
    
loop:
    /* 增加计数器 */
    addi s0, s0, 1
    
    /* 每 1000000 次循环打印一个字符 */
    li t0, 1000000
    remu t1, s0, t0
    bnez t1, skip_print
    
    /* 打印字符 */
    li a7, 64              /* LINUX_SYS_WRITE */
    li a0, 1               /* stdout */
    la a1, msg             /* buffer */
    li a2, 1               /* length */
    ecall
    
skip_print:
    j loop                 /* 无限循环 */

.section .data
msg:
    .byte 'A'
