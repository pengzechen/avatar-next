/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   core/sched.c - 调度相关 syscall(sched_yield, nanosleep)

   从 kernel/syscall/syscall.c 抽出。

   nanosleep 现在是真睡眠：
     - 截止时刻记在 task->sleep_expire_ns，由 100Hz tick(task/itimer.c)
       在到期时清标记并 task_unblock()；
     - 有会真正打断阻塞的信号(task_signal_pending_effective)时提前返回
       -EINTR，并把剩余时间写回 rem；
     - 不再用 yield 空转，也不再有"睡 1ms 实际占满 CPU"的行为。
*/
#include "syscall/syscall_internal.h"
#include "task/task.h"
#include "task/sched.h"
#include "driver/time.h"

void sched_yield_handler(uint64_t regs[6])
{
    task_yield();
    regs[0] = 0;
}

void nanosleep_handler(uint64_t regs[6])
{
    const int64_t NS_PER_SEC = 1000000000LL;

    struct {
        int64_t tv_sec;
        int64_t tv_nsec;
    } req;

    if (!regs[0]) {
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }
    if (copy_from_user_bytes((const void *)regs[0], &req, sizeof(req)) < 0) {
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= NS_PER_SEC) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    task_t *cur = task_current();
    if (!cur) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    uint64_t dur = (uint64_t)req.tv_sec * (uint64_t)NS_PER_SEC + (uint64_t)req.tv_nsec;
    uint64_t now = timer_get_ns();
    uint64_t deadline = now + dur;

    cur->sleep_expire_ns = deadline;

    /*
       睡到到期或被有效信号打断。tick 到期时会把 sleep_expire_ns 清 0，
       所以"标记消失"本身就代表睡完了。
    */
    while (!task_signal_pending_effective(cur)) {
        now = timer_get_ns();
        if (now >= deadline)
            break;

        task_block(NULL);

        /* tick 已处理到期：直接成功返回 */
        if (cur->sleep_expire_ns == 0)
            break;
    }

    bool interrupted = task_signal_pending_effective(cur);
    uint64_t expire = cur->sleep_expire_ns;
    cur->sleep_expire_ns = 0;

    if (!interrupted) {
        regs[0] = 0;
        return;
    }

    /* -EINTR：回填剩余时间（Linux 语义；rem 可为 NULL） */
    uint64_t rem = 0;
    if (expire != 0) {
        now = timer_get_ns();
        rem = (expire > now) ? (expire - now) : 0;
    }
    if (regs[1]) {
        struct {
            int64_t tv_sec;
            int64_t tv_nsec;
        } r;
        r.tv_sec = (int64_t)(rem / (uint64_t)NS_PER_SEC);
        r.tv_nsec = (int64_t)(rem % (uint64_t)NS_PER_SEC);
        if (copy_to_user_bytes(&r, (void *)regs[1], sizeof(r)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
    }
    regs[0] = (uint64_t)(int64_t)-EINTR;
}
