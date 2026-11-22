/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit-test/cyclictest.c — 内核态 cyclictest 等价物（RT_TEST=1）

   这个测试回答 rt-tests cyclictest 的同一个问题：
     “一个按绝对 deadline 周期睡眠的实时线程，实际醒来比 deadline 晚了多少？”

   当前项目还没有用户态 RT（sched_setscheduler / mlockall / clock_nanosleep
   尚未实现），所以这里用内核 RT 线程 + hrtimer_sleep_until() 做等价测量：
     - FIFO 80，钉 CPU0；
     - 周期 1ms（x86 TSC-deadline oneshot）或 10ms（tick 回退）；
     - 先跑 CYCLI_WARMUP 轮丢弃（缓存/启动抖动）；
     - 再记录 CYCLI_CYCLES 轮，统计 min/avg/p50/p99/p99.9/max；
     - x86 oneshot 下额外拆出 IRQ timer latency（deadline → 定时器
       ISR 入口），与总延迟（deadline → RT 线程运行）对照；
     - 同时跑一个 NORMAL hog 钉 CPU0，制造“RT 睡眠期间 CPU 被占满”的负载；
     - 如果醒来时已经超过下一个 deadline，记一次 overrun 并把 next 重同步，
       避免一次长延迟引发“补跑一串”的假象。

   与 rt-tests 的差异：
     - 线程是内核态，不是用户态；没有 syscall 返回路径的噪声；
     - 没有 mlockall/页错误干扰（内核线程没有用户 mm）；
     - 结果用于内核调度/hrtimer 回归，不等价于用户态应用的端到端延迟。

   输出格式（tests/suites/rt_test.py 解析）：
     === CYCLICTEST: PASS period=... warmup=... cycles=... samples=...
         min=... avg=... p50=... p99=... p999=... max=...
         irq_n=... irq_min=... irq_avg=... irq_p99=... irq_max=...
         overruns=... early=... hog=1 hog_iters=... ===

   `hog_iters` 是 NORMAL 忙循环实际完成的迭代数；它只用于证明“负载真的
   获得了 CPU 时间”，不是性能指标。
*/

#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"
#include "driver/time.h"
#include "exception.h"
#include "time/hrtimer.h"
#include "klog.h"

#define CYCLI_CYCLES   2000u
#define CYCLI_WARMUP   100u
#define CYCLI_PRIO     80u
#define CYCLI_HOG_PRIO 10u

static volatile bool g_cyc_done;
static volatile bool g_cyc_stop_hog;
static volatile uint64_t g_cyc_hog_iters;
static volatile uint32_t g_cyc_samples;
static volatile uint32_t g_cyc_overruns;
static volatile uint32_t g_cyc_early;

static uint32_t g_cyc_lat[CYCLI_CYCLES];
static uint64_t g_cyc_sum;
static uint64_t g_cyc_min = ~0ULL;
static uint64_t g_cyc_max;
static uint64_t g_cyc_avg;
static uint32_t g_cyc_p50;
static uint32_t g_cyc_p99;
static uint32_t g_cyc_p999;

/* IRQ timer latency：deadline → timer ISR 入口（x86 TSC-deadline）。 */
static uint32_t g_cyc_irq_lat[CYCLI_CYCLES];
static volatile uint32_t g_cyc_irq_n;
static uint64_t g_cyc_irq_sum;
static uint64_t g_cyc_irq_min = ~0ULL;
static uint64_t g_cyc_irq_max;
static uint64_t g_cyc_irq_avg;
static uint32_t g_cyc_irq_p99;

static void cyclictest_record(uint64_t lat)
{
    uint32_t n = g_cyc_samples;

    if (n >= CYCLI_CYCLES)
        return;

    if (lat > 0xFFFFFFFFULL)
        lat = 0xFFFFFFFFULL;
    g_cyc_lat[n] = (uint32_t)lat;

    g_cyc_sum += lat;
    if (lat < g_cyc_min)
        g_cyc_min = lat;
    if (lat > g_cyc_max)
        g_cyc_max = lat;

    __atomic_store_n(&g_cyc_samples, n + 1, __ATOMIC_RELEASE);
}

