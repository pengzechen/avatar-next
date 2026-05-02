/*
 * tests/mutex_stress_test.c — Mutex 压力测试
 *
 * 测试内容：
 *  1. 多任务竞争同一把锁（10个任务）
 *  2. 共享链表的并发插入和删除
 *  3. 共享计数器的原子性验证
 *  4. 高频率锁操作
 *  5. 数据完整性验证
 */

#include "task/task.h"
#include "task/mutex.h"
#include "klog.h"

/* ── 测试配置 ─────────────────────────────────────────────── */

#define STRESS_NUM_TASKS    10      /* 并发任务数 */
#define STRESS_ITERATIONS   100     /* 每个任务的迭代次数 */
#define STRESS_LIST_SIZE    50      /* 链表最大节点数 */

/* ── 共享数据结构 ─────────────────────────────────────────── */

/* 全局互斥锁 */
static mutex_t g_stress_mutex;

/* 共享计数器 */
static volatile uint32_t g_shared_counter = 0;

/* 共享链表节点 */
typedef struct stress_node {
    int value;
    struct stress_node *next;
} stress_node_t;

/* 共享链表 */
static stress_node_t *g_shared_list = NULL;
static uint32_t g_list_length = 0;

/* 统计信息 */
static uint32_t g_total_lock_operations = 0;
static uint32_t g_total_contentions = 0;  /* 锁竞争次数 */

/* ── 辅助函数 ─────────────────────────────────────────────── */

/* 链表长度验证 */
static uint32_t
list_verify_length(void)
{
    uint32_t count = 0;
    stress_node_t *node = g_shared_list;

    while (node != NULL && count < STRESS_LIST_SIZE * 2) {
        count++;
        node = node->next;
    }

    return count;
}

/* ── 压力测试任务 1：计数器递增 ──────────────────────────── */

static void
counter_task(void *arg)
{
    uint32_t task_id = *(uint32_t *)arg;
    uint32_t local_count = 0;

    for (uint32_t i = 0; i < STRESS_ITERATIONS; i++) {
        /* 尝试获取锁 */
        mutex_lock(&g_stress_mutex);

        /* 临界区：递增共享计数器 */
        uint32_t old_value = g_shared_counter;
        g_shared_counter++;
        local_count++;

        /* 模拟一些工作 */
        for (volatile int j = 0; j < 10000; j++);

        /* 验证：递增后应该等于递增前 + 1 */
        if (g_shared_counter != old_value + 1) {
            KLOG_ERROR("[stress] task_%u: counter corruption! old=%u new=%u\n",
                      task_id, old_value, g_shared_counter);
        }

        g_total_lock_operations++;

        mutex_unlock(&g_stress_mutex);

        /* 短暂让出 CPU */
        if (i % 10 == 0) {
            task_yield();
        }
    }

    KLOG_INFO("[stress] task_%u: completed %u increments\n", task_id, local_count);
    task_exit();
}

/* ── 压力测试任务 2：链表操作 ────────────────────────────── */

static stress_node_t g_node_pool[STRESS_LIST_SIZE];
static bool g_node_used[STRESS_LIST_SIZE];

static stress_node_t *
alloc_node(void)
{
    for (uint32_t i = 0; i < STRESS_LIST_SIZE; i++) {
        if (!g_node_used[i]) {
            g_node_used[i] = true;
            return &g_node_pool[i];
        }
    }
    return NULL;
}

static void
free_node(stress_node_t *node)
{
    if (node >= g_node_pool && node < g_node_pool + STRESS_LIST_SIZE) {
        uint32_t index = node - g_node_pool;
        g_node_used[index] = false;
    }
}

static void
list_task(void *arg)
{
    uint32_t task_id = *(uint32_t *)arg;
    uint32_t insert_count = 0;
    uint32_t delete_count = 0;

    for (uint32_t i = 0; i < STRESS_ITERATIONS; i++) {
        mutex_lock(&g_stress_mutex);

        /* 交替进行插入和删除 */
        if (i % 2 == 0 || g_list_length == 0) {
            /* 插入 */
            stress_node_t *node = alloc_node();
            if (node != NULL) {
                node->value = task_id * 1000 + i;
                node->next = g_shared_list;
                g_shared_list = node;
                g_list_length++;
                insert_count++;
            }
        } else {
            /* 删除 */
            if (g_shared_list != NULL) {
                stress_node_t *node = g_shared_list;
                g_shared_list = node->next;
                free_node(node);
                g_list_length--;
                delete_count++;
            }
        }

        /* 验证链表长度 */
        uint32_t actual_length = list_verify_length();
        if (actual_length != g_list_length) {
            KLOG_ERROR("[stress] task_%u: list length mismatch! expected=%u actual=%u\n",
                      task_id, g_list_length, actual_length);
        }

        g_total_lock_operations++;

        mutex_unlock(&g_stress_mutex);

        if (i % 5 == 0) {
            task_yield();
        }
    }

    KLOG_INFO("[stress] task_%u: %u inserts, %u deletes\n",
              task_id, insert_count, delete_count);
    task_exit();
}

/* ── 压力测试任务 3：混合操作 ────────────────────────────── */

