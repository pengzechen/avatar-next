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

/* 场景 4：NORMAL → RT 立即抢占（self-IPI） */
#define PREEMPT_TEST_ITERS      10
#define PREEMPT_TEST_AVG_MAX_NS 2000000ULL /* 2ms；无 self-IPI 时均值≈tick 周期（10ms） */

static volatile uint32_t s_high_count;
static volatile uint32_t s_low_count;
static volatile uint32_t s_normal_count;
static volatile uint32_t s_done;
static volatile bool s_high_saw_low_zero;
static volatile bool s_low_saw_normal_zero;

static task_t *s_preempt_probe;
static volatile uint64_t s_preempt_wake;
static volatile uint32_t s_preempt_done;
static uint64_t s_preempt_min = ~0ULL;
static uint64_t s_preempt_sum;
static uint64_t s_preempt_max;

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

/*
   场景 4：NORMAL 任务唤醒更高优先级的 RT 任务后继续纯忙循环。
   RT 任务必须靠 self-IPI 立即抢占；没有 self-IPI 时它只能等下一次
   tick，10 轮的平均值会接近 tick 周期（10ms）。
*/
static void rt_preempt_probe_worker(void *arg)
{
    (void)arg;

    for (uint32_t i = 0; i < PREEMPT_TEST_ITERS; i++) {
        task_block(NULL); /* 等 NORMAL preemptor 唤醒 */

        uint64_t now = timer_get_ns();
        uint64_t wake = __atomic_load_n(&s_preempt_wake, __ATOMIC_ACQUIRE);
        uint64_t lat = (now > wake) ? (now - wake) : 0;

        if (lat < s_preempt_min)
            s_preempt_min = lat;
        if (lat > s_preempt_max)
            s_preempt_max = lat;
        s_preempt_sum += lat;

        __atomic_store_n(&s_preempt_done, i + 1, __ATOMIC_RELEASE);
    }

    __atomic_fetch_add(&s_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void normal_preemptor_worker(void *arg)
{
    (void)arg;

    for (uint32_t i = 0; i < PREEMPT_TEST_ITERS; i++) {
        __atomic_store_n(&s_preempt_wake, timer_get_ns(), __ATOMIC_RELEASE);
        task_unblock(s_preempt_probe);

        /* 纯忙循环：没有 syscall/返回边界，只能被中断或立即抢占打断 */
        while (__atomic_load_n(&s_preempt_done, __ATOMIC_ACQUIRE) <= i)
            __asm__ volatile("" ::: "memory");
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

    if (!pass) {
        KLOG_ERROR("=== RT SCHED TEST: FAIL (done=%u high=%u low=%u normal=%u order=%d/%d) ===\n", s_done, s_high_count,
                   s_low_count, s_normal_count, (int)s_high_saw_low_zero, (int)s_low_saw_normal_zero);
        return;
    }

    /*
       场景 4 需要 tick 作为“没有 self-IPI 时的退路”，临时安装。
       idle/boot 上下文的 tick 只会置 need_resched，不会抢走本函数
       的执行；任务都结束后在这里摘掉，main 后面才正式安装。
    */
    timer_set_tick_cb(sched_tick);

    s_done = 0;
    s_preempt_done = 0;
    s_preempt_min = ~0ULL;
    s_preempt_sum = 0;
    s_preempt_max = 0;

    flags = arch_irq_save();
    s_preempt_probe = task_create_kernel_rt("rt-pmp", rt_preempt_probe_worker, NULL, 60, 0);
    task_t *preemptor = task_create_affinity("rt-pre", normal_preemptor_worker, NULL, 10, 0);
    arch_irq_restore(flags);

    bool preempt_pass = false;
    if (!s_preempt_probe || !preemptor) {
        KLOG_ERROR("=== RT SCHED TEST: FAIL (preempt task create) ===\n");
    } else {
        deadline = timer_get_ns() + 3000000000ULL; /* 3s */
        while (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE) < 2 && timer_get_ns() < deadline)
            task_yield();

        uint64_t avg = s_preempt_sum / PREEMPT_TEST_ITERS;
        preempt_pass = s_done == 2 && avg < PREEMPT_TEST_AVG_MAX_NS;
        KLOG_INFO("=== RT SCHED TEST: preempt min/avg/max = %llu/%llu/%llu ns (done=%u) ===\n", s_preempt_min, avg,
                  s_preempt_max, s_done);
    }

    timer_set_tick_cb(NULL);

    if (preempt_pass) {
        KLOG_INFO("=== RT SCHED TEST: PASS ===\n");
    } else {
        KLOG_ERROR("=== RT SCHED TEST: FAIL (immediate preemption) ===\n");
    }
}
