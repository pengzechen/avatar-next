#ifndef KERNEL_TASK_MUTEX_H
#define KERNEL_TASK_MUTEX_H

/*
 * kernel/task/mutex.h — 睡眠锁（Mutex）
 *
 * 实现：
 *  - 基于任务阻塞的睡眠锁
 *  - FIFO 等待队列（先等待先获取）
 *  - 不可重入（同一任务重复获取会死锁）
 *  - 支持超时（可选）
 */

#include "types.h"
#include "list.h"

/* 前向声明 */
typedef struct task task_t;

/* ── Mutex 结构 ──────────────────────────────────────────── */

typedef struct mutex {
    volatile bool    locked;      /* 锁状态：true = 已锁定          */
    list_t           wait_queue;  /* 等待队列（FIFO）               */
    task_t          *holder;      /* 当前持有锁的任务（用于调试）   */
} mutex_t;

/* ── Mutex API ───────────────────────────────────────────── */

/**
 * mutex_init - 初始化互斥锁
 * @mutex: 锁指针
 *
 * 使用前必须调用此函数初始化锁。
 */
void mutex_init(mutex_t *mutex);

/**
 * mutex_lock - 获取互斥锁（可能阻塞）
 * @mutex: 锁指针
 *
 * 如果锁已被其他任务持有，当前任务会进入睡眠等待。
 * 当锁可用时，任务被唤醒并获取锁。
 *
 * 注意：不可重入，同一任务重复获取会死锁。
 */
void mutex_lock(mutex_t *mutex);

/**
 * mutex_unlock - 释放互斥锁
 * @mutex: 锁指针
 *
 * 释放锁并唤醒一个等待的任务（如果有）。
 * 当前任务必须是锁的持有者。
 */
void mutex_unlock(mutex_t *mutex);

/**
 * mutex_trylock - 尝试获取互斥锁（不阻塞）
 * @mutex: 锁指针
 *
 * 返回：true = 成功获取，false = 锁已被持有
 *
 * 如果锁已被持有，立即返回 false，不会阻塞。
 */
bool mutex_trylock(mutex_t *mutex);

/**
 * mutex_is_locked - 检查锁是否已被持有
 * @mutex: 锁指针
 *
 * 返回：true = 已锁定，false = 未锁定
 */
bool mutex_is_locked(mutex_t *mutex);

/**
 * mutex_holder - 获取当前持有锁的任务（调试用）
 * @mutex: 锁指针
 *
 * 返回：持有锁的任务指针，如果未锁定则返回 NULL
 */
task_t *mutex_holder(mutex_t *mutex);

#endif /* KERNEL_TASK_MUTEX_H */
