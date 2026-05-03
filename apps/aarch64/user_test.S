/*
 * kernel/task/user_test.S - 用户态测试程序（带系统调用）
 *
 * 测试系统调用功能：
 *   - SYS_WRITE: 输出字符串
 *   - SYS_GETPID: 获取进程 ID
 *   - SYS_YIELD: 让出 CPU
 *
 * 注意：字符串直接内嵌在代码中，避免数据段访问问题
 */

/* 系统调用号定义 */
#define SYS_EXIT          0
#define SYS_YIELD         1
#define SYS_GETPID        2
#define SYS_SLEEP         3
#define SYS_EXECVE        4
#define SYS_BRK          10
#define SYS_SBRK         11
#define SYS_WRITE        20
#define SYS_READ         21
#define SYS_OPEN         22
#define SYS_CLOSE        23
#define SYS_GETTIMEOFDAY 30

.section .text.user
.global user_test_program

user_test_program:
    /* === 用户程序入口 === */

    /* 输出欢迎消息 - 使用 adr 直接计算字符串地址（真正的 PC 相对） */
    adr     x0, msg_hello
    mov     x1, #31             /* 字符串长度 */
    mov     x8, #SYS_WRITE      /* SYS_WRITE = 20 */
    svc     #0                  /* 触发系统调用 */

    /* 测试 getpid 系统调用 */
    mov     x8, #SYS_GETPID     /* SYS_GETPID = 2 */
    svc     #0
    mov     x19, x0             /* 保存 PID 到 x19 */

    /* 输出 PID 信息 */
    adr     x0, msg_pid
    mov     x1, #20
    mov     x8, #SYS_WRITE
    svc     #0

    /* 输出 PID（简单转换） */
    mov     x0, x19
    add     x0, x0, #'0'
    adr     x1, msg_buf
    strb    w0, [x1]
    adr     x0, msg_buf
    mov     x1, #1
    mov     x8, #SYS_WRITE
    svc     #0

    /* 输出换行 */
    adr     x0, msg_newline
    mov     x1, #1
    mov     x8, #SYS_WRITE
    svc     #0

    /* === 测试 execve：从文件系统加载并执行 /init === */
    adr     x0, msg_exec
    mov     x1, #34
    mov     x8, #SYS_WRITE
    svc     #0

    /* 调用 execve("/init", NULL, NULL) */
    adr     x0, pathname        /* x0 = 程序路径 */
    mov     x1, #0              /* x1 = argv (暂未使用) */
    mov     x2, #0              /* x2 = envp (暂未使用) */
    mov     x8, #SYS_EXECVE     /* SYS_EXECVE = 4 */
    svc     #0                  /* 触发系统调用 */

    /* 如果 execve 失败，继续执行下面的代码 */
    adr     x0, msg_exec_failed
    mov     x1, #30
    mov     x8, #SYS_WRITE
    svc     #0

    /* 进入无限循环 */
    b       .

/* 字符串数据放在代码之后，确保被拷贝到用户空间 */
msg_hello:
    .asciz "[user] Hello from user space!\n"
msg_pid:
    .asciz "[user] Current PID: "
msg_exec:
    .asciz "[user] Executing: execve('/init')\n"
msg_exec_failed:
    .asciz "[user] ERROR: execve failed!\n"
msg_newline:
    .asciz "\n"
msg_buf:
    .skip   1
pathname:
    .asciz "/init"
    .align  3
