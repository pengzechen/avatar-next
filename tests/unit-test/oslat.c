/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit-test/oslat.c — oslat 等价物（RT_TEST=1）

   Linux oslat 测的是“一个已经在 CPU 上忙跑的 RT 线程，被 OS 噪声
   （IRQ、tick、宿主调度）打断多久”。它不是周期唤醒延迟，而是执行
   连续性：

     RT 线程连续读时钟，记录相邻两次读取的间隔；间隔就是这颗 CPU
     没有让 RT 线程推进的时间。典型来源是 timer tick、UART IRQ、
     关中断区间和 QEMU/KVM 的宿主抢占。

   本实现只在内核态测量（用户态 RT API 尚未实现），固定 FIFO 85、
   钉 CPU0、跑 OSLAT_DURATION_NS。输出 min/avg/max 和超过
   1µs/10µs/100µs 的次数；`samples` 是循环实际迭代数，用来证明
   负载真的运行过。x86_64 是主要目标，aarch64/riscv64 也执行同一
   测试；三者的计时分辨率和 TCG 宿主噪声不同。

   注意：计时函数本身的调用开销会计入 min/avg；绝对值适合同架构
   前后对比，不要和 rt-tests 用户态 oslat 的数字直接比较。
*/

#include "task/task.h"
#include "task/sched.h"
#include "driver/time.h"
#include "exception.h"
#include "klog.h"

#define OSLAT_DURATION_NS   500000000ULL /* 500ms，控制 CI 时间 */
#define OSLAT_PRIO          85u
#define OSLAT_OVERRUN_1US   1000ULL
#define OSLAT_OVERRUN_10US  10000ULL
#define OSLAT_OVERRUN_100US 100000ULL

static volatile bool g_oslat_done;
static volatile uint64_t g_oslat_samples;
static volatile uint64_t g_oslat_min;
static volatile uint64_t g_oslat_sum;
static volatile uint64_t g_oslat_max;
static volatile uint64_t g_oslat_over_1us;
static volatile uint64_t g_oslat_over_10us;
static volatile uint64_t g_oslat_over_100us;

static void oslat_worker(void *arg)
{
    (void)arg;

    uint64_t deadline = timer_get_ns() + OSLAT_DURATION_NS;
    uint64_t prev = timer_get_ns();
    uint64_t samples = 0;
    uint64_t min = ~0ULL;
    uint64_t sum = 0;
    uint64_t max = 0;
    uint64_t over_1us = 0;
    uint64_t over_10us = 0;
    uint64_t over_100us = 0;

    for (;;) {
        uint64_t now = timer_get_ns();
        if (now >= deadline)
            break;

        uint64_t gap = now - prev;
        prev = now;

        if (gap < min)
            min = gap;
        if (gap > max)
            max = gap;
        sum += gap;
        if (gap > OSLAT_OVERRUN_1US)
            over_1us++;
        if (gap > OSLAT_OVERRUN_10US)
            over_10us++;
        if (gap > OSLAT_OVERRUN_100US)
            over_100us++;
        samples++;
    }

    __atomic_store_n(&g_oslat_samples, samples, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oslat_min, min, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oslat_sum, sum, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oslat_max, max, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oslat_over_1us, over_1us, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oslat_over_10us, over_10us, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oslat_over_100us, over_100us, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oslat_done, true, __ATOMIC_RELEASE);
    task_exit();
}

bool run_oslat(void)
{
    g_oslat_done = false;
    g_oslat_samples = 0;
    g_oslat_min = 0;
    g_oslat_sum = 0;
    g_oslat_max = 0;
    g_oslat_over_1us = 0;
    g_oslat_over_10us = 0;
    g_oslat_over_100us = 0;

    uint64_t flags = arch_irq_save();
    task_t *t = task_create_kernel_rt("oslat", oslat_worker, NULL, OSLAT_PRIO, 0);
    arch_irq_restore(flags);
    if (!t) {
        KLOG_ERROR("=== OSLAT: FAIL (task create) ===\n");
        return false;
    }

    uint64_t deadline = timer_get_ns() + OSLAT_DURATION_NS * 3ULL + 5000000000ULL;
    while (!__atomic_load_n(&g_oslat_done, __ATOMIC_ACQUIRE) && timer_get_ns() < deadline)
        task_yield();

    if (!__atomic_load_n(&g_oslat_done, __ATOMIC_ACQUIRE)) {
        KLOG_ERROR("=== OSLAT: FAIL (timeout) ===\n");
        return false;
    }

    uint64_t samples = __atomic_load_n(&g_oslat_samples, __ATOMIC_ACQUIRE);
    uint64_t sum = __atomic_load_n(&g_oslat_sum, __ATOMIC_ACQUIRE);
    KLOG_INFO("=== OSLAT: PASS duration=%llu samples=%llu min=%llu avg=%llu max=%llu "
              "over_1us=%llu over_10us=%llu over_100us=%llu ===\n",
              (unsigned long long)OSLAT_DURATION_NS, (unsigned long long)samples,
              (unsigned long long)__atomic_load_n(&g_oslat_min, __ATOMIC_ACQUIRE),
              (unsigned long long)((samples > 0) ? (sum / samples) : 0),
              (unsigned long long)__atomic_load_n(&g_oslat_max, __ATOMIC_ACQUIRE),
              (unsigned long long)__atomic_load_n(&g_oslat_over_1us, __ATOMIC_ACQUIRE),
              (unsigned long long)__atomic_load_n(&g_oslat_over_10us, __ATOMIC_ACQUIRE),
              (unsigned long long)__atomic_load_n(&g_oslat_over_100us, __ATOMIC_ACQUIRE));
    return true;
}
