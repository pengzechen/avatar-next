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
#include "rt_worker.h"
#include "hrtimer.h"
#include "mm_vm.h"
#include "string.h"

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

/* ── 场景 5：模板 worker + 共享页（/dev/rtctl 的内核侧）──────── */

#define RTW_TEST_WAIT_NS   2000000000ULL /* 单个阶段的最长等待 */
#define RTW_TEST_WINDOW_NS 200000000ULL  /* 周期频率统计窗口 */

static bool rtw_test_wait_seq(volatile struct rtw_shm *shm, uint64_t want, uint64_t timeout_ns)
{
    uint64_t deadline = timer_get_ns() + timeout_ns;

    while (__atomic_load_n(&shm->out_seq, __ATOMIC_ACQUIRE) < want) {
        if (timer_get_ns() >= deadline)
            return false;
        task_yield();
    }
    return true;
}

static bool rtw_test_wait_state(uint32_t id, uint32_t want, uint64_t timeout_ns)
{
    uint64_t deadline = timer_get_ns() + timeout_ns;

    for (;;) {
        struct rtw_stats st;
        if (rt_worker_get_stats(id, &st) == 0 && st.state == want)
            return true;
        if (timer_get_ns() >= deadline)
            return false;
        task_yield();
    }
}

/*
   验证：create → 共享页 → start → 1ms/10ms 周期 → 运行期参数 →
   stop → destroy。x86 oneshot 可用时按 1ms 跑，否则按 10ms tick 跑。
   共享页在 destroy 后仍可读（页不归还 PMM），但 magic 必须清零。
*/
static void run_rt_worker_test(void)
{
    bool ok = true;
    bool started = false;
    const char *phase = "create";
    uint32_t id = 0;
    volatile struct rtw_shm *shm = NULL;
    uint64_t last_ticks = 0;
    uint64_t last_exec_max = 0;
    uint64_t last_misses = 0;
    /*
       只有 oneshot 能在这个早期自测窗口里兑现周期睡眠：tick 回调要到
       kernel_main 末尾（所有任务创建完之后）才启用，aarch64/riscv 这段
       窗口里 g_system_ticks 还是 0。没有 oneshot 的架构只验证
       create/共享页/start/stop/destroy 的生命周期，不验证周期频率。
    */
    bool periodic = hrtimer_cpu_oneshot_supported(0);
    uint64_t period = periodic ? 1000000ULL : 10000000ULL;

    timer_set_tick_cb(sched_tick);

    struct rtw_create_req cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = RTW_ABI_VERSION;
    cfg.template_id = RTW_TPL_PERIODIC_IO;
    memcpy(cfg.name, "rtw-gate", 9);
    cfg.priority = 50;
    cfg.cpu = 0;
    cfg.period_ns = period;
    cfg.budget_ns = period / 2;
    cfg.params[0] = 131072; /* gain = 2.0 */

    int rc = rt_worker_create(&cfg);
    if (rc <= 0) {
        ok = false;
        goto out;
    }
    id = (uint32_t)rc;

    uint64_t shm_pa = 0;
    uint64_t shm_size = 0;
    phase = "shm";
    if (rt_worker_shm_lookup(id, &shm_pa, &shm_size) < 0 || shm_size != PAGE_SIZE) {
        ok = false;
        goto out;
    }
    shm = phys_to_virt(shm_pa);
    if (shm->magic != RTSHM_MAGIC || shm->version != RTSHM_VERSION || shm->params[0] != 131072) {
        ok = false;
        goto out;
    }
    __atomic_store_n(&shm->input, 21, __ATOMIC_RELEASE);

    phase = "start";
    if (rt_worker_start(id) != 0) {
        ok = false;
        goto out;
    }
    started = true;

    phase = "run";
    struct rtw_stats st;
    if (periodic) {
        if (!rtw_test_wait_seq(shm, 5, RTW_TEST_WAIT_NS) || __atomic_load_n(&shm->output, __ATOMIC_ACQUIRE) != 42) {
            ok = false;
            goto out;
        }
        if (rt_worker_get_stats(id, &st) < 0 || st.state != RTW_STATE_RUNNING || st.ticks < 5) {
            ok = false;
            goto out;
        }
    } else if (rt_worker_get_stats(id, &st) < 0 || st.state != RTW_STATE_RUNNING ||
               __atomic_load_n(&shm->state, __ATOMIC_ACQUIRE) != RTW_STATE_RUNNING) {
        ok = false;
        goto out;
    }

    /* 运行期参数：gain 2.0 → 3.0，共享页镜像必须同步。 */
    phase = "params";
    uint64_t params[RTW_PARAM_MAX];
    memset(params, 0, sizeof(params));
    params[0] = 196608;
    if (rt_worker_set_params(id, params) != 0 || __atomic_load_n(&shm->params[0], __ATOMIC_ACQUIRE) != 196608 ||
        (__atomic_load_n(&shm->params_seq, __ATOMIC_ACQUIRE) & 1U) != 0) {
        ok = false;
        goto out;
    }

    if (periodic) {
        /* 新参数在下一个周期生效：输入 21 × gain 3.0 = 63。 */
        uint64_t base = __atomic_load_n(&shm->out_seq, __ATOMIC_ACQUIRE);
        if (!rtw_test_wait_seq(shm, base + 3, RTW_TEST_WAIT_NS) ||
            __atomic_load_n(&shm->output, __ATOMIC_ACQUIRE) != 63) {
            ok = false;
            goto out;
        }

        /* 频率门禁：一个窗口内至少完成理论周期的 3/4。 */
        phase = "rate";
        uint64_t win_start = timer_get_ns();
        uint64_t tick_start = __atomic_load_n(&shm->ticks, __ATOMIC_ACQUIRE);
        while (timer_get_ns() - win_start < RTW_TEST_WINDOW_NS)
            task_yield();
        uint64_t win_ns = timer_get_ns() - win_start;
        uint64_t win_ticks = __atomic_load_n(&shm->ticks, __ATOMIC_ACQUIRE) - tick_start;
        if (win_ticks < (win_ns / period) * 3 / 4) {
            KLOG_ERROR("=== RT WORKER TEST: rate %llu ticks in %llu ns (period %llu) ===\n",
                       (unsigned long long)win_ticks, (unsigned long long)win_ns, (unsigned long long)period);
            ok = false;
            goto out;
        }
    }

    last_ticks = __atomic_load_n(&shm->ticks, __ATOMIC_ACQUIRE);
    last_exec_max = __atomic_load_n(&shm->exec_max_ns, __ATOMIC_ACQUIRE);
    last_misses = __atomic_load_n(&shm->misses, __ATOMIC_ACQUIRE);

    phase = "stop";
    if (rt_worker_stop(id) != 0 || !rtw_test_wait_state(id, RTW_STATE_STOPPED, RTW_TEST_WAIT_NS) ||
        __atomic_load_n(&shm->state, __ATOMIC_ACQUIRE) != RTW_STATE_STOPPED) {
        ok = false;
        goto out;
    }
    started = false;

    phase = "destroy";
    if (rt_worker_destroy(id) != 0) {
        ok = false;
        goto out;
    }
    uint32_t destroyed_id = id;
    id = 0;
    uint64_t dummy_pa = 0;
    uint64_t dummy_size = 0;
    if (rt_worker_shm_lookup(destroyed_id, &dummy_pa, &dummy_size) >= 0 || shm->magic != 0 ||
        __atomic_load_n(&shm->state, __ATOMIC_ACQUIRE) != RTW_STATE_FREE) {
        ok = false;
        goto out;
    }

out:
    if (id != 0) {
        if (started) {
            rt_worker_stop(id);
            rtw_test_wait_state(id, RTW_STATE_STOPPED, RTW_TEST_WAIT_NS);
        }
        rt_worker_destroy(id);
    }

    if (ok) {
        KLOG_INFO("=== RT WORKER TEST: PASS period=%llu periodic=%d ticks=%llu misses=%llu exec_max=%llu ===\n",
                  (unsigned long long)period, (int)periodic, (unsigned long long)last_ticks,
                  (unsigned long long)last_misses, (unsigned long long)last_exec_max);
    } else {
        KLOG_ERROR("=== RT WORKER TEST: FAIL (%s) ===\n", phase);
    }

    timer_set_tick_cb(NULL);
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
        run_rt_worker_test();

        extern void run_rt_mutex_tests(void);
        run_rt_mutex_tests();

        extern void run_rt_event_tests(void);
        run_rt_event_tests();

        KLOG_INFO("=== RT SCHED TEST: PASS ===\n");
    } else {
        KLOG_ERROR("=== RT SCHED TEST: FAIL (immediate preemption) ===\n");
    }
}
