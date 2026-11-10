/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit-test/rt_demo.c — RT 调度性能 demo（RT_DEMO=1）

   两个场景，全部钉 CPU0：
     1. RT ping-pong：RT FIFO 50 worker 与 FIFO 10 waker 互相 block/unblock，
        测 wake→run 延迟（min/avg/max + 直方图）；
     2. 普通任务→RT 抢占：普通内核线程唤醒 RT FIFO 60 probe 后继续忙循环，
        probe 要等下一次 timer 抢占点才能运行 —— 用来展示当前 100 Hz tick
        对“非自愿抢占”的约束。

   这两个场景只用内核线程 API，不经过 syscall，符合“RT 仅内核域”的定位。

   调用点必须在创建 net-poll / busybox 之前（见 kernel/main.c）：boot 上下文
   是 idle 任务，而 idle 永不进入就绪队列。一旦有 net-poll 这种“永远 ready”
   的 NORMAL 任务，boot 被 tick 抢占后就不会再被调度回来，后面的调用点根本
   到不了。demo 自己临时安装 tick 回调，结束后摘掉，不改变后续启动时序。
*/

#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"
#include "driver/time.h"
#include "exception.h"
#include "klog.h"

#define DEMO_ITERS      5000
#define PREEMPT_ITERS   50
#define HIST_BUCKETS    9
#define PREEMPT_BUCKETS 8
#define DEMO_TIMEOUT_NS 10000000000ULL /* 10s 兜底 */
#define HOG_TIMEOUT_NS  50000000ULL    /* 单次 50ms 兜底（tick 周期 10ms） */

static const uint64_t g_hist_thresh[HIST_BUCKETS] = {
    1000ULL, 2000ULL, 5000ULL, 10000ULL, 20000ULL, 50000ULL, 100000ULL, 1000000ULL, ~0ULL,
};

static const uint64_t g_preempt_thresh[PREEMPT_BUCKETS] = {
    2000ULL, 5000ULL, 10000ULL, 20000ULL, 50000ULL, 100000ULL, 1000000ULL, ~0ULL,
};

static task_t *g_worker;
static task_t *g_waker;
static task_t *g_probe;

static volatile uint64_t g_ping_wake;
static volatile uint32_t g_ping_done;
static uint64_t g_lat_min = ~0ULL;
static uint64_t g_lat_max;
static uint64_t g_lat_sum;
static uint32_t g_lat_n;
static uint32_t g_hist[HIST_BUCKETS];

static volatile uint64_t g_probe_wake;
static volatile uint32_t g_probe_done;
static volatile bool g_demo_done;

static uint64_t g_pre_min = ~0ULL;
static uint64_t g_pre_max;
static uint64_t g_pre_sum;
static uint32_t g_pre_n;
static uint32_t g_pre_hist[PREEMPT_BUCKETS];

static void demo_log_hist(const char *tag, const uint64_t *thresh, const uint32_t *hist, int n)
{
    for (int i = 0; i < n; i++) {
        if (thresh[i] == ~0ULL)
            KLOG_INFO("=== RT DEMO %s bucket[overflow] = %u ===\n", tag, hist[i]);
        else
            KLOG_INFO("=== RT DEMO %s bucket[%llu ns] = %u ===\n", tag, thresh[i], hist[i]);
    }
}

static void demo_record(uint64_t lat)
{
    if (lat < g_lat_min)
        g_lat_min = lat;
    if (lat > g_lat_max)
        g_lat_max = lat;
    g_lat_sum += lat;
    g_lat_n++;

    for (int i = 0; i < HIST_BUCKETS; i++) {
        if (lat < g_hist_thresh[i]) {
            g_hist[i]++;
            break;
        }
    }
}

static void rt_worker_fn(void *arg)
{
    (void)arg;

    for (uint32_t i = 0; i < DEMO_ITERS; i++) {
        task_block(NULL);

        uint64_t now = timer_get_ns();
        uint64_t wake = __atomic_load_n(&g_ping_wake, __ATOMIC_ACQUIRE);
        uint64_t lat = (now > wake) ? (now - wake) : 0;
        demo_record(lat);

        __atomic_store_n(&g_ping_done, i + 1, __ATOMIC_RELEASE);
    }

    KLOG_INFO("=== RT DEMO pingpong: n=%u min=%llu avg=%llu max=%llu ns ===\n", g_lat_n, g_lat_min,
              g_lat_n ? (g_lat_sum / g_lat_n) : 0, g_lat_max);
    demo_log_hist("pingpong", g_hist_thresh, g_hist, HIST_BUCKETS);

    task_exit();
}

static void demo_preempt_record(uint64_t lat)
{
    if (lat < g_pre_min)
        g_pre_min = lat;
    if (lat > g_pre_max)
        g_pre_max = lat;
    g_pre_sum += lat;
    g_pre_n++;

    for (int i = 0; i < PREEMPT_BUCKETS; i++) {
        if (lat < g_preempt_thresh[i]) {
            g_pre_hist[i]++;
            break;
        }
    }
}

