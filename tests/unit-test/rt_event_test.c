/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit-test/rt_event_test.c — rt_event / event 模板自测（RT_TEST=1）

   1. rt_event 基本语义：非阻塞命中、clear、任意位/全部位。
   2. 多个等待者的 mask 过滤：set 0x1 只唤醒等 0x1 的任务，等 0x2 的
      继续睡，直到 set 0x2。
   3. 超时：仅在 hrtimer oneshot 可用时测试（早期自测窗口里 aarch64/
      riscv64 的 tick 回调尚未启用）。
   4. event 模板：共享事件环容量 64。先投 100 条（64 入环、36 drop），
      start 后处理 64 条并验证输出；再投 20 条，验证继续处理和统计；
      最后在 worker 阻塞于 rt_event_wait 时 stop，必须能及时退出。

   所有任务钉 CPU0。事件唤醒走 sched_unblock/self-IPI，不依赖 tick。
*/

#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"
#include "task/rt_event.h"
#include "rt_worker.h"
#include "hrtimer.h"
#include "mm_vm.h"
#include "driver/time.h"
#include "klog.h"
#include "string.h"

/* ── 直接等待 rt_event 的任务 ─────────────────────────────── */

static rt_event_t g_evt;
static volatile uint32_t g_evt_a_entered;
static volatile uint32_t g_evt_b_entered;
static volatile uint32_t g_evt_c_entered;
static volatile uint32_t g_evt_a_done;
static volatile uint32_t g_evt_b_done;
static volatile uint32_t g_evt_c_done;
static volatile uint32_t g_evt_a_rc;
static volatile uint32_t g_evt_b_rc;
static volatile uint32_t g_evt_c_rc;

