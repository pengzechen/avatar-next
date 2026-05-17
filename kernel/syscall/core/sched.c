/*
 * core/sched.c - 调度相关 syscall（sched_yield, nanosleep）
 *
 * 从 kernel/syscall/syscall.c 抽出。
 * 当前 nanosleep 是简化实现：仅 yield 一次返回 0。
 */
#include "syscall/syscall_internal.h"
#include "task/task.h"
#include "task/sched.h"

void sched_yield_handler(uint64_t regs[6])
{
    task_yield();
    regs[0] = 0;
}

void nanosleep_handler(uint64_t regs[6])
{
    /* TODO: 真正基于定时器睡眠；当前仅 yield 一次 */
    (void)regs;
    task_yield();
    regs[0] = 0;
}