static void
mixed_task(void *arg)
{
    uint32_t task_id = *(uint32_t *)arg;
    uint32_t operations = 0;

    for (uint32_t i = 0; i < STRESS_ITERATIONS; i++) {
        /* 使用 trylock 避免阻塞 */
        if (mutex_trylock(&g_stress_mutex)) {
            /* 混合操作 */
            g_shared_counter++;
            operations++;

            /* 偶尔操作链表 */
            if (i % 5 == 0) {
                if (g_shared_list != NULL && (i % 10 == 0)) {
                    /* 删除 */
                    stress_node_t *node = g_shared_list;
                    g_shared_list = node->next;
                    free_node(node);
                    g_list_length--;
                } else if (g_list_length < STRESS_LIST_SIZE) {
                    /* 插入 */
                    stress_node_t *node = alloc_node();
                    if (node != NULL) {
                        node->value = task_id * 2000 + i;
                        node->next = g_shared_list;
                        g_shared_list = node;
                        g_list_length++;
                    }
                }
            }

            g_total_lock_operations++;
            mutex_unlock(&g_stress_mutex);
        } else {
            /* 锁被占用，记录竞争 */
            g_total_contentions++;
        }

        /* 短暂延迟 */
        for (volatile int j = 0; j < 1000; j++);

        if (i % 8 == 0) {
            task_yield();
        }
    }

    KLOG_INFO("[stress] task_%u: %u mixed operations\n", task_id, operations);
    task_exit();
}

/* ── 验证任务 ───────────────────────────────────────────── */

static void
verify_task(void *arg)
{
    (void)arg;

    /* 等待一段时间让其他任务运行 */
    for (int i = 0; i < 10; i++) {
        task_yield();
    }

    /* 最终验证 */
    mutex_lock(&g_stress_mutex);

    KLOG_INFO("[stress] === Final Verification ===\n");
    KLOG_INFO("[stress] Shared counter: %u\n", g_shared_counter);
    KLOG_INFO("[stress] List length: %u\n", g_list_length);
    KLOG_INFO("[stress] Total lock operations: %u\n", g_total_lock_operations);
    KLOG_INFO("[stress] Total contentions: %u\n", g_total_contentions);

    /* 验证链表一致性 */
    uint32_t actual_length = list_verify_length();
    if (actual_length != g_list_length) {
        KLOG_ERROR("[stress] LIST CORRUPTION! expected=%u actual=%u\n",
                  g_list_length, actual_length);
    } else {
        KLOG_INFO("[stress] List verification: PASSED ✓\n");
    }

    /* 验证计数器合理性 */
    uint32_t expected_min = STRESS_NUM_TASKS * STRESS_ITERATIONS / 2;
    if (g_shared_counter < expected_min) {
        KLOG_WARN("[stress] Counter lower than expected: %u < %u\n",
                 g_shared_counter, expected_min);
    } else {
        KLOG_INFO("[stress] Counter verification: PASSED ✓\n");
    }

    mutex_unlock(&g_stress_mutex);

    KLOG_INFO("[stress] Verification task exiting\n");
    task_exit();
}

/* ── 主测试入口 ─────────────────────────────────────────── */

static uint32_t g_task_ids[STRESS_NUM_TASKS];

void
run_mutex_stress_test(void)
{
    KLOG_INFO("=== Mutex Stress Test ===\n");
    KLOG_INFO("[stress] Configuration:\n");
    KLOG_INFO("[stress]   Tasks: %u\n", STRESS_NUM_TASKS);
    KLOG_INFO("[stress]   Iterations per task: %u\n", STRESS_ITERATIONS);
    KLOG_INFO("[stress]   Max list size: %u\n", STRESS_LIST_SIZE);

    /* 初始化互斥锁 */
    mutex_init(&g_stress_mutex);

    /* 初始化链表节点池 */
    for (uint32_t i = 0; i < STRESS_LIST_SIZE; i++) {
        g_node_used[i] = false;
    }

    /* 重置统计信息 */
    g_shared_counter = 0;
    g_shared_list = NULL;
    g_list_length = 0;
    g_total_lock_operations = 0;
    g_total_contentions = 0;

    /* 创建测试任务 */
    KLOG_INFO("[stress] Creating stress test tasks...\n");

    /* 创建计数器任务（前半部分） */
    for (uint32_t i = 0; i < STRESS_NUM_TASKS / 3; i++) {
        g_task_ids[i] = i;
        char name[16];
        name[0] = 'c';
        name[1] = 't';
        name[2] = '0' + i;
        name[3] = '\0';
        task_create(name, counter_task, &g_task_ids[i], 1);
    }

    /* 创建链表操作任务（中间部分） */
    for (uint32_t i = STRESS_NUM_TASKS / 3; i < 2 * STRESS_NUM_TASKS / 3; i++) {
        g_task_ids[i] = i;
        char name[16];
        name[0] = 'l';
        name[1] = 't';
        name[2] = '0' + i;
        name[3] = '\0';
        task_create(name, list_task, &g_task_ids[i], 1);
    }

    /* 创建混合操作任务（后半部分） */
    for (uint32_t i = 2 * STRESS_NUM_TASKS / 3; i < STRESS_NUM_TASKS; i++) {
        g_task_ids[i] = i;
        char name[16];
        name[0] = 'm';
        name[1] = 't';
        name[2] = '0' + i;
        name[3] = '\0';
        task_create(name, mixed_task, &g_task_ids[i], 1);
    }

    /* 创建验证任务 */
    task_create("verify", verify_task, NULL, 1);

    KLOG_INFO("[stress] All tasks created. Test running...\n");
    KLOG_INFO("[stress] \n");
}
