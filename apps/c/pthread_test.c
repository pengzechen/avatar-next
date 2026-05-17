/*
 * pthread_test.c — Avatar OS pthread 综合测试
 *
 * 测试项：
 *   Test 1  基本线程创建与 join
 *   Test 2  互斥锁并发计数
 *   Test 3  条件变量生产者/消费者
 *   Test 4  多线程同时竞争同一互斥锁（压力测试）
 *   Test 5  线程 TLS（__thread 变量）
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

/* ────────────────────────────────────────────────────────────────────
 * 公共工具
 * ──────────────────────────────────────────────────────────────────── */

#define NUM_THREADS   4
#define LOOP_COUNT    20000
#define RACE_LOOPS    2000   /* Test 0: fewer iters, sched_yield per step */

static void pass(const char *name) { printf("  [PASS] %s\n", name); }
static void fail(const char *name, const char *reason) {
    printf("  [FAIL] %s: %s\n", name, reason);
}

/* ────────────────────────────────────────────────────────────────────
 * Test 1: 基本 pthread_create / pthread_join
 * ──────────────────────────────────────────────────────────────────── */

static void *t1_worker(void *arg)
{
    int id = *(int *)arg;
    /* 每个线程返回自己的 id * 2 */
    return (void *)(long)(id * 2);
}

static void test1_basic_join(void)
{
    pthread_t tids[NUM_THREADS];
    int       ids[NUM_THREADS];
    int ok = 1;

    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i + 1;
        if (pthread_create(&tids[i], NULL, t1_worker, &ids[i]) != 0) {
            fail("basic_join", "pthread_create failed");
            return;
        }
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        void *ret;
        pthread_join(tids[i], &ret);
        if ((long)ret != (ids[i] * 2)) {
            ok = 0;
        }
    }
    if (ok) pass("basic_join");
    else    fail("basic_join", "wrong return value");
}

/* ────────────────────────────────────────────────────────────────────
 * Test 0: 无锁竞争演示（对照组）
 *   故意不加锁，用显式 read→busy_work→write 拉大竞争窗口，
 *   证明真实的 data race 确实在发生。
 * ──────────────────────────────────────────────────────────────────── */

static volatile long t0_counter = 0;

static void *t0_worker_racy(void *arg)
{
    (void)arg;
    for (int i = 0; i < RACE_LOOPS; i++) {
        /* 显式三步 read-modify-write，中间 sched_yield 强制切换 CPU */
        long tmp = t0_counter;          /* step 1: read */
        sched_yield();                  /* step 2: 让出 CPU，保证另一线程运行 */
        t0_counter = tmp + 1;           /* step 3: write 过时值，覆盖别人的更新 */
    }
    return NULL;
}

static void test0_race_demo(void)
{
    pthread_t tids[NUM_THREADS];
    t0_counter = 0;

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&tids[i], NULL, t0_worker_racy, NULL);
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
        printf("  WARNING: no loss observed (scheduler may be fully serializing)\n");
}

/* ────────────────────────────────────────────────────────────────────
 * Test 2: 互斥锁并发计数
 * ──────────────────────────────────────────────────────────────────── */

static pthread_mutex_t t2_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile long   t2_counter = 0;

static void *t2_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < LOOP_COUNT; i++) {
        pthread_mutex_lock(&t2_mutex);
        t2_counter++;
        pthread_mutex_unlock(&t2_mutex);
    }
    return NULL;
}

static void test2_mutex_counter(void)
{
    pthread_t tids[NUM_THREADS];
    t2_counter = 0;

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&tids[i], NULL, t2_worker, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(tids[i], NULL);

    long expected = (long)NUM_THREADS * LOOP_COUNT;
    if (t2_counter == expected)
        pass("mutex_counter");
    else {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected %ld got %ld", expected, t2_counter);
        fail("mutex_counter", buf);
    }
}

/* ────────────────────────────────────────────────────────────────────
 * Test 3: 条件变量生产者/消费者
 * ──────────────────────────────────────────────────────────────────── */

#define T3_ITEMS 8

static pthread_mutex_t t3_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  t3_cond  = PTHREAD_COND_INITIALIZER;
static int             t3_queue[T3_ITEMS];
static int             t3_head = 0, t3_tail = 0, t3_count = 0;
static int             t3_done = 0;   /* 生产者完成标志 */

static void *t3_producer(void *arg)
{
    int total = *(int *)arg;
    for (int i = 0; i < total; i++) {
        pthread_mutex_lock(&t3_mutex);
        while (t3_count == T3_ITEMS)        /* 队列满则等待 */
            pthread_cond_wait(&t3_cond, &t3_mutex);
        t3_queue[t3_tail % T3_ITEMS] = i + 1;
        t3_tail++;
        t3_count++;
        pthread_cond_signal(&t3_cond);
        pthread_mutex_unlock(&t3_mutex);
    }
    pthread_mutex_lock(&t3_mutex);
    t3_done = 1;
    pthread_cond_broadcast(&t3_cond);
    pthread_mutex_unlock(&t3_mutex);
    return NULL;
}

