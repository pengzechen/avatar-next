#ifndef KERNEL_TASK_ITIMER_H
#define KERNEL_TASK_ITIMER_H

/*
 * kernel/task/itimer.h — 每任务 ITIMER_REAL（alarm / setitimer）
 *
 * 为什么需要：LTP 的每个测例都用 alarm() 给自己装看门狗，靠 SIGALRM 打断
 * 阻塞中的 read/poll 来从卡死里脱身。没有它，**任何卡住的测例都会把整个
 * 套件永久挂住** —— 这是跑回归时最先撞到的问题（实测 kill02 卡死）
 * 而不是"少一个不常用的 syscall"。
 *
 * 实现是 tick 驱动的粗暴扫描：定时器到期检查放在 sched_tick() 里，
 * 每次 tick（100 Hz）扫一遍任务池（TASK_MAX = 64）。64 × 100/s 的开销可以
 * 忽略，换来的是不需要维护定时器堆/红黑树，也不会引入新的锁。
 *
 * 只做 ITIMER_REAL。ITIMER_VIRTUAL / ITIMER_PROF 返回 EINVAL —— 本内核没有
 * 虚拟/统计 CPU 时间的概念，装个永远不响的定时器比明确报错更糟。
 */

#include "types.h"
#include "task/task.h"

/**
 * itimer_set - 装载/取消该任务的 ITIMER_REAL
 * @t:           目标任务
 * @value_ns:    首次到期延迟（纳秒）。0 表示取消。
 * @interval_ns: 周期（纳秒）。0 表示一次性。
 */
void itimer_set(task_t *t, uint64_t value_ns, uint64_t interval_ns);

/**
 * itimer_get - 读取剩余时间与周期
 * @t:           目标任务
 * @remain_ns:   输出：距下次到期的剩余纳秒（未装则 0）。可为 NULL。
 * @interval_ns: 输出：周期纳秒。可为 NULL。
 */
void itimer_get(task_t *t, uint64_t *remain_ns, uint64_t *interval_ns);

/**
 * itimer_tick - 到期扫描，由 sched_tick() 每次 tick 调用
 *
 * 对每个到期的任务：投 SIGALRM；若它正阻塞则唤醒（只置 pending 位是叫不醒
 * 阻塞任务的，见实现里的注释）。周期的重新装载，一次性的清除。
 */
void itimer_tick(void);

#endif /* KERNEL_TASK_ITIMER_H */