static void cyclictest_irq_record(uint64_t lat)
{
    uint32_t n = g_cyc_irq_n;

    if (n >= CYCLI_CYCLES)
        return;

    if (lat > 0xFFFFFFFFULL)
        lat = 0xFFFFFFFFULL;
    g_cyc_irq_lat[n] = (uint32_t)lat;

    g_cyc_irq_sum += lat;
    if (lat < g_cyc_irq_min)
        g_cyc_irq_min = lat;
    if (lat > g_cyc_irq_max)
        g_cyc_irq_max = lat;

    __atomic_store_n(&g_cyc_irq_n, n + 1, __ATOMIC_RELEASE);
}

/* 简单插入排序：CYCLI_CYCLES=2000，启动期一次性排序，成本可忽略。 */
static void cyclictest_sort(uint32_t *a, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        uint32_t v = a[i];
        uint32_t j = i;

        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

static uint32_t cyclictest_percentile(const uint32_t *a, uint32_t n, uint32_t permille)
{
    if (n == 0)
        return 0;

    uint32_t idx = (uint32_t)(((uint64_t)n * permille) / 1000ULL);
    if (idx >= n)
        idx = n - 1;
    return a[idx];
}

static void cyclictest_hog(void *arg)
{
    (void)arg;

    /*
       纯忙循环，不 yield：RT 线程睡眠的 1ms/10ms 窗口内，这颗 CPU 被
       NORMAL 负载占满；RT 醒来时靠 self-IPI/hrtimer 抢占它。
       iters 只用于证明“它真的跑过”，不作为性能指标。
    */
    uint64_t iters = 0;
    while (!__atomic_load_n(&g_cyc_stop_hog, __ATOMIC_ACQUIRE)) {
        iters++;
        __asm__ volatile("" ::: "memory");
    }

    __atomic_store_n(&g_cyc_hog_iters, iters, __ATOMIC_RELEASE);
    task_exit();
}

static void cyclictest_worker(void *arg)
{
    uint64_t period = (uint64_t)(uintptr_t)arg;
    uint64_t next = timer_get_ns() + period;

    for (uint32_t i = 0; i < CYCLI_WARMUP + CYCLI_CYCLES; i++) {
        hrtimer_sleep_until(next);

        uint64_t now = timer_get_ns();
        uint64_t lat = 0;

        if (now >= next) {
            lat = now - next;
        } else {
            /* 理论上 hrtimer_sleep_until 不会早醒；真早醒就记 0。 */
            __atomic_fetch_add(&g_cyc_early, 1U, __ATOMIC_RELAXED);
        }

        if (i >= CYCLI_WARMUP) {
            cyclictest_record(lat);

            /*
               同一个 deadline 的 IRQ 延迟：deadline → timer ISR 入口。
               总延迟 lat 是 deadline → 本线程运行；两者之差就是
               “唤醒后调度到线程”的那一段。非 oneshot 架构没有这个值。
            */
            uint64_t irq_lat = 0;
            if (now >= next && hrtimer_irq_latency(next, &irq_lat))
                cyclictest_irq_record(irq_lat);
        }

        next += period;

        /*
           已经错过下一个（或更多）deadline：记一次 overrun 并重同步，
           不补跑，否则一个长延迟会被后续连续唤醒放大成“恢复很快”的假象。
        */
        if (now > next) {
            __atomic_fetch_add(&g_cyc_overruns, 1U, __ATOMIC_RELAXED);
            next = now + period;
        }
    }

    uint32_t n = __atomic_load_n(&g_cyc_samples, __ATOMIC_ACQUIRE);
    cyclictest_sort(g_cyc_lat, n);

    g_cyc_avg = (n > 0) ? (g_cyc_sum / n) : 0;
    g_cyc_p50 = cyclictest_percentile(g_cyc_lat, n, 500);
    g_cyc_p99 = cyclictest_percentile(g_cyc_lat, n, 990);
    g_cyc_p999 = cyclictest_percentile(g_cyc_lat, n, 999);

    uint32_t irq_n = __atomic_load_n(&g_cyc_irq_n, __ATOMIC_ACQUIRE);
    cyclictest_sort(g_cyc_irq_lat, irq_n);
    g_cyc_irq_avg = (irq_n > 0) ? (g_cyc_irq_sum / irq_n) : 0;
    g_cyc_irq_p99 = cyclictest_percentile(g_cyc_irq_lat, irq_n, 990);

    __atomic_store_n(&g_cyc_stop_hog, true, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cyc_done, true, __ATOMIC_RELEASE);
    task_exit();
}

/*
   run_cyclictest - 在 RT_TEST 的 tick 已安装窗口内运行。
   返回 true 表示完成全部周期；延迟阈值由 tests/suites/rt_test.py 判。
*/
bool run_cyclictest(void)
{
    uint64_t period;
    uint64_t flags;

    g_cyc_done = false;
    g_cyc_stop_hog = false;
    g_cyc_hog_iters = 0;
    g_cyc_samples = 0;
    g_cyc_overruns = 0;
    g_cyc_early = 0;
    g_cyc_sum = 0;
    g_cyc_min = ~0ULL;
    g_cyc_max = 0;
    g_cyc_irq_n = 0;
    g_cyc_irq_sum = 0;
    g_cyc_irq_min = ~0ULL;
    g_cyc_irq_max = 0;

    /*
       x86 oneshot 可用时用 1ms；其它架构回退到 tick 周期。tick 回退路径
       需要 sched_tick -> itimer_tick() 来唤醒 sleep_expire_ns 睡眠者，
       所以这里自己安装/摘除回调，不依赖调用点的时序。
    */
    period = hrtimer_cpu_oneshot_supported(0) ? 1000000ULL : (uint64_t)TIMER_TICK_MS * 1000000ULL;
    if (period == 0)
        period = 10000000ULL;

    timer_set_tick_cb(sched_tick);

    flags = arch_irq_save();
    task_t *rt = task_create_kernel_rt("cyclictest", cyclictest_worker, (void *)(uintptr_t)period, CYCLI_PRIO, 0);
    task_t *hog = task_create_affinity("cycli-hog", cyclictest_hog, NULL, CYCLI_HOG_PRIO, 0);
    arch_irq_restore(flags);

    if (!rt || !hog) {
        timer_set_tick_cb(NULL);
        KLOG_ERROR("=== CYCLICTEST: FAIL (task create) ===\n");
        return false;
    }

    uint64_t timeout = timer_get_ns() + ((uint64_t)CYCLI_WARMUP + CYCLI_CYCLES) * period * 3ULL + 5000000000ULL;

    while (!__atomic_load_n(&g_cyc_done, __ATOMIC_ACQUIRE) && timer_get_ns() < timeout)
        task_yield();

    bool ok = __atomic_load_n(&g_cyc_done, __ATOMIC_ACQUIRE);
    if (!ok) {
        __atomic_store_n(&g_cyc_stop_hog, true, __ATOMIC_RELEASE);
        KLOG_ERROR("=== CYCLICTEST: FAIL (timeout) ===\n");
        timer_set_tick_cb(NULL);
        return false;
    }

    /*
       done 由 RT worker 在退出前置位；hog 是 NORMAL 且比 idle 高，
       boot/idle 上下文要等 hog 看到 stop_hog 并退出后才能跑到这里，
       所以此时 hog_iters 已经是最终值。
    */
    uint32_t n = __atomic_load_n(&g_cyc_samples, __ATOMIC_ACQUIRE);
    uint32_t irq_n = __atomic_load_n(&g_cyc_irq_n, __ATOMIC_ACQUIRE);
    KLOG_INFO("=== CYCLICTEST: PASS period=%llu warmup=%u cycles=%u samples=%u "
              "min=%llu avg=%llu p50=%u p99=%u p999=%u max=%llu "
              "irq_n=%u irq_min=%llu irq_avg=%llu irq_p99=%u irq_max=%llu "
              "overruns=%u early=%u hog=1 hog_iters=%llu ===\n",
              (unsigned long long)period, CYCLI_WARMUP, CYCLI_CYCLES, n, (unsigned long long)((n > 0) ? g_cyc_min : 0),
              (unsigned long long)g_cyc_avg, g_cyc_p50, g_cyc_p99, g_cyc_p999, (unsigned long long)g_cyc_max, irq_n,
              (unsigned long long)((irq_n > 0) ? g_cyc_irq_min : 0), (unsigned long long)g_cyc_irq_avg, g_cyc_irq_p99,
              (unsigned long long)g_cyc_irq_max, __atomic_load_n(&g_cyc_overruns, __ATOMIC_ACQUIRE),
              __atomic_load_n(&g_cyc_early, __ATOMIC_ACQUIRE),
              (unsigned long long)__atomic_load_n(&g_cyc_hog_iters, __ATOMIC_ACQUIRE));

    timer_set_tick_cb(NULL);
    return true;
}
