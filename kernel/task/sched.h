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

/**
 * sched_check_and_yield - 检查并执行重调度
 *
 * 由异常返回路径调用。如果 g_need_resched 标志被设置，
 * 则清除标志并执行 sched_schedule()。
 *
 * 返回：true 表示发生了任务切换，false 表示没有。
 */
bool sched_check_and_yield(void);

/**
 * sched_check_and_yield_from_trap - trap 返回前的调度检查
 * @frame_ptr: 架构 trap_frame_t 指针
 *
 * RISC-V 阶段 1 策略：允许 S-mode 接收中断，但 S-mode trap 返回时
 * 暂不抢占任意内核代码；U-mode trap 返回仍按 need_resched 调度。
 */
bool sched_check_and_yield_from_trap(void *frame_ptr);

#endif /* KERNEL_TASK_SCHED_H */
