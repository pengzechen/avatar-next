/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit-test/rt_throttle_test.c — RT 带宽限制（RT throttling）自测

   策略：每个 1s 周期内 RT（含被 PI 提升的任务）最多运行 950ms，超出后
   本核暂时跳过 RT 队列，让 NORMAL / idle 拿到 CPU，下一个周期再放行。

   测例：
     - NORMAL(10) 和 FIFO(80) 两个任务同时就绪，各自纯忙循环到 2.5s 截止；
     - 没有 throttling：FIFO 80 独占 CPU，NORMAL 的计数为 0；
     - 有 throttling：RT 每个周期被压掉最后约 50ms，NORMAL 至少能拿到
       RT 迭代数的 1%（实际约 5%）。

   注意两个任务都在 deadline 自行退出，所以即使 throttling 完全失效，
   boot/idle 也一定会在 2.5s 后回来，测试只会失败、不会挂死。
*/

#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"
#include "driver/time.h"
#include "klog.h"

#define THR_WINDOW_NS 2500000000ULL /* 2.5 个默认周期 */

static volatile uint64_t g_thr_deadline_ns;
static volatile uint64_t g_thr_rt_iters;
static volatile uint64_t g_thr_normal_iters;
static volatile uint32_t g_thr_done;

static void thr_rt_fn(void *arg)
{
    (void)arg;

    while (timer_get_ns() < g_thr_deadline_ns)
        g_thr_rt_iters++;

    __atomic_fetch_add(&g_thr_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void thr_normal_fn(void *arg)
{
    (void)arg;

    while (timer_get_ns() < g_thr_deadline_ns)
        g_thr_normal_iters++;

    __atomic_fetch_add(&g_thr_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

bool run_rt_throttle_test(void)
{
    /*
       从干净的周期/预算开始：前面的 RT 用例也跑过 RT，残留的 rt_time 会
       让周期边界不对齐，测例可能刚好卡在周期末尾而看不到让出窗口。
    */
    sched_rt_throttle_set(1000000000ULL, 950000000ULL);

    /*
       RT 自测窗口里 g_tick_cb 是 NULL（前面的用例摘掉了），而 throttling
       必须在 tick 上记账/置 need_resched。这里像 cyclictest 一样临时安装，
       测完再摘掉。
    */
    timer_set_tick_cb(sched_tick);

    g_thr_deadline_ns = timer_get_ns() + THR_WINDOW_NS;
    g_thr_rt_iters = 0;
    g_thr_normal_iters = 0;
    g_thr_done = 0;

    task_t *normal = task_create_affinity("thr-norm", thr_normal_fn, NULL, 10, 0);
    task_t *rt = task_create_kernel_rt("thr-rt", thr_rt_fn, NULL, 80, 0);
    if (!normal || !rt) {
        KLOG_ERROR("=== RT THROTTLE TEST: FAIL (task create) ===\n");
        return false;
    }

    /*
       NORMAL 先创建、RT 后创建；RT 一就绪就抢占。两个任务都纯忙循环，
       boot/idle 只会在它们都到达 deadline 退出后才回来。
    */
    while (__atomic_load_n(&g_thr_done, __ATOMIC_ACQUIRE) < 2U)
        task_yield();

    uint64_t rt_iters = g_thr_rt_iters;
    uint64_t normal_iters = g_thr_normal_iters;
    uint64_t events = cpu_current()->rt_throttle_events;

    /*
       测完恢复默认配置并重置记账，避免残余的 rt_time / throttled 状态
       影响后面的 cyclictest / oslat。
    */
    timer_set_tick_cb(NULL);

    sched_rt_throttle_set(1000000000ULL, 950000000ULL);

    /*
       无 throttling：normal_iters == 0、events == 0。
       有 throttling：每个周期 RT 让出最后约 5%，normal_iters 至少应为
       rt_iters 的 1%，且至少进入过一次限流。
    */
    bool ok = rt_iters > 0 && normal_iters > 0 && events > 0 && normal_iters * 100ULL >= rt_iters;

    KLOG_INFO("=== RT THROTTLE TEST: %s rt_iters=%llu normal_iters=%llu events=%llu ===\n", ok ? "PASS" : "FAIL",
              (unsigned long long)rt_iters, (unsigned long long)normal_iters, (unsigned long long)events);
    return ok;
}
