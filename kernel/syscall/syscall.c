/*
 * kernel/syscall/syscall.c - 系统调用实现
 */

#include "syscall/syscall.h"
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
        case SYS_WRITE: {
            const char *str = (const char *)regs[0];
            uint64_t len = regs[1];
            regs[0] = sys_write(str, len);
            break;
        }

        case SYS_EXIT: {
            int status = (int)regs[0];
            sys_exit(status);
            /* 不返回 */
            break;
        }

        case SYS_YIELD: {
            regs[0] = sys_yield();
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
