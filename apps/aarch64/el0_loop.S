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
    /* 纯自旋，不调用任何 syscall。
     * 内核调度器在时钟抢占时打印切换日志。 */
.Lloop:
    b    .Lloop
