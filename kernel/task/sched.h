/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef KERNEL_TASK_SCHED_H
#define KERNEL_TASK_SCHED_H

/*
   kernel/task/sched.h — 调度器接口

   实现：简单轮转调度(Round-Robin)
    - 就绪队列：FIFO 双向链表
    - idle 任务作为空队列时的回退任务
    - sched_tick() 由定时器 ISR 调用, 触发抢占
*/

#include "task/task.h"

/*
   sched_init - 初始化调度器
   @idle_task: boot 上下文对应的 idle 任务指针

   由 task_init() 调用, 不应被用户直接调用。
*/
void sched_init(task_t *idle_task);

/*
   sched_enqueue - 将任务加入就绪队列尾部
   @task: 状态必须为 TASK_READY
*/
void sched_enqueue(task_t *task);

/*
   sched_dequeue - 将任务从就绪队列中移除
   @task: 要移除的任务(若不在队列中则无操作)
*/
void sched_dequeue(task_t *task);

/*
   sched_unblock - 把 BLOCKED 任务唤醒并(按需)入队

   与旧的 task_unblock 内联实现相比, 本函数：
     - 在目标核 rq_lock 下检查 state/on_cpu, 消除「任务还在跑就入队」；
     - 若任务仍在 on_cpu, 只置 wake_pending, 由它自己的调度器在切出时入队；
     - 真正入队到远端核时立刻发 resched IPI, 不等下一次 tick。

   可在任务/中断上下文调用。
*/
void sched_unblock(task_t *task);

/*
   sched_resched_cpu - 请求某颗核重新调度
   @cpu_id: 逻辑 CPU 编号

   置目标核的 need_resched, 并向远端核发 resched IPI。目标核可能正在：
     - 跑宿主任务/内核线程 → 中断返回路径上调度；
     - 跑 guest → IPI 被硬件路由为一次 VM-exit/HS trap, VMM 主循环顶部
       调 sched_check_and_yield() 让出 CPU。
   因此它同时是「跨核抢占 vCPU 任务」的入口。
*/
void sched_resched_cpu(uint32_t cpu_id);

/*
   sched_schedule - 执行一次调度

   将当前任务(若仍 RUNNING)重新入队尾部,
   从队头取下一个任务并切换过去。
   调用前中断状态不限；函数内部会临时关中断。
*/
void sched_schedule(void);

/*
   sched_tick - 定时器 tick 回调, 触发抢占式调度

   由 timer 驱动通过 timer_set_tick_cb() 注册后,
   在每个 timer 中断中被调用。
*/
void sched_tick(void);

/*
   sched_check_and_yield - 检查并执行重调度

   由异常返回路径调用。如果 g_need_resched 标志被设置,
   则清除标志并执行 sched_schedule()。

   返回：true 表示发生了任务切换, false 表示没有。
*/
bool sched_check_and_yield(void);

/*
   sched_check_and_yield_from_trap - trap 返回前的调度检查
   @frame_ptr: 架构 trap_frame_t 指针

   AArch64/RISC-V：返回用户态时按 need_resched 调度；返回内核态时
   仅在 preemptible() 为 true 时调度。
*/
bool sched_check_and_yield_from_trap(void *frame_ptr);

#endif /* KERNEL_TASK_SCHED_H */
