/*
 * apps/c/mutex/test.c — 基于 futex 的用户态 mutex 实现与测试
 *
 * 展示如何用内核 futex 系统调用（FUTEX_WAIT / FUTEX_WAKE）从零实现 mutex，
 * 这正是 pthread_mutex 的底层机制。
 *
 * 实现了两种 mutex：
 *   umutex_t    — 不可重入的高效 futex mutex（三态：0=空闲 1=锁定无等待者 2=锁定有等待者）
 *   rmutex_t    — 基于 umutex_t 的可重入（递归）mutex
 *
 * 测试项：
 *   Test 0  无锁竞争演示（对照组，证明竞争确实发生）
 *   Test 1  umutex 基础单线程 lock / unlock / trylock
 *   Test 2  umutex 多线程计数器（等价于 pthread Test 2）
 *   Test 3  rmutex 单线程递归加锁（lock 3 次 → unlock 3 次）
 *   Test 4  rmutex 多线程 + 递归深度 2
 *   Test 5  umutex trylock 竞争场景
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>      /* 仅用于线程创建（pthread_create / join） */
#include <stdatomic.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <limits.h>
#include <string.h>
#include <sched.h>

/* ─── futex 系统调用号（如果 musl 没有定义则自行补充） ──────────── */
#ifndef SYS_futex
#  if defined(__aarch64__) || defined(__riscv)
#    define SYS_futex 98
#  elif defined(__x86_64__)
#    define SYS_futex 202
#  else
#    error "SYS_futex: unsupported architecture"
#  endif
#endif

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_WAIT_PRIVATE  (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE  (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

static inline long
futex(atomic_int *addr, int op, int val)
{
    return syscall(SYS_futex, addr, op, val, NULL, NULL, 0);
}

/* ═══════════════════════════════════════════════════════════════════
 * umutex_t — 不可重入 futex mutex
 *
 * state = 0: 未锁定
 * state = 1: 已锁定，无等待者
 * state = 2: 已锁定，有等待者
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    atomic_int state;
} umutex_t;

#define UMUTEX_INIT { .state = ATOMIC_VAR_INIT(0) }

static void
umutex_init(umutex_t *m)
{
    atomic_init(&m->state, 0);
}

static void
umutex_lock(umutex_t *m)
{
    int c;

    /* 快速路径：0 → 1（无竞争）*/
    c = 0;
    if (atomic_compare_exchange_strong_explicit(
            &m->state, &c, 1,
            memory_order_acquire, memory_order_relaxed))
        return;

    /* 慢速路径：有竞争，设置 state=2 后进入 futex 睡眠 */
    do {
        /* 若当前 state=1 或 state=2，先确保 state=2（通知 unlock 要唤醒） */
        if (c == 2 ||
            atomic_compare_exchange_strong_explicit(
                &m->state, &c, 2,
                memory_order_acquire, memory_order_relaxed)) {
            /* 睡眠直到 state != 2 */
            futex(&m->state, FUTEX_WAIT_PRIVATE, 2);
        }
        /* 尝试以 state=2 方式获取（保留"有等待者"信息）*/
        c = 0;
    } while (!atomic_compare_exchange_strong_explicit(
                 &m->state, &c, 2,
                 memory_order_acquire, memory_order_relaxed));
}

static int
umutex_trylock(umutex_t *m)
{
    int c = 0;
    return atomic_compare_exchange_strong_explicit(
               &m->state, &c, 1,
               memory_order_acquire, memory_order_relaxed);
}

static void
umutex_unlock(umutex_t *m)
{
    /* fetch_sub: 1→0 无等待者直接返回；2→1 有等待者需要唤醒 */
    if (atomic_fetch_sub_explicit(&m->state, 1, memory_order_release) != 1) {
        atomic_store_explicit(&m->state, 0, memory_order_release);
        futex(&m->state, FUTEX_WAKE_PRIVATE, 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * rmutex_t — 可重入（递归）futex mutex
 *
 * 基于 umutex_t，增加 owner 和 count 字段。
 * 同一线程多次 lock 只递增 count；unlock 递减，count=0 时真正释放。
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    umutex_t       base;
    atomic_size_t  owner;  /* pthread_t 转 size_t，0 表示无持有者 */
    int            count;  /* 重入深度（仅由持有线程访问） */
} rmutex_t;

#define RMUTEX_INIT { .base = UMUTEX_INIT, \
                      .owner = ATOMIC_VAR_INIT(0), \
                      .count = 0 }

static void
rmutex_init(rmutex_t *m)
{
    umutex_init(&m->base);
    atomic_init(&m->owner, 0);
    m->count = 0;
}

static void
rmutex_lock(rmutex_t *m)
{
    size_t self = (size_t)pthread_self();

    /* 重入检测：已是持有者，直接递增计数 */
    if (atomic_load_explicit(&m->owner, memory_order_relaxed) == self) {
        m->count++;
        return;
    }

    /* 非持有者：获取底层锁 */
    umutex_lock(&m->base);
    /* 设置 owner（此时我们已独占锁，安全写入）*/
    atomic_store_explicit(&m->owner, self, memory_order_relaxed);
    m->count = 1;
}

static void
rmutex_unlock(rmutex_t *m)
{
    /* 递减计数；不为 0 则仍在递归持有中 */
    if (--m->count > 0)
        return;

    /* 清除 owner 后再释放底层锁 */
    atomic_store_explicit(&m->owner, 0, memory_order_relaxed);
    umutex_unlock(&m->base);
}

/* ─────────────────────────────────────────────────────────────────── */

#define NUM_THREADS   4
#define LOOP_COUNT    20000
#define RACE_LOOPS    2000

static void pass(const char *name) { printf("  [PASS] %s\n", name); }
static void fail(const char *name, const char *reason) {
    printf("  [FAIL] %s: %s\n", name, reason);
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 0: 无锁竞争演示
 * ═══════════════════════════════════════════════════════════════════ */

static volatile long t0_counter = 0;

static void *t0_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < RACE_LOOPS; i++) {
        long tmp = t0_counter;
        sched_yield();
        t0_counter = tmp + 1;
    }
    return NULL;
}

static void test0_race_demo(void)
{
    pthread_t tids[NUM_THREADS];
    t0_counter = 0;

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&tids[i], NULL, t0_worker, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(tids[i], NULL);

    long expected = (long)NUM_THREADS * RACE_LOOPS;
    long actual   = t0_counter;
    long lost     = expected - actual;
    int  pct      = (int)(lost * 100 / expected);

    printf("  expected = %ld\n", expected);
    printf("  actual   = %ld\n", actual);
    if (lost > 0)
        printf("  lost     = %ld (%d%%)  --> RACE CONFIRMED\n", lost, pct);
    else
        printf("  WARNING: no loss observed (scheduler may be serializing)\n");
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 1: umutex 基础单线程
 * ═══════════════════════════════════════════════════════════════════ */

static void test1_umutex_basic(void)
{
    umutex_t m = UMUTEX_INIT;

    /* lock → unlock */
    umutex_lock(&m);
    if (atomic_load(&m.state) == 0) {
        fail("umutex_basic", "state should be nonzero after lock");
        return;
    }
    umutex_unlock(&m);
    if (atomic_load(&m.state) != 0) {
        fail("umutex_basic", "state should be 0 after unlock");
        return;
    }

    /* trylock（空闲 → 成功）*/
    if (!umutex_trylock(&m)) {
        fail("umutex_basic", "trylock on free mutex should succeed");
        return;
    }
    /* trylock 已持有时（不可重入，应失败）*/
    if (umutex_trylock(&m)) {
        umutex_unlock(&m);
        umutex_unlock(&m);
        fail("umutex_basic", "trylock on held mutex should fail (non-reentrant)");
        return;
    }
    umutex_unlock(&m);

    pass("umutex_basic");
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 2: umutex 多线程计数器
 * ═══════════════════════════════════════════════════════════════════ */

static umutex_t t2_mutex = UMUTEX_INIT;
static volatile long t2_counter = 0;

static void *t2_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < LOOP_COUNT; i++) {
        umutex_lock(&t2_mutex);
        t2_counter++;
        umutex_unlock(&t2_mutex);
    }
    return NULL;
}

static void test2_umutex_counter(void)
{
    pthread_t tids[NUM_THREADS];
    t2_counter = 0;
    umutex_init(&t2_mutex);

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&tids[i], NULL, t2_worker, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(tids[i], NULL);

    long expected = (long)NUM_THREADS * LOOP_COUNT;
    if (t2_counter == expected)
        pass("umutex_counter");
    else {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected %ld got %ld", expected, t2_counter);
        fail("umutex_counter", buf);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 3: rmutex 单线程递归加锁
 * ═══════════════════════════════════════════════════════════════════ */

static void test3_rmutex_recursive(void)
{
    rmutex_t m = RMUTEX_INIT;

    /* 递归加锁三次 */
    rmutex_lock(&m);
    if (m.count != 1) { fail("rmutex_recursive", "count should be 1"); return; }

    rmutex_lock(&m);
    if (m.count != 2) { fail("rmutex_recursive", "count should be 2"); return; }

    rmutex_lock(&m);
    if (m.count != 3) { fail("rmutex_recursive", "count should be 3"); return; }

    /* 对应三次解锁：前两次不释放锁 */
    rmutex_unlock(&m);
    if (m.count != 2) { fail("rmutex_recursive", "count should be 2 after 1st unlock"); return; }
    if (atomic_load(&m.base.state) == 0) {
        fail("rmutex_recursive", "lock should still be held after partial unlock");
        return;
    }

    rmutex_unlock(&m);
    if (m.count != 1) { fail("rmutex_recursive", "count should be 1 after 2nd unlock"); return; }

    rmutex_unlock(&m);
    if (m.count != 0) { fail("rmutex_recursive", "count should be 0 after full unlock"); return; }
    if (atomic_load(&m.base.state) != 0) {
        fail("rmutex_recursive", "lock should be released after full unlock");
        return;
    }

    pass("rmutex_recursive");
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 4: rmutex 多线程 + 递归深度 2
 * ═══════════════════════════════════════════════════════════════════ */

static rmutex_t t4_mutex = RMUTEX_INIT;
static volatile long t4_counter = 0;

/* 每个线程加锁两次（递归深度 2），递增计数器，再解锁两次 */
static void *t4_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < LOOP_COUNT; i++) {
        rmutex_lock(&t4_mutex);   /* 第 1 次 */
        rmutex_lock(&t4_mutex);   /* 第 2 次（重入）*/
        t4_counter++;
        rmutex_unlock(&t4_mutex); /* 释放第 2 次 */
        rmutex_unlock(&t4_mutex); /* 释放第 1 次（真正释放）*/
    }
    return NULL;
}

static void test4_rmutex_threaded(void)
{
    pthread_t tids[NUM_THREADS];
    t4_counter = 0;
    rmutex_init(&t4_mutex);

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&tids[i], NULL, t4_worker, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(tids[i], NULL);

    long expected = (long)NUM_THREADS * LOOP_COUNT;
    if (t4_counter == expected)
        pass("rmutex_threaded");
    else {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected %ld got %ld", expected, t4_counter);
        fail("rmutex_threaded", buf);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 5: umutex trylock 竞争场景
 * ═══════════════════════════════════════════════════════════════════ */

static umutex_t t5_mutex    = UMUTEX_INIT;
static volatile long t5_acquired = 0;
static volatile long t5_failed   = 0;

static void *t5_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < LOOP_COUNT; i++) {
        if (umutex_trylock(&t5_mutex)) {
            t5_acquired++;
            umutex_unlock(&t5_mutex);
        } else {
            t5_failed++;
        }
    }
    return NULL;
}

static void test5_trylock_race(void)
{
    pthread_t tids[NUM_THREADS];
    t5_acquired = 0;
    t5_failed   = 0;
    umutex_init(&t5_mutex);

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&tids[i], NULL, t5_worker, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(tids[i], NULL);

    long total = (long)NUM_THREADS * LOOP_COUNT;
    printf("  trylock acquired=%ld failed=%ld total=%ld\n",
           t5_acquired, t5_failed, total);

    /* 所有尝试之和 = 成功 + 失败 */
    if (t5_acquired + t5_failed == total)
        pass("trylock_race");
    else
        fail("trylock_race", "acquired + failed != total");
}

/* ═══════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("=== Avatar OS User-Space Mutex Test (futex-based) ===\n\n");

    printf("[Test 0] Race demo (no lock)\n");
    test0_race_demo();
    printf("\n");

    printf("[Test 1] umutex basic (single-thread)\n");
    test1_umutex_basic();
    printf("\n");

    printf("[Test 2] umutex multi-thread counter\n");
    test2_umutex_counter();
    printf("\n");

    printf("[Test 3] rmutex recursive single-thread\n");
    test3_rmutex_recursive();
    printf("\n");

    printf("[Test 4] rmutex multi-thread recursive depth=2\n");
    test4_rmutex_threaded();
    printf("\n");

    printf("[Test 5] umutex trylock under contention\n");
    test5_trylock_race();
    printf("\n");

    printf("=== Done ===\n");
    return 0;
}
