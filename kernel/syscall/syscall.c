/*
 * kernel/syscall/syscall.c - 系统调用实现
 */

#include "syscall/syscall.h"
#include "syscall/bin_loader.h"
#include "klog.h"
#include "task/task.h"
#include "task/sched.h"

/* ── 系统调用处理函数 ──────────────────────────────────────────── */

void syscall_handler(uint64_t *regs)
{
    /* AArch64 系统调用约定：
     * x8 = 系统调用号
     * x0-x7 = 参数
     * 返回值写入 x0
     */
    uint64_t syscall_num = regs[8];

    /* 调试信息 */
    task_t *current = task_current();
    KLOG_DEBUG("[syscall] pid=%u, syscall_num=%llu, args=[0x%llx, 0x%llx, 0x%llx]\n",
              current->id, syscall_num, regs[0], regs[1], regs[2]);

    /* 分发系统调用 */
    switch (syscall_num) {
        /* 进程管理 */
        case SYS_EXIT: {
            int status = (int)regs[0];
            sys_exit(status);
            break;
        }

        case SYS_YIELD: {
            regs[0] = sys_yield();
            break;
        }

        case SYS_GETPID: {
            regs[0] = sys_getpid();
            break;
        }

        case SYS_SLEEP: {
            uint64_t ms = regs[0];
            regs[0] = sys_sleep(ms);
            break;
        }

        case SYS_EXECVE: {
            const char *pathname = (const char *)regs[0];
            char **argv = (char **)regs[1];
            char **envp = (char **)regs[2];
            regs[0] = sys_execve(pathname, argv, envp);
            break;
        }

        /* 内存管理 */
        case SYS_BRK: {
            void *addr = (void *)regs[0];
            regs[0] = (uint64_t)sys_brk(addr);
            break;
        }

        case SYS_SBRK: {
            int64_t increment = (int64_t)regs[0];
            regs[0] = (uint64_t)sys_sbrk(increment);
            break;
        }

        /* 文件操作 */
        case SYS_WRITE: {
            const char *str = (const char *)regs[0];
            uint64_t len = regs[1];
            regs[0] = sys_write(str, len);
            break;
        }

        case SYS_READ: {
            char *buf = (char *)regs[0];
            uint64_t len = regs[1];
            regs[0] = sys_read(buf, len);
            break;
        }

        case SYS_OPEN: {
            const char *pathname = (const char *)regs[0];
            int flags = (int)regs[1];
            int mode = (int)regs[2];
            regs[0] = sys_open(pathname, flags, mode);
            break;
        }

        case SYS_CLOSE: {
            int fd = (int)regs[0];
            regs[0] = sys_close(fd);
            break;
        }

        /* 时间相关 */
        case SYS_GETTIMEOFDAY: {
            struct timeval *tv = (struct timeval *)regs[0];
            void *tz = (void *)regs[1];
            regs[0] = sys_gettimeofday(tv, tz);
            break;
        }

        default:
            KLOG_ERROR("[syscall] Unknown syscall: %llu\n", syscall_num);
            regs[0] = -1;
            break;
    }
}

/* ── 具体系统调用实现 ──────────────────────────────────────────── */

int64_t sys_write(const char *str, uint64_t len)
{
    if (str == NULL) {
        return -1;
    }

    /* 简单实现：直接输出到 UART */
    /* 注意：这里没有验证用户指针，后续需要改进 */
    for (uint64_t i = 0; i < len; i++) {
        klog_putchar(str[i]);
    }

    return (int64_t)len;
}

void sys_exit(int status)
{
    task_t *current = task_current();

    KLOG_INFO("[syscall] process '%s' (id=%u) exiting with status %d\n",
              current->name, current->id, status);

    /* 调用 task_exit 退出当前进程 */
    task_exit();

    /* 不应该到达这里 */
    while (1)
        ;
}

int64_t sys_yield(void)
{
    task_yield();
    return 0;
}

int64_t sys_getpid(void)
{
    task_t *current = task_current();
    return (int64_t)current->id;
}

int64_t sys_sleep(uint64_t ms)
{
    /* 简单实现：使用忙等待
     * TODO: 改进为基于定时器的睡眠，让出 CPU
     */
    if (ms == 0) {
        return 0;
    }

    /* 暂时使用简单的忙等待循环
     * 假设 CPU 运行在约 1-2 GHz，每次循环约几个纳秒
     */
    volatile uint64_t count = ms * 100000;
    while (count--) {
        __asm__ volatile("nop");
    }

    return 0;
}

int64_t sys_execve(const char *pathname, char **argv, char **envp)
{
    if (pathname == NULL) {
        return -1;
    }

    KLOG_DEBUG("[syscall] execve('%s')\n", pathname);

    /* 调用二进制加载器 */
    int rc = bin_loader_load_from_file(pathname, argv, envp);

    /* 如果成功，不应该到达这里 */
    return rc;
}

void *sys_brk(void *addr)
{
    /* 简单实现：未实现堆管理
     * TODO: 实现真正的堆管理器
     */
    /* 如果 addr 为 NULL，返回当前堆顶 */
    if (addr == NULL) {
        /* 暂时返回一个固定地址 */
        return (void *)0x50000000;
    }

    /* 暂时不支持动态调整堆 */
    KLOG_WARN("[syscall] brk(0x%llx) not fully supported\n", addr);
    return (void *)-1;
}

void *sys_sbrk(int64_t increment)
{
    /* 简单实现：维护一个简单的堆指针
     * TODO: 实现真正的堆管理器
     */
    static uint64_t heap_end = 0x50000000;
    void *old_brk = (void *)heap_end;

    if (increment == 0) {
        return old_brk;
    }

    heap_end += increment;

    KLOG_DEBUG("[syscall] sbrk(%lld) = 0x%llx\n", increment, old_brk);
    return old_brk;
}

int64_t sys_read(char *buf, uint64_t len)
{
    if (buf == NULL || len == 0) {
        return -1;
    }

    /* 简单实现：阻塞读取单个字符
     * TODO: 实现真正的输入缓冲
     */
    /* 暂时返回 0 表示没有数据 */
    return 0;
}

int64_t sys_open(const char *pathname, int flags, int mode)
{
    (void)flags;
    (void)mode;
    /* 暂未实现文件系统 */
    KLOG_WARN("[syscall] open('%s') not implemented\n", pathname);
    return -1;
}

int64_t sys_close(int fd)
{
    /* 暂未实现文件系统 */
    KLOG_WARN("[syscall] close(%d) not implemented\n", fd);
    return -1;
}

int64_t sys_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    /* 简单实现：返回固定时间
     * TODO: 实现真正的时钟驱动
     */
    if (tv == NULL) {
        return -1;
    }

    /* 暂时返回一个固定的时间值 */
    tv->tv_sec = 1000;
    tv->tv_usec = 0;

    return 0;
}
