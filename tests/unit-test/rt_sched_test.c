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
#include "rt/rt_worker.h"
#include "time/hrtimer.h"
#include "mm/mm_vm.h"
#include "string.h"
#include "task/rt_event.h"
#include "rt/rt_latency.h"
#ifdef RT_IRQ_TRACE
#include "task/preempt.h"
#endif

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

static bool rtw_test_wait_ticks(uint32_t id, uint64_t want, uint64_t timeout_ns)
{
    uint64_t deadline = timer_get_ns() + timeout_ns;

    for (;;) {
        struct rtw_stats st;
        if (rt_worker_get_stats(id, &st) == 0 && st.ticks >= want)
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

/*
   场景 5b：观察窗口与 watchdog/设备错误计数。

   event worker 由 rt_worker_post_event() 驱动，不依赖 tick，因此三架构
   都能在自测窗口里拿到非零计数。这里验证：
     - CREATE 后计数为零，RESET 后窗口起点前进、计数清零；
     - out_seq 是状态不是统计量，RESET 不能把它清掉；
     - 共享页里的统计镜像同步清零；
     - watchdog 在目标不推进时 trips/last_trip 增长，RESET 清零
       （只有 oneshot 可用时才能在本自测窗口里实际跑周期）。
   设备错误计数需要能失败的 rtdev；内置 vout 不会失败，这里只确认
   未绑定时计数为零，具体错误路径由驱动自己的测试覆盖。
*/
static bool run_rt_obs_test(void)
{
    bool ok = true;
    const char *phase = "event-create";
    uint32_t ev_id = 0;
    uint32_t target_id = 0;
    uint32_t wd_id = 0;
    bool ev_started = false;
    bool wd_started = false;
    bool wd_tested = false;
    uint64_t wd_checks = 0;
    uint64_t wd_trips = 0;
    struct rtw_stats st;

    struct rtw_create_req cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = RTW_ABI_VERSION;
    cfg.template_id = RTW_TPL_EVENT;
    memcpy(cfg.name, "rtw-obs-ev", 11);
    cfg.priority = 50;
    cfg.cpu = 0;
    cfg.period_ns = 0; /* 无限等待，post 唤醒 */
    cfg.params[0] = 131072;

    int rc = rt_worker_create(&cfg);
    if (rc <= 0) {
        ok = false;
        goto out;
    }
    ev_id = (uint32_t)rc;

    uint64_t shm_pa = 0;
    uint64_t shm_size = 0;
    if (rt_worker_shm_lookup(ev_id, &shm_pa, &shm_size) < 0 || shm_size != PAGE_SIZE ||
        rt_worker_get_stats(ev_id, &st) < 0 || st.ticks != 0 || st.events_posted != 0) {
        ok = false;
        goto out;
    }
    volatile struct rtw_shm *shm = phys_to_virt(shm_pa);
    uint64_t since_create = st.stats_since_ns;

    phase = "event-start";
    if (rt_worker_start(ev_id) != 0) {
        ok = false;
        goto out;
    }
    ev_started = true;
    for (uint32_t i = 0; i < 3; i++) {
        if (rt_worker_post_event(ev_id, 21) != 0) {
            ok = false;
            goto out;
        }
    }

    phase = "event-run";
    if (!rtw_test_wait_ticks(ev_id, 3, RTW_TEST_WAIT_NS) || rt_worker_get_stats(ev_id, &st) < 0 || st.ticks < 3 ||
        st.events_posted != 3 || st.events_processed != 3) {
        ok = false;
        goto out;
    }

    /* 运行中重置：只要求窗口起点不倒退；并发中的一次更新允许落在新窗口。 */
    phase = "reset-running";
    if (rt_worker_reset_stats(ev_id) != 0 || rt_worker_get_stats(ev_id, &st) < 0 || st.stats_since_ns < since_create) {
        ok = false;
        goto out;
    }

    phase = "event-stop";
    if (rt_worker_stop(ev_id) != 0 || !rtw_test_wait_state(ev_id, RTW_STATE_STOPPED, RTW_TEST_WAIT_NS)) {
        ok = false;
        goto out;
    }
    ev_started = false;

    /* 停止后重置：这次必须精确清零，且不能动 out_seq/output。 */
    phase = "reset-stopped";
    if (rt_worker_reset_stats(ev_id) != 0 || rt_worker_get_stats(ev_id, &st) < 0) {
        ok = false;
        goto out;
    }
    if (st.ticks != 0 || st.misses != 0 || st.overruns != 0 || st.exec_last_ns != 0 || st.exec_max_ns != 0 ||
        st.wake_late_last_ns != 0 || st.wake_late_max_ns != 0 || st.dev_writes != 0 || st.dev_errors != 0 ||
        st.dev_last_error != 0 || st.events_posted != 0 || st.events_processed != 0 || st.events_dropped != 0 ||
        st.wd_checks != 0 || st.wd_trips != 0 || st.wd_last_trip_ns != 0) {
        ok = false;
        goto out;
    }
    if (st.out_seq < 3 || st.stats_since_ns < since_create || __atomic_load_n(&shm->ticks, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&shm->misses, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&shm->overruns, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&shm->dev_writes, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&shm->out_seq, __ATOMIC_ACQUIRE) != st.out_seq) {
        ok = false;
        goto out;
    }

    phase = "event-destroy";
    if (rt_worker_destroy(ev_id) != 0) {
        ok = false;
        goto out;
    }
    ev_id = 0;

    /*
       watchdog：只在 oneshot 架构上实际跑周期。目标 worker 只 create
       不 start，out_seq 永远不推进，watchdog 第二轮就会切到安全值。
    */
    if (!hrtimer_cpu_oneshot_supported(0))
        goto out;

    wd_tested = true;
    uint64_t period = 1000000ULL;

    phase = "wd-target-create";
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = RTW_ABI_VERSION;
    cfg.template_id = RTW_TPL_PERIODIC_IO;
    memcpy(cfg.name, "rtw-obs-tgt", 12);
    cfg.priority = 50;
    cfg.cpu = 0;
    cfg.period_ns = period;
    cfg.params[0] = 65536;
    rc = rt_worker_create(&cfg);
    if (rc <= 0) {
        ok = false;
        goto out;
    }
    target_id = (uint32_t)rc;

    phase = "wd-create";
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = RTW_ABI_VERSION;
    cfg.template_id = RTW_TPL_WATCHDOG;
    memcpy(cfg.name, "rtw-obs-wd", 11);
    cfg.priority = 60;
    cfg.cpu = 0;
    cfg.period_ns = period;
    cfg.params[0] = target_id;
    cfg.params[1] = period / 2; /* 目标不动，第二轮必超时 */
    cfg.params[2] = 999;
    rc = rt_worker_create(&cfg);
    if (rc <= 0) {
        ok = false;
        goto out;
    }
    wd_id = (uint32_t)rc;

    phase = "wd-start";
    if (rt_worker_start(wd_id) != 0) {
        ok = false;
        goto out;
    }
    wd_started = true;

    phase = "wd-run";
    if (!rtw_test_wait_ticks(wd_id, 3, RTW_TEST_WAIT_NS) || rt_worker_get_stats(wd_id, &st) < 0 || st.wd_checks < 3 ||
        st.wd_trips < 1 || st.wd_last_trip_ns == 0 || st.last_output != 999) {
        ok = false;
        goto out;
    }
    wd_checks = st.wd_checks;
    wd_trips = st.wd_trips;

    phase = "wd-stop";
    if (rt_worker_stop(wd_id) != 0 || !rtw_test_wait_state(wd_id, RTW_STATE_STOPPED, RTW_TEST_WAIT_NS)) {
        ok = false;
        goto out;
    }
    wd_started = false;

    phase = "wd-reset";
    if (rt_worker_reset_stats(wd_id) != 0 || rt_worker_get_stats(wd_id, &st) < 0 || st.wd_checks != 0 ||
        st.wd_trips != 0 || st.wd_last_trip_ns != 0) {
        ok = false;
        goto out;
    }

    phase = "wd-destroy";
    if (rt_worker_destroy(wd_id) != 0 || rt_worker_destroy(target_id) != 0) {
        ok = false;
        goto out;
    }
    wd_id = 0;
    target_id = 0;

out:
    if (wd_id != 0) {
        if (wd_started) {
            rt_worker_stop(wd_id);
            rtw_test_wait_state(wd_id, RTW_STATE_STOPPED, RTW_TEST_WAIT_NS);
        }
        rt_worker_destroy(wd_id);
    }
    if (target_id != 0)
        rt_worker_destroy(target_id);
    if (ev_id != 0) {
        if (ev_started) {
            rt_worker_stop(ev_id);
            rtw_test_wait_state(ev_id, RTW_STATE_STOPPED, RTW_TEST_WAIT_NS);
        }
        rt_worker_destroy(ev_id);
    }

    if (ok) {
        KLOG_INFO("=== RT OBS TEST: PASS reset=1 wd_tested=%d wd_checks=%llu wd_trips=%llu ===\n", (int)wd_tested,
                  (unsigned long long)wd_checks, (unsigned long long)wd_trips);
    } else {
        KLOG_ERROR("=== RT OBS TEST: FAIL (%s) ===\n", phase);
    }
    return ok;
}

/* ── 跨核迁移计数自测（SMP>1）────────────────────────────── */

static rt_event_t g_mig_evt;
static volatile uint32_t g_mig_done;

static void migration_probe(void *arg)
{
    (void)arg;
    (void)rt_event_wait(&g_mig_evt, 0x1U, false, 0, true);
    __atomic_store_n(&g_mig_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

/*
   SMP>1 时强制一次跨核迁移：
     probe 先在 CPU0 运行并阻塞 → 把 affinity 改成 CPU1 → set event 唤醒。
   它首次在 CPU1 被切入时，sched 的 last_run_cpu 是 0，迁移计数应 +1。
*/
static bool run_migration_test(void)
{
    if (g_num_cpus < 2)
        return true;

    rt_event_init(&g_mig_evt);
    g_mig_done = 0;

    uint64_t flags = arch_irq_save();
    task_t *probe = task_create_kernel_rt("rt-mig", migration_probe, NULL, 60, 0);
    arch_irq_restore(flags);
    if (!probe) {
        KLOG_ERROR("=== RT MIGRATION TEST: FAIL (task create) ===\n");
        return false;
    }

    uint64_t deadline = timer_get_ns() + 2000000000ULL;
    while (probe->state != TASK_BLOCKED && timer_get_ns() < deadline)
        task_yield();
    if (probe->state != TASK_BLOCKED) {
        KLOG_ERROR("=== RT MIGRATION TEST: FAIL (probe not blocked) ===\n");
        rt_event_set(&g_mig_evt, 0x1U);
        return false;
    }

    uint64_t before = rt_latency_migration_count();
    probe->cpu_affinity = 1U;
    rt_event_set(&g_mig_evt, 0x1U);

    deadline = timer_get_ns() + 2000000000ULL;
    while (!__atomic_load_n(&g_mig_done, __ATOMIC_ACQUIRE) && timer_get_ns() < deadline)
        task_yield();
    if (!__atomic_load_n(&g_mig_done, __ATOMIC_ACQUIRE)) {
        KLOG_ERROR("=== RT MIGRATION TEST: FAIL (timeout) ===\n");
        return false;
    }

    uint64_t after = rt_latency_migration_count();
    if (after <= before) {
        KLOG_ERROR("=== RT MIGRATION TEST: FAIL (migrations=%llu->%llu) ===\n", (unsigned long long)before,
                   (unsigned long long)after);
        return false;
    }

    KLOG_INFO("=== RT MIGRATION TEST: PASS migrations=%llu->%llu ===\n", (unsigned long long)before,
              (unsigned long long)after);
    return true;
}

#ifdef RT_IRQ_TRACE
/*
   诊断自测：RT_IRQ_TRACE=1 时 preempt_disable/enable 在真实调度路径上
   被调用。这里直接持一段 50µs 的 preempt-off，确认最外层计数 +1 且
   max 至少覆盖这段区间。普通编译不包含这段代码。
*/
static bool run_preemptoff_test(void)
{
    /*
       关中断做整段测量：否则 preempt_enable 之后、读计数之前的一次
       timer IRQ 也可能拿 spinlock，让 count 多涨几次，断言就不稳。
    */
    uint64_t flags = arch_irq_save();
    uint64_t count_before = rt_latency_preempt_off_count();

    preempt_disable();
    uint64_t t0 = timer_get_ns();
    while (timer_get_ns() - t0 < 50000)
        ;
    uint64_t t1 = timer_get_ns();
    preempt_enable();

    uint64_t count_after = rt_latency_preempt_off_count();
    uint64_t max = rt_latency_preempt_off_max_ns();
    uint64_t held = t1 - t0;
    arch_irq_restore(flags);

    if (count_after != count_before + 1 || max < held) {
        KLOG_ERROR("=== PREEMPTOFF TEST: FAIL count=%llu->%llu held=%llu max=%llu ===\n",
                   (unsigned long long)count_before, (unsigned long long)count_after, (unsigned long long)held,
                   (unsigned long long)max);
        return false;
    }

    KLOG_INFO("=== PREEMPTOFF TEST: PASS count=%llu held=%llu max=%llu ===\n", (unsigned long long)count_after,
              (unsigned long long)held, (unsigned long long)max);
    return true;
}
#endif

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

        if (!run_rt_obs_test()) {
            KLOG_ERROR("=== RT SCHED TEST: FAIL (observability) ===\n");
            return;
        }

        if (!run_migration_test()) {
            KLOG_ERROR("=== RT SCHED TEST: FAIL (migration) ===\n");
            return;
        }

        extern void run_rt_mutex_tests(void);
        run_rt_mutex_tests();

        extern void run_rt_event_tests(void);
        run_rt_event_tests();

        /*
           cyclictest 放在最后：它自己安装/摘除 tick 回调，并占用 CPU0
           做 1ms/10ms 周期 + NORMAL hog 的测量。返回 false 只表示没跑完，
           延迟阈值由 tests/suites/rt_test.py 按架构判。
        */
        extern bool run_cyclictest(void);
        if (!run_cyclictest()) {
            KLOG_ERROR("=== RT SCHED TEST: FAIL (cyclictest) ===\n");
            return;
        }

        /* oslat：RT 忙循环的连续执行间隔（IRQ/tick/宿主噪声）。 */
        extern bool run_oslat(void);
        if (!run_oslat()) {
            KLOG_ERROR("=== RT SCHED TEST: FAIL (oslat) ===\n");
            return;
        }

#ifdef RT_IRQ_TRACE
        if (!run_preemptoff_test()) {
            KLOG_ERROR("=== RT SCHED TEST: FAIL (preemptoff) ===\n");
            return;
        }
#endif

        KLOG_INFO("=== RT SCHED TEST: PASS ===\n");
    } else {
        KLOG_ERROR("=== RT SCHED TEST: FAIL (immediate preemption) ===\n");
    }
}
