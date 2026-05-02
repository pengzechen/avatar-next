#ifndef KERNEL_TASK_SCHED_H
#define KERNEL_TASK_SCHED_H

/*
 * kernel/task/sched.h — 调度器接口
 *
 * 实现：简单轮转调度（Round-Robin）
 *  - 就绪队列：FIFO 双向链表
 *  - idle 任务作为空队列时的回退任务
 *  - sched_tick() 由定时器 ISR 调用，触发抢占
 */

#include "task/task.h"

/**
 * sched_init - 初始化调度器
 * @idle_task: boot 上下文对应的 idle 任务指针
 *
 * 由 task_init() 调用，不应被用户直接调用。
 */
void sched_init(task_t *idle_task);

/**
 * sched_enqueue - 将任务加入就绪队列尾部
 * @task: 状态必须为 TASK_READY
 */
void sched_enqueue(task_t *task);

/**
 * sched_dequeue - 将任务从就绪队列中移除
 * @task: 要移除的任务（若不在队列中则无操作）
 */
void sched_dequeue(task_t *task);

/**
 * sched_schedule - 执行一次调度
 *
 * 将当前任务（若仍 RUNNING）重新入队尾部，
 * 从队头取下一个任务并切换过去。
 * 调用前中断状态不限；函数内部会临时关中断。
 */
void sched_schedule(void);

/**
 * sched_tick - 定时器 tick 回调，触发抢占式调度
 *
 * 由 timer 驱动通过 timer_set_tick_cb() 注册后，
 * 在每个 timer 中断中被调用。
 */
void sched_tick(void);

#endif /* KERNEL_TASK_SCHED_H */
