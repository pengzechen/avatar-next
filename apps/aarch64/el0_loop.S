/*
 * apps/aarch64/el0_loop.S — EL0 用户态无限 yield 循环
 *
 * 每 50 次 yield 打印一条消息，其余时间调 SYS_YIELD 让出 CPU。
 * 不依赖文件系统，不调用 execve。
 */

#define SYS_YIELD   1
#define SYS_WRITE  20

.section .text.user
.global el0_loop_program
.type el0_loop_program, @function
el0_loop_program:
    mov  x19, #0            /* 循环计数器 */

.Lloop:
    /* SYS_YIELD: 让出 CPU */
    mov  x8, #SYS_YIELD
    svc  #0

    add  x19, x19, #1

    /* 每 50 次打印一条消息 */
    mov  x0, #50
    udiv x1, x19, x0
    mul  x1, x1, x0
    cmp  x1, x19
    bne  .Lloop

    /* SYS_WRITE: 输出计数消息 */
    adr  x0, msg_loop
    mov  x1, #22
    mov  x8, #SYS_WRITE
    svc  #0

    b    .Lloop

msg_loop:
    .asciz "[el0] loop tick (EL0)\n"
    .align 2
