/*
 * lib/syscall.S - 用户空间系统调用汇编包装（AArch64）
 *
 * 提供 C 语言可调用的系统调用接口
 */
#ifdef __aarch64__

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

.global _syscall_wrapper

/*
 * _syscall_wrapper - 通用系统调用包装
 * x0 = 系统调用号
 * x1-x5 = 参数
 * 返回值在 x0
 */
_syscall_wrapper:
    mov     x8, x0          /* 将系统调用号移入 x8 */
    mov     x0, x1          /* 第一个参数 */
    mov     x1, x2          /* 第二个参数 */
    mov     x2, x3          /* 第三个参数 */
    mov     x3, x4          /* 第四个参数 */
    mov     x4, x5          /* 第五个参数 */
    svc     #0              /* 触发系统调用 */
    ret

/* ── 进程管理 ───────────────────────────────────────────────────── */

.global exit
exit:
    mov     x8, #SYS_EXIT
    svc     #0
    /* exit 不应该返回 */
    /* 如果返回，无限循环 */
1:  b       1b

.global getpid
getpid:
    mov     x8, #SYS_GETPID
    svc     #0
    ret

.global sleep
sleep:
    mov     x8, #SYS_SLEEP
    svc     #0
    ret

.global execve
execve:
    mov     x8, #SYS_EXECVE
    svc     #0
    ret

/* ── 内存管理 ───────────────────────────────────────────────────── */

.global brk
brk:
    mov     x8, #SYS_BRK
    svc     #0
    ret

.global sbrk
sbrk:
    mov     x8, #SYS_SBRK
    svc     #0
    ret

/* ── 文件操作 ───────────────────────────────────────────────────── */

.global write
write:
    mov     x8, #SYS_WRITE
    svc     #0
    ret

.global read
read:
    mov     x8, #SYS_READ
    svc     #0
    ret

.global open
open:
    mov     x8, #SYS_OPEN
    svc     #0
    ret

.global close
close:
    mov     x8, #SYS_CLOSE
    svc     #0
    ret

/* ── 时间相关 ───────────────────────────────────────────────────── */

.global gettimeofday
gettimeofday:
    mov     x8, #SYS_GETTIMEOFDAY
    svc     #0
    ret

#endif /* __aarch64__ */