static void evt_waiter_a(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_evt_a_entered, 1U, __ATOMIC_RELEASE);
    g_evt_a_rc = rt_event_wait(&g_evt, 0x1U, false, 0, false);
    __atomic_store_n(&g_evt_a_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void evt_waiter_b(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_evt_b_entered, 1U, __ATOMIC_RELEASE);
    g_evt_b_rc = rt_event_wait(&g_evt, 0x2U, false, 0, false);
    __atomic_store_n(&g_evt_b_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void evt_waiter_c(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_evt_c_entered, 1U, __ATOMIC_RELEASE);
    g_evt_c_rc = rt_event_wait(&g_evt, 0x3U, true, 0, true);
    __atomic_store_n(&g_evt_c_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

/* 超时任务：只测返回值与大致耗时 */
static volatile uint32_t g_evt_tmo_done;
static volatile uint32_t g_evt_tmo_rc;
static volatile uint64_t g_evt_tmo_elapsed;

static void evt_timeout_task(void *arg)
{
    (void)arg;
    uint64_t t0 = timer_get_ns();
    g_evt_tmo_rc = rt_event_wait(&g_evt, 0x1U, false, 20000000ULL, false);
    uint64_t t1 = timer_get_ns();
    g_evt_tmo_elapsed = (t1 > t0) ? (t1 - t0) : 0;
    __atomic_store_n(&g_evt_tmo_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static bool wait_flag(volatile uint32_t *flag, uint64_t timeout_ns)
{
    uint64_t deadline = timer_get_ns() + timeout_ns;
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == 0) {
        if (timer_get_ns() >= deadline)
            return false;
        task_yield();
    }
    return true;
}

static bool wait_blocked(task_t *t, uint64_t timeout_ns)
{
    uint64_t deadline = timer_get_ns() + timeout_ns;
    while (t->state != TASK_BLOCKED) {
        if (timer_get_ns() >= deadline)
            return false;
        task_yield();
    }
    return true;
}

/* ── rt_event 语义测试 ───────────────────────────────────── */

static bool run_rt_event_sync_tests(uint32_t *timeout_ran)
{
    *timeout_ran = 0;

    /* 基本语义：预置位 → wait 立即满足并 clear */
    rt_event_init(&g_evt);
    rt_event_set(&g_evt, 0x1U | 0x2U);
    if (rt_event_get(&g_evt) != 0x3U)
        return false;
    if (rt_event_wait(&g_evt, 0x1U, false, 0, true) != 0x1U)
        return false;
    if (rt_event_get(&g_evt) != 0x2U)
        return false;
    rt_event_clear(&g_evt, 0x2U);
    if (rt_event_get(&g_evt) != 0)
        return false;

    /* mask 过滤：set 0x1 只唤醒 A，B 继续等 0x2 */
    rt_event_init(&g_evt);
    g_evt_a_entered = g_evt_b_entered = 0;
    g_evt_a_done = g_evt_b_done = 0;
    g_evt_a_rc = g_evt_b_rc = 0;

    task_t *ta = task_create_kernel_rt("evt-a", evt_waiter_a, NULL, 60, 0);
    task_t *tb = task_create_kernel_rt("evt-b", evt_waiter_b, NULL, 60, 0);
    if (!ta || !tb || !wait_flag(&g_evt_a_entered, 2000000000ULL) || !wait_flag(&g_evt_b_entered, 2000000000ULL) ||
        !wait_blocked(ta, 2000000000ULL) || !wait_blocked(tb, 2000000000ULL))
        return false;

    rt_event_set(&g_evt, 0x1U);
    if (!wait_flag(&g_evt_a_done, 2000000000ULL) || g_evt_a_rc != 0x1U)
        return false;
    for (int i = 0; i < 32; i++)
        task_yield();
    if (__atomic_load_n(&g_evt_b_done, __ATOMIC_ACQUIRE) != 0)
        return false;

    rt_event_set(&g_evt, 0x2U);
    if (!wait_flag(&g_evt_b_done, 2000000000ULL) || g_evt_b_rc != 0x2U)
        return false;

    /* all 语义 + clear：0x1 单独不满足，0x3 才唤醒 C，并清掉 0x3 */
    rt_event_init(&g_evt);
    g_evt_c_entered = g_evt_c_done = 0;
    g_evt_c_rc = 0;
    task_t *tc = task_create_kernel_rt("evt-c", evt_waiter_c, NULL, 60, 0);
    if (!tc || !wait_flag(&g_evt_c_entered, 2000000000ULL) || !wait_blocked(tc, 2000000000ULL))
        return false;
    rt_event_set(&g_evt, 0x1U);
    for (int i = 0; i < 32; i++)
        task_yield();
    if (__atomic_load_n(&g_evt_c_done, __ATOMIC_ACQUIRE) != 0)
        return false;
    rt_event_set(&g_evt, 0x2U);
    if (!wait_flag(&g_evt_c_done, 2000000000ULL) || g_evt_c_rc != 0x3U)
        return false;
    if (rt_event_get(&g_evt) != 0)
        return false;

    /*
       超时依赖 sleep_expire_ns 的定时唤醒；早期 RT 自测窗口里 tick 回调
       还没启用，aarch64/riscv64 只能跳过。
    */
    if (hrtimer_cpu_oneshot_supported(0)) {
        rt_event_init(&g_evt);
        g_evt_tmo_done = 0;
        g_evt_tmo_rc = 0xFFFFFFFFU;
        g_evt_tmo_elapsed = 0;
        task_t *td = task_create_kernel_rt("evt-tmo", evt_timeout_task, NULL, 60, 0);
        if (!td || !wait_flag(&g_evt_tmo_done, 2000000000ULL))
            return false;
        if (g_evt_tmo_rc != 0)
            return false;
        if (g_evt_tmo_elapsed < 10000000ULL || g_evt_tmo_elapsed > 1000000000ULL)
            return false;
        *timeout_ran = 1;
    }

    return true;
}

/* ── event 模板 + 共享事件环 ─────────────────────────────── */

static bool run_event_template_test(uint64_t *posted, uint64_t *processed, uint64_t *dropped)
{
    struct rtw_create_req cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = RTW_ABI_VERSION;
    cfg.template_id = RTW_TPL_EVENT;
    memcpy(cfg.name, "evt-gate", 9);
    cfg.priority = 50;
    cfg.cpu = 0;
    cfg.period_ns = 0; /* 无限等待事件 */
    cfg.budget_ns = 0;
    cfg.params[0] = 131072; /* gain = 2.0 */

    int rc = rt_worker_create(&cfg);
    if (rc <= 0)
        return false;
    uint32_t id = (uint32_t)rc;

    uint64_t pa = 0, sz = 0;
    if (rt_worker_shm_lookup(id, &pa, &sz) < 0 || sz != PAGE_SIZE) {
        rt_worker_destroy(id);
        return false;
    }
    struct rtw_shm *shm = phys_to_virt(pa);
    struct rtw_event_ring *ev = rtw_shm_events(shm);

    /*
       先投递、后 start：容量 64，100 条里前 64 条入环，后 36 条必须
       以 -ENOSPC 丢弃并计数。这样 drop 行为是确定性的，不靠时序。
    */
    for (uint64_t i = 1; i <= 100; i++)
        (void)rt_worker_post_event(id, i);

    if (ev->posted != 64 || ev->dropped != 36) {
        rt_worker_destroy(id);
        return false;
    }

    if (rt_worker_start(id) != 0) {
        rt_worker_destroy(id);
        return false;
    }

    uint64_t deadline = timer_get_ns() + 5000000000ULL;
    while (__atomic_load_n(&ev->processed, __ATOMIC_ACQUIRE) < 64 && timer_get_ns() < deadline)
        task_yield();
    if (__atomic_load_n(&ev->processed, __ATOMIC_ACQUIRE) != 64 ||
        __atomic_load_n(&shm->output, __ATOMIC_ACQUIRE) != 128) {
        rt_worker_stop(id);
        rt_worker_destroy(id);
        return false;
    }

    /* 再投 20 条：worker 此时阻塞在 rt_event_wait，事件必须立即唤醒 */
    for (uint64_t i = 101; i <= 120; i++) {
        if (rt_worker_post_event(id, i) != 0) {
            rt_worker_stop(id);
            rt_worker_destroy(id);
            return false;
        }
    }

    deadline = timer_get_ns() + 5000000000ULL;
    while (__atomic_load_n(&ev->processed, __ATOMIC_ACQUIRE) < 84 && timer_get_ns() < deadline)
        task_yield();
    if (__atomic_load_n(&ev->processed, __ATOMIC_ACQUIRE) != 84 ||
        __atomic_load_n(&shm->ticks, __ATOMIC_ACQUIRE) != 84 ||
        __atomic_load_n(&shm->output, __ATOMIC_ACQUIRE) != 240) {
        rt_worker_stop(id);
        rt_worker_destroy(id);
        return false;
    }

    struct rtw_stats st;
    if (rt_worker_get_stats(id, &st) < 0) {
        rt_worker_stop(id);
        rt_worker_destroy(id);
        return false;
    }
    *posted = st.events_posted;
    *processed = st.events_processed;
    *dropped = st.events_dropped;

    /* worker 此时阻塞在无限等待；stop 必须能把它叫醒并退出 */
    if (rt_worker_stop(id) != 0) {
        rt_worker_destroy(id);
        return false;
    }
    deadline = timer_get_ns() + 2000000000ULL;
    for (;;) {
        if (rt_worker_get_stats(id, &st) == 0 && st.state == RTW_STATE_STOPPED)
            break;
        if (timer_get_ns() >= deadline) {
            rt_worker_destroy(id);
            return false;
        }
        task_yield();
    }

    if (rt_worker_destroy(id) != 0)
        return false;
    uint64_t dummy_pa = 0, dummy_sz = 0;
    if (rt_worker_shm_lookup(id, &dummy_pa, &dummy_sz) >= 0 || shm->magic != 0)
        return false;

    return *posted == 84 && *processed == 84 && *dropped == 36;
}

/* ── 入口 ─────────────────────────────────────────────────── */

void run_rt_event_tests(void)
{
    const char *phase = "event-sync";
    uint32_t timeout_ran = 0;
    uint64_t posted = 0, processed = 0, dropped = 0;
    bool ok = true;

    if (!run_rt_event_sync_tests(&timeout_ran)) {
        ok = false;
        goto out;
    }

    phase = "event-template";
    if (!run_event_template_test(&posted, &processed, &dropped)) {
        ok = false;
        goto out;
    }

out:
    if (ok) {
        KLOG_INFO("=== RT EVENT TEST: PASS timeout=%u posted=%llu processed=%llu dropped=%llu ===\n",
                  (unsigned)timeout_ran, (unsigned long long)posted, (unsigned long long)processed,
                  (unsigned long long)dropped);
    } else {
        KLOG_ERROR("=== RT EVENT TEST: FAIL (%s) timeout=%u posted=%llu processed=%llu dropped=%llu ===\n", phase,
                   (unsigned)timeout_ran, (unsigned long long)posted, (unsigned long long)processed,
                   (unsigned long long)dropped);
    }
}
