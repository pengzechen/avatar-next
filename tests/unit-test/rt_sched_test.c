/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit-test/rt_sched_test.c — RT 调度类自测（RT_TEST=1）

   验证 R2 的语义：
     1. SCHED_FIFO 高优先级任务先于低优先级 RT 任务运行；
     2. 低优先级 RT 任务先于 NORMAL 任务运行；
     3. FIFO 任务不被 tick 轮转（自己 yield 时仍保持最高优先级）。

   所有任务钉在 CPU0，避免 SMP 下并行执行让顺序断言失效。
   测试只创建短暂的内核任务，跑完自行 task_exit，不影响后续启动。
*/

#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"
#include "driver/time.h"
#include "exception.h"
#include "klog.h"

#define RT_TEST_ITERS 100

static volatile uint32_t s_high_count;
static volatile uint32_t s_low_count;
static volatile uint32_t s_normal_count;
static volatile uint32_t s_done;
static volatile bool s_high_saw_low_zero;
static volatile bool s_low_saw_normal_zero;

static void rt_test_worker(void *arg)
{
    int prio = (int)(uintptr_t)arg;

    for (uint32_t i = 0; i < RT_TEST_ITERS; i++) {
        if (prio == 50) {
            s_high_count++;
            if (i + 1 == RT_TEST_ITERS)
                s_high_saw_low_zero = (s_low_count == 0);
        } else {
            s_low_count++;
            if (i + 1 == RT_TEST_ITERS)
                s_low_saw_normal_zero = (s_normal_count == 0);
        }
        task_yield();
    }

    __atomic_fetch_add(&s_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void normal_test_worker(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < RT_TEST_ITERS; i++) {
        s_normal_count++;
        task_yield();
    }
    __atomic_fetch_add(&s_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

void run_rt_sched_test(void)
{
    KLOG_INFO("=== RT SCHED TEST ===\n");

    s_high_count = s_low_count = s_normal_count = 0;
    s_done = 0;
    s_high_saw_low_zero = false;
    s_low_saw_normal_zero = false;

    /*
       三个任务必须在同一段关中断区间内创建：第一个 RT 任务入队后
       need_resched 已经置位，若中途被 tick 抢占，测试会先跑 RT 任务，
       后面的任务还没创建，顺序断言就失去意义。
    */
    uint64_t flags = arch_irq_save();
    task_t *normal = task_create_affinity("rt-norm", normal_test_worker, NULL, 10, 0);
    task_t *low = task_create_kernel_rt("rt-low", rt_test_worker, (void *)10, 10, 0);
    task_t *high = task_create_kernel_rt("rt-high", rt_test_worker, (void *)50, 50, 0);
    arch_irq_restore(flags);

    if (!normal || !low || !high) {
        KLOG_ERROR("=== RT SCHED TEST: FAIL (task create) ===\n");
        return;
    }

    uint64_t deadline = timer_get_ns() + 2000000000ULL; /* 2s */
    while (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE) < 3 && timer_get_ns() < deadline)
        task_yield();

    bool pass = s_done == 3 && s_high_count == RT_TEST_ITERS && s_low_count == RT_TEST_ITERS &&
                s_normal_count == RT_TEST_ITERS && s_high_saw_low_zero && s_low_saw_normal_zero;

    if (pass) {
        KLOG_INFO("=== RT SCHED TEST: PASS ===\n");
    } else {
        KLOG_ERROR("=== RT SCHED TEST: FAIL (done=%u high=%u low=%u normal=%u order=%d/%d) ===\n", s_done, s_high_count,
                   s_low_count, s_normal_count, (int)s_high_saw_low_zero, (int)s_low_saw_normal_zero);
    }
}
