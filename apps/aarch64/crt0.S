/*
 * apps/aarch64/crt0.S - AArch64 C 程序最小运行时启动
 *
 * 内核 ELF 加载器按照 Linux AArch64 ABI 设置初始栈：
 *
 *   sp -> [argc]           (8 bytes)
 *         [argv[0]]        (8 bytes)
 *         ...
 *         [argv[argc-1]]
 *         [NULL]           (argv 终止)
 *         [envp[0]]
 *         ...
 *         [NULL]           (envp 终止)
 *         [auxv pairs...]
 *
 * musl libc 的 _start 期望：x0 = sp（同 Linux 规范）
 * 对于我们自己的 C init，同样读取 sp 来获取 argc/argv/envp。
 */

.section .text
.global _start
_start:
    /* x0 = sp（Linux AArch64 ABI：_start 时 x0 未定义，sp 指向 argc） */
    /* 某些 musl 版本把 sp 作为 x0 传入，我们主动读取 sp */
    mov     x0, sp

    /* 从栈上解析 argc, argv, envp */
    ldr     x19, [sp]           /* argc */
    add     x20, sp, #8         /* argv = sp + 8 */
    /* envp = argv + (argc + 1) * 8 */
    add     x21, x19, #1
    lsl     x21, x21, #3
    add     x21, x20, x21       /* envp */

    /* 调用 main(argc, argv, envp) */
    mov     x0, x19
    mov     x1, x20
    mov     x2, x21
    bl      main

    /* main 返回后调用 exit(返回值) */
    /* x0 已经是 main 的返回值 */
    mov     x8, #0              /* SYS_EXIT = 0 */
    svc     #0

    /* 不应到达这里 */
1:  b       1b
