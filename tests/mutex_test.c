/*
 * tests/mutex_test.c — Mutex 测试程序
 */

#include "task/task.h"
#include "task/mutex.h"
#include "klog.h"

/* 共享计数器 */
static uint32_t g_shared_counter = 0;

/* 全局互斥锁 */
static mutex_t g_mutex;

/* 测试任务 A：递增共享计数器 */
static void
mutex_task_a(void *arg)
{
    (void)arg;

    for (int i = 0; i < 5; i++) {
        mutex_lock(&g_mutex);

        g_shared_counter++;
        KLOG_INFO("[task_a] counter = %u\n", g_shared_counter);

        /* 模拟一些工作 */
        for (volatile int j = 0; j < 1000000; j++);

        mutex_unlock(&g_mutex);

        /* 让出 CPU */
        task_yield();
    }

    KLOG_INFO("[task_a] exiting\n");
    task_exit();
}

/* 测试任务 B：递增共享计数器 */
static void
mutex_task_b(void *arg)
{
    (void)arg;

    for (int i = 0; i < 5; i++) {
        mutex_lock(&g_mutex);

        g_shared_counter++;
        KLOG_INFO("[task_b] counter = %u\n", g_shared_counter);

        /* 模拟一些工作 */
        for (volatile int j = 0; j < 1000000; j++);

        mutex_unlock(&g_mutex);

        /* 让出 CPU */
        task_yield();
    }

    KLOG_INFO("[task_b] exiting\n");
    task_exit();
}

/* 测试任务 C：递增共享计数器 */
static void
mutex_task_c(void *arg)
{
    (void)arg;

    for (int i = 0; i < 5; i++) {
        mutex_lock(&g_mutex);

        g_shared_counter++;
        KLOG_INFO("[task_c] counter = %u\n", g_shared_counter);

        /* 模拟一些工作 */
        for (volatile int j = 0; j < 1000000; j++);

        mutex_unlock(&g_mutex);

        /* 让出 CPU */
        task_yield();
    }

    KLOG_INFO("[task_c] exiting\n");
    task_exit();
}

/* 测试 trylock */
static void
trylock_task(void *arg)
{
    (void)arg;

    KLOG_INFO("[trylock_task] testing mutex_trylock\n");

    /* 尝试获取未持有的锁 */
    if (mutex_trylock(&g_mutex)) {
        KLOG_INFO("[trylock_task] acquired mutex via trylock\n");
        mutex_unlock(&g_mutex);
    } else {
        KLOG_INFO("[trylock_task] failed to acquire mutex via trylock\n");
    }

    KLOG_INFO("[trylock_task] exiting\n");
    task_exit();
}

/* 主测试入口 */
void
run_mutex_tests(void)
{
    KLOG_INFO("=== Mutex Test ===\n");

    /* 初始化互斥锁 */
    mutex_init(&g_mutex);

    /* 检查初始状态 */
    if (mutex_is_locked(&g_mutex)) {
        KLOG_ERROR("[mutex_test] mutex should not be locked initially\n");
    } else {
        KLOG_INFO("[mutex_test] mutex initial state: unlocked ✓\n");
    }

    KLOG_INFO("[mutex_test] creating tasks...\n");

    /* 创建测试任务 */
    task_create("task_a", mutex_task_a, NULL, 1);
    task_create("task_b", mutex_task_b, NULL, 1);
    task_create("task_c", mutex_task_c, NULL, 1);

    /* 等待所有任务完成（通过 yield 让出 CPU） */
    /* 注意：这里我们假设 idle 任务会持续 yield */
    KLOG_INFO("[mutex_test] tasks created, idle yielding...\n");

    /* 创建 trylock 测试任务 */
    task_create("trylock", trylock_task, NULL, 1);
}
