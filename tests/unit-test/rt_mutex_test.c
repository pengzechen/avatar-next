/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit-test/rt_mutex_test.c — mutex / rt_mutex 自测（RT_TEST=1）

   1. 普通 mutex：基本语义（trylock / 可重入 / 解锁），
      以及多任务竞争下的互斥性。检测手段是临界区内的原子计数器：
      如果两个任务同时进入，inside 会看到 >1。
   2. rt_mutex：直接优先级继承。让低优先级 owner 持锁、高优先级任务
      等待，断言 owner 的 pi_priority 被提升到等待者的优先级；
      再让中等优先级任务就绪，验证 owner 以提升后的优先级先释放锁，
      等待者按优先级顺序获得锁，最后 owner 的优先级恢复。

   所有任务钉在 CPU0，避免 SMP 下并行让顺序断言失去意义。
   本文件由 rt_sched_test.c 在 RT_TEST 变体里调用。
*/

#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"
#include "task/mutex.h"
#include "task/rt_mutex.h"
#include "task/rt_event.h"
#include "driver/time.h"
#include "exception.h"
#include "klog.h"

/* ── 1. 普通 mutex ────────────────────────────────────────── */

#define MTX_STRESS_TASKS 6
#define MTX_STRESS_ITERS 3000

static mutex_t g_rtmtest_mutex;

static volatile uint32_t g_rtmtest_counter;
static volatile uint32_t g_rtmtest_inside;
static volatile uint32_t g_rtmtest_errors;
static volatile uint32_t g_rtmtest_done;
static uint32_t g_rtmtest_ids[MTX_STRESS_TASKS];