static void rt_probe_fn(void *arg)
{
    (void)arg;

    for (uint32_t i = 0; i < PREEMPT_ITERS; i++) {
        task_block(NULL); /* 等普通 hog 唤醒 */

        uint64_t now = timer_get_ns();
        uint64_t wake = __atomic_load_n(&g_probe_wake, __ATOMIC_ACQUIRE);
        demo_preempt_record((now > wake) ? (now - wake) : 0);

        __atomic_store_n(&g_probe_done, i + 1, __ATOMIC_RELEASE);
    }

    task_exit();
}

static void normal_hog_fn(void *arg)
{
    (void)arg;

    /*
       普通内核线程不被 timer 以外的机制抢占（RT probe 已就绪但当前仍是
       本任务）。每次唤醒后一直算，直到 probe 在 tick 抢占点运行完，
       共 PREEMPT_ITERS 轮 —— 单次采样落在 10ms tick 周期的哪个相位
       是随机的，要分布才有意义。
    */
    for (uint32_t i = 0; i < PREEMPT_ITERS; i++) {
        __atomic_store_n(&g_probe_wake, timer_get_ns(), __ATOMIC_RELEASE);
        task_unblock(g_probe);

        uint64_t deadline = timer_get_ns() + HOG_TIMEOUT_NS;
        while (__atomic_load_n(&g_probe_done, __ATOMIC_ACQUIRE) <= i && timer_get_ns() < deadline) {
            __asm__ volatile("" ::: "memory");
        }
    }

    KLOG_INFO("=== RT DEMO preempt: n=%u min=%llu avg=%llu max=%llu ns ===\n", g_pre_n, g_pre_min,
              g_pre_n ? (g_pre_sum / g_pre_n) : 0, g_pre_max);
    demo_log_hist("preempt", g_preempt_thresh, g_pre_hist, PREEMPT_BUCKETS);

    /* demo 全部结束：ping-pong 统计已由 worker 打完，抢占延迟也已记录。 */
    __atomic_store_n(&g_demo_done, true, __ATOMIC_RELEASE);
    task_exit();
}

static void rt_waker_fn(void *arg)
{
    (void)arg;

    for (uint32_t i = 0; i < DEMO_ITERS; i++) {
        __atomic_store_n(&g_ping_wake, timer_get_ns(), __ATOMIC_RELEASE);
        task_unblock(g_worker);

        /*
           不能在这里 task_block()：self-IPI 立即抢占后，worker 可能在
           waker 真正睡下之前就运行并把 waker 标记成 READY，随后 waker
           再把自己 BLOCKED，这次唤醒就丢了。用「计数 + 让出」等待，
           对任意抢占时机都成立。
        */
        while (__atomic_load_n(&g_ping_done, __ATOMIC_ACQUIRE) <= i)
            task_yield();
    }

    /*
       ping-pong 结束后再做一次“普通任务 → RT”抢占测试：
       probe FIFO 60 先运行并阻塞，normal hog 随后运行、唤醒 probe。
    */
    uint64_t flags = arch_irq_save();
    g_probe = task_create_kernel_rt("rt-probe", rt_probe_fn, NULL, 60, 0);
    task_t *hog = task_create_affinity("rt-hog", normal_hog_fn, NULL, 10, 0);
    arch_irq_restore(flags);

    if (!g_probe || !hog) {
        KLOG_ERROR("=== RT DEMO: probe/hog create failed ===\n");
        __atomic_store_n(&g_demo_done, true, __ATOMIC_RELEASE);
        task_exit();
    }

    task_exit();
}

void run_rt_demo(void)
{
    KLOG_INFO("=== RT DEMO start (iters=%u, CPU0) ===\n", DEMO_ITERS);

    g_demo_done = false;
    g_probe_done = 0;
    g_ping_done = 0;
    g_lat_min = ~0ULL;
    g_lat_max = 0;
    g_lat_sum = 0;
    g_lat_n = 0;
    g_pre_min = ~0ULL;
    g_pre_max = 0;
    g_pre_sum = 0;
    g_pre_n = 0;

    /*
       抢占测试需要 timer tick 驱动（NORMAL hog 只能被 tick 抢占）。
       先装上，demo 结束再摘掉：调用点早于正常启动路径里的
       timer_set_tick_cb()，不摘会让 boot 尾巴提前进入可抢占状态。
    */
    timer_set_tick_cb(sched_tick);

    /*
       关中断把 worker+waker 的创建放在同一个窗口里：worker 入队后
       need_resched 已置位，不能让 tick 在 g_waker 赋值之前把 worker 放上 CPU。
    */
    uint64_t flags = arch_irq_save();
    g_worker = task_create_kernel_rt("rt-worker", rt_worker_fn, NULL, 50, 0);
    g_waker = task_create_kernel_rt("rt-waker", rt_waker_fn, NULL, 10, 0);
    arch_irq_restore(flags);

    if (!g_worker || !g_waker) {
        KLOG_ERROR("=== RT DEMO: worker/waker create failed ===\n");
        timer_set_tick_cb(NULL);
        return;
    }

    uint64_t deadline = timer_get_ns() + DEMO_TIMEOUT_NS;
    while (!__atomic_load_n(&g_demo_done, __ATOMIC_ACQUIRE) && timer_get_ns() < deadline)
        task_yield();

    timer_set_tick_cb(NULL);

    if (!__atomic_load_n(&g_demo_done, __ATOMIC_ACQUIRE))
        KLOG_ERROR("=== RT DEMO: TIMEOUT (worker/waker did not finish) ===\n");
}
