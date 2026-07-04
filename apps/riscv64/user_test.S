/*
 * apps/riscv64/user_test.S - 用户态测试程序（RISC-V 64 位）
 *
 * 测试系统调用功能：
 *   - SYS_WRITE (20): 输出字符串
 *   - SYS_GETPID (2):  获取进程 ID
 *   - SYS_EXIT  (0):   退出
 *
 * RISC-V 系统调用约定：
 *   a7 = 系统调用号
 *   a0-a5 = 参数
 *   a0 = 返回值
 *   ecall = 触发系统调用
 *
 * 注意：
 *   .option norelax  防止链接器将 la 放松为 gp 相对寻址；
 *   这样 auipc+addi 保持 PC 相对，代码被复制到用户 VA 0x10000 后
 *   依然能正确寻址紧跟代码的字符串数据。
 */

/* 自定义系统调用号（与 kernel/syscall/syscall.h 一致） */
#define SYS_EXIT    0
#define SYS_YIELD   1
#define SYS_GETPID  2
#define SYS_WRITE   0x4000

.section .text.user
.global user_test_program

user_test_program:
    /* 禁止链接器将 la 放松为 gp 相对寻址 */
    .option push
    .option norelax

    /* ── 第一步：输出欢迎消息 ─────────────────────────────── */
    la      a0, msg_hello           /* a0 = 字符串指针（PC 相对） */
    li      a1, 34                  /* a1 = 字节数（不含 NUL）  */
    li      a7, SYS_WRITE
    ecall

    /* ── 第二步：获取 PID ────────────────────────────────── */
    li      a7, SYS_GETPID
    ecall
    mv      s0, a0                  /* 保存 PID */

    /* ── 第三步：输出 "PID: " 前缀 ─────────────────────── */
    la      a0, msg_pid
    li      a1, 15
    li      a7, SYS_WRITE
    ecall

    /* ── 第四步：将 PID 转为 ASCII 并写入缓冲 ──────────── */
    /* 仅支持 PID 0-9（调试用，内核进程号极小） */
    la      a1, msg_buf
    addi    a0, s0, '0'
    sb      a0, 0(a1)

    la      a0, msg_buf
    li      a1, 1
    li      a7, SYS_WRITE
    ecall

    /* ── 第五步：换行 ────────────────────────────────────── */
    la      a0, msg_newline
    li      a1, 1
    li      a7, SYS_WRITE
    ecall

    /* ── 第六步：输出退出消息 ────────────────────────────── */
    la      a0, msg_exit
    li      a1, 21
    li      a7, SYS_WRITE
    ecall

    /* ── 第七步：退出 ────────────────────────────────────── */
    li      a0, 0                   /* 退出码 = 0 */
    li      a7, SYS_EXIT
    ecall

    /* 不应到达此处 */
1:  j       1b

    .option pop

/* ── 字符串数据（紧跟代码，确保被完整复制到用户空间）──── */

msg_hello:
    /* "[rv-user] Hello from RISC-V user!\n" = 34 bytes */
    .ascii  "[rv-user] Hello from RISC-V user!\n"

msg_pid:
    /* "[rv-user] PID: " = 15 bytes */
    .ascii  "[rv-user] PID: "

msg_newline:
    .byte   '\n'

msg_exit:
    /* "[rv-user] Exiting...\n" = 21 bytes */
    .ascii  "[rv-user] Exiting...\n"

msg_buf:
    .byte   '?', '\n'

    .align  3