static void mtx_stress_task(void *arg)
{
    (void)arg;

    for (uint32_t i = 0; i < MTX_STRESS_ITERS; i++) {
        mutex_lock(&g_rtmtest_mutex);

        if (__atomic_add_fetch(&g_rtmtest_inside, 1U, __ATOMIC_ACQ_REL) != 1U)
            __atomic_add_fetch(&g_rtmtest_errors, 1U, __ATOMIC_RELAXED);

        uint32_t v = g_rtmtest_counter;
        /* 拉长临界区，让并发任务真的撞上 */
        for (volatile uint32_t j = 0; j < 64; j++)
            ;
        g_rtmtest_counter = v + 1;

        __atomic_sub_fetch(&g_rtmtest_inside, 1U, __ATOMIC_ACQ_REL);
        mutex_unlock(&g_rtmtest_mutex);

        /*
           每 32 轮在持锁时让出一次：制造真实的 BLOCKED 等待，
           覆盖“入队后、切出前被唤醒”的丢唤醒窗口。
        */
        if ((i & 31U) == 0) {
            mutex_lock(&g_rtmtest_mutex); /* 这里必然走可重入 */
            task_yield();
            mutex_unlock(&g_rtmtest_mutex);
        }

        if ((i & 15U) == 0)
            task_yield();
    }

    __atomic_add_fetch(&g_rtmtest_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static bool run_mutex_stress(void)
{
    mutex_init(&g_rtmtest_mutex);
    g_rtmtest_counter = 0;
    g_rtmtest_inside = 0;
    g_rtmtest_errors = 0;
    g_rtmtest_done = 0;

    uint32_t ncpu = g_num_cpus ? g_num_cpus : 1U;
    for (uint32_t i = 0; i < MTX_STRESS_TASKS; i++) {
        g_rtmtest_ids[i] = i;
        task_t *t = task_create_affinity("mtx-str", mtx_stress_task, &g_rtmtest_ids[i], 10, i % ncpu);
        if (!t)
            return false;
    }

    uint64_t deadline = timer_get_ns() + 10000000000ULL; /* 10s（TCG 兜底） */
    while (__atomic_load_n(&g_rtmtest_done, __ATOMIC_ACQUIRE) < MTX_STRESS_TASKS && timer_get_ns() < deadline)
        task_yield();

    return __atomic_load_n(&g_rtmtest_done, __ATOMIC_ACQUIRE) == MTX_STRESS_TASKS &&
           g_rtmtest_counter == MTX_STRESS_TASKS * MTX_STRESS_ITERS && g_rtmtest_inside == 0 && g_rtmtest_errors == 0;
}

/* ── 2. rt_mutex 优先级继承 ───────────────────────────────── */

static rt_mutex_t g_rtm;
static task_t *g_rtm_owner_task;

static volatile uint32_t g_rtm_order_counter;
static volatile uint32_t g_rtm_owner_locked;
static volatile uint32_t g_rtm_owner_done;
static volatile uint32_t g_rtm_high_entered;
static volatile uint32_t g_rtm_high_done;
static volatile uint32_t g_rtm_low_done;
static volatile uint32_t g_rtm_med_done;
static volatile int g_rtm_owner_lock_rc;
static volatile bool g_rtm_owner_unlock_ok;
static volatile uint32_t g_rtm_owner_boost_seen;
static volatile uint32_t g_rtm_owner_boost_after;
static volatile int g_rtm_high_lock_rc;
static volatile int g_rtm_low_lock_rc;
static volatile uint32_t g_rtm_high_order;
static volatile uint32_t g_rtm_low_order;
static volatile uint32_t g_rtm_med_order;

static uint32_t rtm_next_order(void)
{
    return __atomic_add_fetch(&g_rtm_order_counter, 1U, __ATOMIC_RELEASE);
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

static bool wait_task_blocked(task_t *t, uint64_t timeout_ns)
{
    uint64_t deadline = timer_get_ns() + timeout_ns;

    while (t->state != TASK_BLOCKED) {
        if (timer_get_ns() >= deadline)
            return false;
        task_yield();
    }
    return true;
}

static void rtm_owner_fn(void *arg)
{
    (void)arg;

    g_rtm_owner_lock_rc = rt_mutex_lock(&g_rtm);
    __atomic_store_n(&g_rtm_owner_locked, 1U, __ATOMIC_RELEASE);

    /* 等测试安排好高/低优先级等待者和一个中等优先级任务 */
    task_block(NULL);

    g_rtm_owner_boost_seen = task_current()->pi_priority;

    /*
       如果优先级继承失效，owner 还是 20，中等优先级任务（50）会在这里
       抢走 CPU，高优先级等待者要等它跑完才能拿到锁。
    */
    for (volatile uint32_t j = 0; j < 300000; j++)
        ;

    g_rtm_owner_unlock_ok = rt_mutex_unlock(&g_rtm);
    g_rtm_owner_boost_after = task_current()->pi_priority;

    __atomic_store_n(&g_rtm_owner_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void rtm_high_fn(void *arg)
{
    (void)arg;

    __atomic_store_n(&g_rtm_high_entered, 1U, __ATOMIC_RELEASE);
    g_rtm_high_lock_rc = rt_mutex_lock(&g_rtm);
    g_rtm_high_order = rtm_next_order();
    rt_mutex_unlock(&g_rtm);

    __atomic_store_n(&g_rtm_high_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void rtm_low_fn(void *arg)
{
    (void)arg;

    g_rtm_low_lock_rc = rt_mutex_lock(&g_rtm);
    g_rtm_low_order = rtm_next_order();
    rt_mutex_unlock(&g_rtm);

    __atomic_store_n(&g_rtm_low_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void rtm_med_fn(void *arg)
{
    (void)arg;

    for (volatile uint32_t j = 0; j < 2000000; j++)
        ;
    g_rtm_med_order = rtm_next_order();

    __atomic_store_n(&g_rtm_med_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static bool run_rt_mutex_pi_test(bool normal_owner)
{
    rt_mutex_init(&g_rtm);
    g_rtm_order_counter = 0;
    g_rtm_owner_locked = 0;
    g_rtm_owner_done = 0;
    g_rtm_high_entered = 0;
    g_rtm_high_done = 0;
    g_rtm_low_done = 0;
    g_rtm_med_done = 0;
    g_rtm_owner_lock_rc = -1;
    g_rtm_high_lock_rc = -1;
    g_rtm_low_lock_rc = -1;
    g_rtm_owner_unlock_ok = false;
    g_rtm_owner_boost_seen = 0;
    g_rtm_owner_boost_after = 0;
    g_rtm_high_order = 0;
    g_rtm_low_order = 0;
    g_rtm_med_order = 0;

    /*
       分别覆盖“RT owner 被提升”和“NORMAL owner 被临时提升为 RT”两条路径；
       后者会走到 sched_set_pi_boost() 的就绪队列重分类。
    */
    if (normal_owner)
        g_rtm_owner_task = task_create_affinity("rtm-own", rtm_owner_fn, NULL, 10, 0);
    else
        g_rtm_owner_task = task_create_kernel_rt("rtm-own", rtm_owner_fn, NULL, 20, 0);
    if (!g_rtm_owner_task || !wait_flag(&g_rtm_owner_locked, 2000000000ULL))
        return false;
    if (g_rtm_owner_lock_rc != 0)
        return false;

    task_t *high = task_create_kernel_rt("rtm-hi", rtm_high_fn, NULL, 80, 0);
    if (!high || !wait_flag(&g_rtm_high_entered, 2000000000ULL) || !wait_task_blocked(high, 2000000000ULL))
        return false;

    /* owner 应该已经被提升到高优先级等待者的 80 */
    if (__atomic_load_n(&g_rtm_owner_task->pi_priority, __ATOMIC_ACQUIRE) != 80U)
        return false;

    task_t *low = task_create_kernel_rt("rtm-lo", rtm_low_fn, NULL, 60, 0);
    if (!low || !wait_task_blocked(low, 2000000000ULL))
        return false;

    /* 两个等待者时，仍取最高优先级 80 */
    if (__atomic_load_n(&g_rtm_owner_task->pi_priority, __ATOMIC_ACQUIRE) != 80U)
        return false;

    task_t *med = task_create_kernel_rt("rtm-med", rtm_med_fn, NULL, 50, 0);
    if (!med)
        return false;

    /* 放 owner 走；它现在是 80，应该先于 med(50) 释放锁 */
    task_unblock(g_rtm_owner_task);

    uint64_t deadline = timer_get_ns() + 10000000000ULL;
    while ((__atomic_load_n(&g_rtm_owner_done, __ATOMIC_ACQUIRE) == 0 ||
            __atomic_load_n(&g_rtm_high_done, __ATOMIC_ACQUIRE) == 0 ||
            __atomic_load_n(&g_rtm_low_done, __ATOMIC_ACQUIRE) == 0 ||
            __atomic_load_n(&g_rtm_med_done, __ATOMIC_ACQUIRE) == 0) &&
           timer_get_ns() < deadline)
        task_yield();

    return __atomic_load_n(&g_rtm_owner_done, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&g_rtm_high_done, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&g_rtm_low_done, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&g_rtm_med_done, __ATOMIC_ACQUIRE) != 0 && g_rtm_owner_unlock_ok &&
           g_rtm_high_lock_rc == 0 && g_rtm_low_lock_rc == 0 && g_rtm_owner_boost_seen == 80U &&
           g_rtm_owner_boost_after == 0U &&
           /* 等待者按优先级拿锁：high(80) → low(60) → 中等任务最后 */
           g_rtm_high_order == 1U && g_rtm_low_order == 2U && g_rtm_med_order == 3U;
}

/* ── 3. rt_mutex PI 链（三层传递）────────────────────────── */

/*
   L(20) 持 M1 后在事件上睡眠；M(50) 持 M2 后阻塞在 M1；H(80) 再阻塞在 M2。
   没有 PI 链时，H 只把 M 直接提到 80（M 此时已阻塞在 M1 上），L 仍停留在
   50；有链时 L 应被传递提升到 80。

   再用一个 W(60) 做“墙”：同时唤醒 W 和 L，若 L 是 80，它会先于 W 释放
   M1，H 的拿锁延迟是微秒级；若 L 只有 50，W(60) 会先跑 20ms，H 的延迟
   被拖到 20ms 以上。既检查优先级不变量，也检查实际行为。
*/

static rt_mutex_t g_chain_m1;
static rt_mutex_t g_chain_m2;
static rt_event_t g_chain_release_evt;
static rt_event_t g_chain_wall_evt;
static task_t *g_chain_low;
static task_t *g_chain_mid;
static task_t *g_chain_high;
static task_t *g_chain_wall;

static volatile uint32_t g_chain_low_ready;
static volatile uint32_t g_chain_mid_ready;
static volatile uint32_t g_chain_done;
static volatile uint64_t g_chain_t0_ns;
static volatile uint64_t g_chain_high_done_ns;
static volatile int g_chain_low_rc;
static volatile int g_chain_mid_rc;
static volatile int g_chain_high_rc;
static volatile bool g_chain_unlock_ok;
static volatile uint32_t g_chain_prio_low;
static volatile uint32_t g_chain_prio_mid;
static volatile uint64_t g_chain_high_lat_ns;

static void chain_low_fn(void *arg)
{
    (void)arg;

    g_chain_low_rc = rt_mutex_lock(&g_chain_m1);
    __atomic_store_n(&g_chain_low_ready, 1U, __ATOMIC_RELEASE);

    (void)rt_event_wait(&g_chain_release_evt, 0x1U, false, 0, true);

    if (!rt_mutex_unlock(&g_chain_m1))
        g_chain_unlock_ok = false;
    __atomic_fetch_add(&g_chain_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void chain_mid_fn(void *arg)
{
    (void)arg;

    if (!wait_flag(&g_chain_low_ready, 2000000000ULL)) {
        g_chain_unlock_ok = false;
        __atomic_fetch_add(&g_chain_done, 1U, __ATOMIC_RELEASE);
        task_exit();
    }

    g_chain_mid_rc = rt_mutex_lock(&g_chain_m2);
    __atomic_store_n(&g_chain_mid_ready, 1U, __ATOMIC_RELEASE);
    if (g_chain_mid_rc != 0) {
        g_chain_unlock_ok = false;
        __atomic_fetch_add(&g_chain_done, 1U, __ATOMIC_RELEASE);
        task_exit();
    }

    /* 阻塞在 M1 上：链 L <- M1 <- M */
    if (rt_mutex_lock(&g_chain_m1) != 0)
        g_chain_unlock_ok = false;
    if (!rt_mutex_unlock(&g_chain_m2))
        g_chain_unlock_ok = false;
    if (!rt_mutex_unlock(&g_chain_m1))
        g_chain_unlock_ok = false;

    __atomic_fetch_add(&g_chain_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void chain_high_fn(void *arg)
{
    (void)arg;

    /* 此时 M 已经阻塞在 M1 上；H 阻塞在 M2 才会形成三层链。 */
    g_chain_high_rc = rt_mutex_lock(&g_chain_m2);
    if (g_chain_high_rc == 0 && !rt_mutex_unlock(&g_chain_m2))
        g_chain_unlock_ok = false;

    __atomic_store_n(&g_chain_high_done_ns, timer_get_ns(), __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_chain_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static void chain_wall_fn(void *arg)
{
    (void)arg;

    (void)rt_event_wait(&g_chain_wall_evt, 0x1U, false, 0, true);

    /* 20ms 纯忙循环：PI 链失效时它会把 H 的拿锁延迟拖到 20ms 以上。 */
    uint64_t end = timer_get_ns() + 20000000ULL;
    while (timer_get_ns() < end)
        ;

    __atomic_fetch_add(&g_chain_done, 1U, __ATOMIC_RELEASE);
    task_exit();
}

static bool run_rt_mutex_pi_chain_test(void)
{
    bool ok = true;
    bool setup_done = false;

    rt_mutex_init(&g_chain_m1);
    rt_mutex_init(&g_chain_m2);
    rt_event_init(&g_chain_release_evt);
    rt_event_init(&g_chain_wall_evt);

    g_chain_low_ready = 0;
    g_chain_mid_ready = 0;
    g_chain_done = 0;
    g_chain_t0_ns = 0;
    g_chain_high_done_ns = 0;
    g_chain_low_rc = -1;
    g_chain_mid_rc = -1;
    g_chain_high_rc = -1;
    g_chain_unlock_ok = true;
    g_chain_prio_low = 0;
    g_chain_prio_mid = 0;
    g_chain_high_lat_ns = 0;

    g_chain_low = task_create_kernel_rt("rtm-c-lo", chain_low_fn, NULL, 20, 0);
    if (!g_chain_low || !wait_flag(&g_chain_low_ready, 2000000000ULL) || g_chain_low_rc != 0) {
        ok = false;
        goto out;
    }

    g_chain_mid = task_create_kernel_rt("rtm-c-mid", chain_mid_fn, NULL, 50, 0);
    if (!g_chain_mid || !wait_flag(&g_chain_mid_ready, 2000000000ULL) || g_chain_mid_rc != 0 ||
        !wait_task_blocked(g_chain_mid, 2000000000ULL)) {
        ok = false;
        goto out;
    }

    /* 关键顺序：M 先阻塞在 M1 上，H 之后才来抢 M2。 */
    g_chain_high = task_create_kernel_rt("rtm-c-hi", chain_high_fn, NULL, 80, 0);
    if (!g_chain_high || !wait_task_blocked(g_chain_high, 2000000000ULL)) {
        ok = false;
        goto out;
    }

    g_chain_prio_low = task_effective_rt_prio(g_chain_low);
    g_chain_prio_mid = task_effective_rt_prio(g_chain_mid);

    /* H(80) -> M2(owner=M) -> M 阻塞在 M1(owner=L)：L 必须被传递提升到 80。 */
    if (g_chain_prio_low != 80U || g_chain_prio_mid != 80U)
        ok = false;

    g_chain_wall = task_create_kernel_rt("rtm-c-wall", chain_wall_fn, NULL, 60, 0);
    if (!g_chain_wall || !wait_task_blocked(g_chain_wall, 2000000000ULL)) {
        ok = false;
        goto out;
    }

    setup_done = true;

    /*
       在同一个关中断窗口里唤醒 wall(60) 和 low(20/80)，保证两者都在队列里
       之后才发生抢占：PI 链生效时 low 是 80，先于 wall；失效时 low 只有 50，
       wall(60) 会先跑满 20ms。
    */
    uint64_t flags = arch_irq_save();
    (void)rt_event_set(&g_chain_wall_evt, 0x1U);
    g_chain_t0_ns = timer_get_ns();
    (void)rt_event_set(&g_chain_release_evt, 0x1U);
    arch_irq_restore(flags);

out:
    /* 失败路径也要放走阻塞的参与者，避免污染后续测试。 */
    (void)rt_event_set(&g_chain_wall_evt, 0x1U);
    (void)rt_event_set(&g_chain_release_evt, 0x1U);

    uint64_t deadline = timer_get_ns() + 5000000000ULL;
    while (__atomic_load_n(&g_chain_done, __ATOMIC_ACQUIRE) < 4U && timer_get_ns() < deadline)
        task_yield();

    bool done = __atomic_load_n(&g_chain_done, __ATOMIC_ACQUIRE) == 4U;
    uint64_t t0 = __atomic_load_n(&g_chain_t0_ns, __ATOMIC_ACQUIRE);
    uint64_t hdone = __atomic_load_n(&g_chain_high_done_ns, __ATOMIC_ACQUIRE);
    uint64_t latency = (hdone > t0) ? (hdone - t0) : 0;
    g_chain_high_lat_ns = latency;

    bool chain_ok = setup_done && ok && done && g_chain_unlock_ok && g_chain_low_rc == 0 && g_chain_mid_rc == 0 &&
                    g_chain_high_rc == 0 && g_chain_prio_low == 80U && g_chain_prio_mid == 80U && latency < 10000000ULL;

    KLOG_INFO("=== RT PI CHAIN: %s prio_low=%u prio_mid=%u high_lat=%llu ns ===\n", chain_ok ? "PASS" : "FAIL",
              (unsigned)g_chain_prio_low, (unsigned)g_chain_prio_mid, (unsigned long long)latency);
    return chain_ok;
}

/* ── 入口 ─────────────────────────────────────────────────── */

void run_rt_mutex_tests(void)
{
    bool ok = true;
    const char *phase = "basic";

    /* 普通 mutex 基本语义（boot/idle 上下文也可以当 holder） */
    mutex_init(&g_rtmtest_mutex);
    if (mutex_is_locked(&g_rtmtest_mutex) || !mutex_trylock(&g_rtmtest_mutex) || !mutex_is_locked(&g_rtmtest_mutex) ||
        !mutex_trylock(&g_rtmtest_mutex) || mutex_holder(&g_rtmtest_mutex) != task_current()) {
        ok = false;
        goto out;
    }
    mutex_unlock(&g_rtmtest_mutex);
    mutex_unlock(&g_rtmtest_mutex);
    if (mutex_is_locked(&g_rtmtest_mutex)) {
        ok = false;
        goto out;
    }

    phase = "mutex-stress";
    if (!run_mutex_stress()) {
        ok = false;
        goto out;
    }

    phase = "rt-mutex-basic";
    rt_mutex_init(&g_rtm);
    if (!rt_mutex_trylock(&g_rtm) || rt_mutex_trylock(&g_rtm) || rt_mutex_owner(&g_rtm) != task_current() ||
        !rt_mutex_unlock(&g_rtm) || rt_mutex_unlock(&g_rtm)) {
        ok = false;
        goto out;
    }
    if (rt_mutex_lock(&g_rtm) != 0 || rt_mutex_lock(&g_rtm) != -RTM_EDEADLK || !rt_mutex_unlock(&g_rtm)) {
        ok = false;
        goto out;
    }

    phase = "rt-mutex-pi-rt-owner";
    if (!run_rt_mutex_pi_test(false)) {
        ok = false;
        goto out;
    }

    phase = "rt-mutex-pi-normal-owner";
    if (!run_rt_mutex_pi_test(true)) {
        ok = false;
        goto out;
    }

    phase = "rt-mutex-pi-chain";
    if (!run_rt_mutex_pi_chain_test()) {
        ok = false;
        goto out;
    }

out:
    if (ok) {
        KLOG_INFO("=== RT MUTEX TEST: PASS mtx=%u err=%u boost=%u after=%u order=%u/%u/%u "
                  "chain=%u/%u/%llu ===\n",
                  (unsigned)g_rtmtest_counter, (unsigned)g_rtmtest_errors, (unsigned)g_rtm_owner_boost_seen,
                  (unsigned)g_rtm_owner_boost_after, (unsigned)g_rtm_high_order, (unsigned)g_rtm_low_order,
                  (unsigned)g_rtm_med_order, (unsigned)g_chain_prio_low, (unsigned)g_chain_prio_mid,
                  (unsigned long long)g_chain_high_lat_ns);
    } else {
        KLOG_ERROR("=== RT MUTEX TEST: FAIL (%s) mtx=%u err=%u inside=%u boost=%u after=%u "
                   "rc=%d/%d/%d order=%u/%u/%u ===\n",
                   phase, (unsigned)g_rtmtest_counter, (unsigned)g_rtmtest_errors, (unsigned)g_rtmtest_inside,
                   (unsigned)g_rtm_owner_boost_seen, (unsigned)g_rtm_owner_boost_after, (int)g_rtm_owner_lock_rc,
                   (int)g_rtm_high_lock_rc, (int)g_rtm_low_lock_rc, (unsigned)g_rtm_high_order,
                   (unsigned)g_rtm_low_order, (unsigned)g_rtm_med_order);
    }
}
