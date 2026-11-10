/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef KERNEL_TASK_RT_MUTEX_H
#define KERNEL_TASK_RT_MUTEX_H

/*
   kernel/task/rt_mutex.h — 带优先级继承的睡眠锁

   与 task/mutex.h 的区别：
     - mutex：FIFO、可重入，没有优先级继承。适合普通内核/文件系统路径。
     - rt_mutex：非递归，等待者按优先级排队；高优先级等待者阻塞时把
       owner 临时提升到它的优先级，释放时把锁交给最高优先级等待者并
       恢复 owner 的基础优先级。适合 RT 任务之间的共享状态。

   使用约束：
     - 只能在任务上下文调用（会睡眠，不能进 ISR / step 的 RT 面）。
     - 非递归：同一任务重复加锁返回 -RTM_EDEADLK，不会自死锁。
     - 只对 RT 等待者产生提升；NORMAL 等待者的请求优先级按 0 计。
     - V1 只做直接继承：如果 owner 自己又阻塞在另一把 rt_mutex 上，
       不会沿等待链继续提升（没有嵌套 PI 链，也没有死锁检测）。
*/

#include "types.h"
#include "list.h"
#include "spinlock.h"

typedef struct task task_t;

#define RTM_EDEADLK 35
#define RTM_EINVAL  22

typedef struct rt_mutex {
    spinlock_t lock; /* 保护 owner 与 waiters */
    task_t *owner;   /* 当前持有者；NULL = 空闲 */
    list_t waiters;  /* 按有效优先级降序，同优先级 FIFO */
} rt_mutex_t;

void rt_mutex_init(rt_mutex_t *m);

/*
   rt_mutex_lock - 获取锁，可能睡眠
   返回 0 = 成功；-RTM_EDEADLK = 本任务已持有（非递归）；
   -RTM_EINVAL = 非任务上下文。
*/
int rt_mutex_lock(rt_mutex_t *m);

/* 非阻塞：成功 true。已被任何任务（包括自己）持有都返回 false。 */
bool rt_mutex_trylock(rt_mutex_t *m);

/* 释放锁并交给最高优先级等待者；调用者不是 owner 时返回 false。 */
bool rt_mutex_unlock(rt_mutex_t *m);

/* 观察用；内部加锁。 */
task_t *rt_mutex_owner(rt_mutex_t *m);

#endif /* KERNEL_TASK_RT_MUTEX_H */