static void *t3_consumer(void *arg)
{
    long *sum = (long *)arg;
    *sum = 0;
    for (;;) {
        pthread_mutex_lock(&t3_mutex);
        while (t3_count == 0 && !t3_done)
            pthread_cond_wait(&t3_cond, &t3_mutex);
        if (t3_count == 0 && t3_done) {
            pthread_mutex_unlock(&t3_mutex);
            break;
        }
        *sum += t3_queue[t3_head % T3_ITEMS];
        t3_head++;
        t3_count--;
        pthread_cond_signal(&t3_cond);
        pthread_mutex_unlock(&t3_mutex);
    }
    return NULL;
}

static void test3_cond_producer_consumer(void)
{
    pthread_t prod, cons;
    int total = 200;
    long sum = 0;
    t3_head = t3_tail = t3_count = t3_done = 0;

    pthread_create(&cons, NULL, t3_consumer, &sum);
    pthread_create(&prod, NULL, t3_producer, &total);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    long expected = (long)total * (total + 1) / 2;   /* 1+2+...+200 = 20100 */
    if (sum == expected)
        pass("cond_producer_consumer");
    else {
        char buf[64];
        snprintf(buf, sizeof(buf), "sum=%ld expected=%ld", sum, expected);
        fail("cond_producer_consumer", buf);
    }
}

/* ────────────────────────────────────────────────────────────────────
 * Test 4: 多线程压力竞争互斥锁
 * ──────────────────────────────────────────────────────────────────── */

#define T4_THREADS  4
#define T4_LOOPS    10000

static pthread_mutex_t t4_mutex   = PTHREAD_MUTEX_INITIALIZER;
static volatile long   t4_counter = 0;

static void *t4_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < T4_LOOPS; i++) {
        pthread_mutex_lock(&t4_mutex);
        long v = t4_counter;
        /* 模拟临界区里的工作：读-修改-写 */
        v += 1;
        t4_counter = v;
        pthread_mutex_unlock(&t4_mutex);
    }
    return NULL;
}

static void test4_mutex_stress(void)
{
    pthread_t tids[T4_THREADS];
    t4_counter = 0;

    for (int i = 0; i < T4_THREADS; i++)
        pthread_create(&tids[i], NULL, t4_worker, NULL);
    for (int i = 0; i < T4_THREADS; i++)
        pthread_join(tids[i], NULL);

    long expected = (long)T4_THREADS * T4_LOOPS;
    if (t4_counter == expected)
        pass("mutex_stress");
    else {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected %ld got %ld", expected, t4_counter);
        fail("mutex_stress", buf);
    }
}

/* ────────────────────────────────────────────────────────────────────
 * Test 5: 线程局部存储 (__thread)
 * ──────────────────────────────────────────────────────────────────── */

static __thread int tls_val = 0;

static void *t5_worker(void *arg)
{
    int id = *(int *)arg;
    tls_val = id * 100;
    /* 让调度器有机会切换 */
    for (volatile int i = 0; i < 100000; i++) {}
    return (void *)(long)tls_val;   /* 如果 TLS 正确，应返回 id*100 */
}

static void test5_tls(void)
{
    pthread_t tids[NUM_THREADS];
    int ids[NUM_THREADS];
    int ok = 1;

    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i + 1;
        pthread_create(&tids[i], NULL, t5_worker, &ids[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        void *ret;
        pthread_join(tids[i], &ret);
        if ((long)ret != ids[i] * 100)
            ok = 0;
    }
    if (ok) pass("tls_thread_local");
    else    fail("tls_thread_local", "TLS value corrupted across threads");
}

/* ────────────────────────────────────────────────────────────────────
 * main
 * ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== Avatar OS pthread test ===\n");

    printf("\n[Test 0] RACE DEMO: NO mutex (%d threads x %d loops, sched_yield in race window)\n",
           NUM_THREADS, RACE_LOOPS);
    printf("  (intentional data race to verify contention is real)\n");
    test0_race_demo();

    printf("\n[Test 1] Basic create/join\n");
    test1_basic_join();

    printf("\n[Test 2] Mutex counter (%d threads x %d loops)\n",
           NUM_THREADS, LOOP_COUNT);
    printf("  (same workload as Test 0, but with mutex)\n");
    test2_mutex_counter();

    printf("\n[Test 3] Condition variable producer/consumer\n");
    test3_cond_producer_consumer();

    printf("\n[Test 4] Mutex stress (%d threads x %d loops)\n",
           T4_THREADS, T4_LOOPS);
    test4_mutex_stress();

    printf("\n[Test 5] Thread-local storage (__thread)\n");
    test5_tls();

    printf("\n=== Done ===\n");
    return 0;
}
