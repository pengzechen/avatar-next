/*
 * tests/mutex_comparison_test.c — Mutex 对比测试
 *
 * 目的：对比无锁和有锁情况下的行为差异
 *
 * 测试场景：
 *  1. 无锁版本：多个任务同时修改共享数据（会有竞争）
 *  2. 有锁版本：使用 mutex 保护共享数据（安全）
 */

#include "task/task.h"
#include "task/mutex.h"
#include "klog.h"

/* ── 测试配置 ─────────────────────────────────────────────── */

#define COMPARE_NUM_TASKS    10      /* 增加任务数 */
#define COMPARE_ITERATIONS   200     /* 增加迭代次数 */

/* ── 共享数据（模拟银行账户）──────────────────────────────── */

typedef struct {
    uint32_t balance;           /* 账户余额 */
    uint32_t deposits;          /* 存款次数 */
    uint32_t withdrawals;       /* 取款次数 */
    uint32_t errors;            /* 错误次数 */
} account_t;

/* 无锁版本使用的账户 */
static account_t g_account_no_lock;

/* 有锁版本使用的账户 */
static account_t g_account_locked;
static mutex_t   g_account_mutex;

/* ── 无锁版本：直接操作共享数据 ───────────────────────────── */

static void
no_lock_deposit_task(void *arg)
{
    uint32_t task_id = *(uint32_t *)arg;

    for (uint32_t i = 0; i < COMPARE_ITERATIONS; i++) {
        /* ❌ 无锁：直接修改共享数据 */
        uint32_t old_balance = g_account_no_lock.balance;

        /* 🔴 关键：在read和write之间强制yield，制造竞态窗口 */
        task_yield();

        g_account_no_lock.balance = old_balance + 100;
        g_account_no_lock.deposits++;

        /* 验证：余额应该是 50 的倍数（deposit +100, withdraw -50）*/
        if (g_account_no_lock.balance % 50 != 0) {
            g_account_no_lock.errors++;
        }
    }

    KLOG_INFO("[no-lock] deposit_task_%u: completed %u deposits\n",
              task_id, COMPARE_ITERATIONS);
    task_exit();
}

static void
no_lock_withdraw_task(void *arg)
{
    uint32_t task_id = *(uint32_t *)arg;

    for (uint32_t i = 0; i < COMPARE_ITERATIONS; i++) {
        /* ❌ 无锁：直接修改共享数据 */
        uint32_t old_balance = g_account_no_lock.balance;

        /* 🔴 关键：在read和write之间强制yield，制造竞态窗口 */
        task_yield();

        if (old_balance >= 50) {
            g_account_no_lock.balance = old_balance - 50;
            g_account_no_lock.withdrawals++;
        }

        /* 验证：余额应该是 50 的倍数 */
        if (g_account_no_lock.balance % 50 != 0) {
            g_account_no_lock.errors++;
        }
    }

    KLOG_INFO("[no-lock] withdraw_task_%u: completed %u withdrawals\n",
              task_id, COMPARE_ITERATIONS);
    task_exit();
}

/* ── 有锁版本：使用 mutex 保护 ───────────────────────────── */

static void
locked_deposit_task(void *arg)
{
    uint32_t task_id = *(uint32_t *)arg;

    for (uint32_t i = 0; i < COMPARE_ITERATIONS; i++) {
        /* ✅ 有锁：使用 mutex 保护 */
        mutex_lock(&g_account_mutex);

        uint32_t old_balance = g_account_locked.balance;

        g_account_locked.balance = old_balance + 100;
        g_account_locked.deposits++;

        /* 验证：余额应该是 50 的倍数（因为deposit +100，withdraw -50）*/
        if (g_account_locked.balance % 50 != 0) {
            g_account_locked.errors++;
        }

        mutex_unlock(&g_account_mutex);

        /* 在mutex外yield，让其他任务竞争锁 */
        task_yield();
    }

    KLOG_INFO("[locked] deposit_task_%u: completed %u deposits\n",
              task_id, COMPARE_ITERATIONS);
    task_exit();
}

static void
locked_withdraw_task(void *arg)
{
    uint32_t task_id = *(uint32_t *)arg;

    for (uint32_t i = 0; i < COMPARE_ITERATIONS; i++) {
        /* ✅ 有锁：使用 mutex 保护 */
        mutex_lock(&g_account_mutex);

        uint32_t old_balance = g_account_locked.balance;

        if (old_balance >= 50) {
            g_account_locked.balance = old_balance - 50;
            g_account_locked.withdrawals++;
        }

        /* 验证：余额应该是 50 的倍数（因为只减50）*/
        if (g_account_locked.balance % 50 != 0) {
            g_account_locked.errors++;
        }

        mutex_unlock(&g_account_mutex);

        /* 在mutex外yield，让其他任务竞争锁 */
        task_yield();
    }

    KLOG_INFO("[locked] withdraw_task_%u: completed %u withdrawals\n",
              task_id, COMPARE_ITERATIONS);
    task_exit();
}

/* ── 结果验证和打印 ───────────────────────────────────────── */

static void
print_results(const char *test_name, account_t *account)
{
    KLOG_INFO("\n");
    KLOG_INFO("=== %s Results ===\n", test_name);
    KLOG_INFO("Final balance: %u\n", account->balance);
    KLOG_INFO("Total deposits: %u (expected %u)\n", account->deposits,
              COMPARE_NUM_TASKS / 2 * COMPARE_ITERATIONS);
    KLOG_INFO("Total withdrawals: %u (expected %u)\n", account->withdrawals,
              COMPARE_NUM_TASKS / 2 * COMPARE_ITERATIONS);
    KLOG_INFO("Errors detected: %u\n", account->errors);

    /* 计算预期余额 */
    int32_t expected = 1000 +  /* 初始余额 */
                       (int32_t)account->deposits * 100 -
                       (int32_t)account->withdrawals * 50;
    int32_t difference = (int32_t)account->balance - expected;

    KLOG_INFO("Expected balance: %d\n", expected);
    KLOG_INFO("Difference: %d\n", difference);

    /* 检查三种错误类型 */
    bool has_error = false;

    /* 1. 验证错误（balance不是50的倍数）*/
    if (account->errors > 0) {
        KLOG_ERROR("❌ VALIDATION ERRORS: %u operations failed balance check!\n",
                  account->errors);
        has_error = true;
    }

    /* 2. 丢失操作（deposits/withdrawals计数不正确）*/
    uint32_t expected_deposits = COMPARE_NUM_TASKS / 2 * COMPARE_ITERATIONS;
    uint32_t expected_withdrawals = COMPARE_NUM_TASKS / 2 * COMPARE_ITERATIONS;
    if (account->deposits != expected_deposits || account->withdrawals != expected_withdrawals) {
        KLOG_ERROR("❌ LOST OPERATIONS: Expected %u deposits/%u withdrawals, got %u/%u\n",
                  expected_deposits, expected_withdrawals,
                  account->deposits, account->withdrawals);
        has_error = true;
    }

    /* 3. Balance不正确（lost updates导致）*/
    if (difference != 0) {
        KLOG_ERROR("❌ LOST UPDATES: Balance off by %d (expected %d, got %u)\n",
                  difference, expected, account->balance);
        has_error = true;
    }

    if (has_error) {
        KLOG_ERROR("❌ DATA CORRUPTION DETECTED!\n");
    } else {
        KLOG_INFO("✓ All operations completed correctly\n");
    }
    KLOG_INFO("\n");
}

/* ── 无锁测试 ─────────────────────────────────────────────── */

static uint32_t g_no_lock_ids[COMPARE_NUM_TASKS];

static void
run_no_lock_test(void)
{
    KLOG_INFO("╔══════════════════════════════════════════════════════╗\n");
    KLOG_INFO("║  Phase 1: NO LOCK (Race Condition Demo)            ║\n");
    KLOG_INFO("╚══════════════════════════════════════════════════════╝\n");

    /* 初始化无锁账户 */
    g_account_no_lock.balance = 1000;
    g_account_no_lock.deposits = 0;
    g_account_no_lock.withdrawals = 0;
    g_account_no_lock.errors = 0;

    KLOG_INFO("Initial balance: %u\n", g_account_no_lock.balance);
    KLOG_INFO("Creating %u tasks WITHOUT mutex protection...\n",
              COMPARE_NUM_TASKS);

    /* 创建无锁任务：一半存款，一半取款 */
    for (uint32_t i = 0; i < COMPARE_NUM_TASKS / 2; i++) {
        g_no_lock_ids[i] = i;
        char name[16];
        name[0] = 'd'; name[1] = '0' + i; name[2] = '\0';
        task_create(name, no_lock_deposit_task, &g_no_lock_ids[i], 1);
    }

    for (uint32_t i = COMPARE_NUM_TASKS / 2; i < COMPARE_NUM_TASKS; i++) {
        g_no_lock_ids[i] = i;
        char name[16];
        name[0] = 'w'; name[1] = '0' + i; name[2] = '\0';
        task_create(name, no_lock_withdraw_task, &g_no_lock_ids[i], 1);
    }
}

/* ── 有锁测试 ─────────────────────────────────────────────── */

static uint32_t g_locked_ids[COMPARE_NUM_TASKS];

static void
run_locked_test(void)
{
    KLOG_INFO("╔══════════════════════════════════════════════════════╗\n");
    KLOG_INFO("║  Phase 2: WITH LOCK (Mutex Protected)               ║\n");
    KLOG_INFO("╚══════════════════════════════════════════════════════╝\n");

    /* 初始化有锁账户（全新数据） */
    g_account_locked.balance = 1000;
    g_account_locked.deposits = 0;
    g_account_locked.withdrawals = 0;
    g_account_locked.errors = 0;

    /* 初始化 mutex */
    mutex_init(&g_account_mutex);

    KLOG_INFO("Initial balance: %u\n", g_account_locked.balance);
    KLOG_INFO("Creating %u tasks WITH mutex protection...\n",
              COMPARE_NUM_TASKS);

    /* 创建有锁任务：一半存款，一半取款 */
    for (uint32_t i = 0; i < COMPARE_NUM_TASKS / 2; i++) {
        g_locked_ids[i] = i;
        char name[16];
        name[0] = 'D'; name[1] = '0' + i; name[2] = '\0';
        task_create(name, locked_deposit_task, &g_locked_ids[i], 1);
    }

    for (uint32_t i = COMPARE_NUM_TASKS / 2; i < COMPARE_NUM_TASKS; i++) {
        g_locked_ids[i] = i;
        char name[16];
        name[0] = 'W'; name[1] = '0' + i; name[2] = '\0';
        task_create(name, locked_withdraw_task, &g_locked_ids[i], 1);
    }

    /* 等待所有有锁任务完成再返回 */
    for (int i = 0; i < 100; i++) {
        task_yield();
    }
}

/* ── 对比总结任务 ─────────────────────────────────────────── */

static void
summary_task(void *arg)
{
    (void)arg;

    /* 所有任务已完成，直接打印总结 */
    KLOG_INFO("\n");
    KLOG_INFO("╔══════════════════════════════════════════════════════╗\n");
    KLOG_INFO("║           COMPARISON SUMMARY                         ║\n");
    KLOG_INFO("╚══════════════════════════════════════════════════════╝\n");

    KLOG_INFO("\n");

    if (g_account_no_lock.errors > 0) {
        KLOG_INFO("❌ Race conditions detected in Phase 1 (NO LOCK)!\n");
        KLOG_INFO("   Without proper synchronization, concurrent tasks\n");
        KLOG_INFO("   can corrupt shared data structures.\n");
        KLOG_INFO("\n");
        KLOG_INFO("   Phase 2 (WITH LOCK) prevented these errors.\n");
    } else {
        KLOG_INFO("✓ No errors detected in either phase\n");
    }

    KLOG_INFO("\n");
    KLOG_INFO("Key Takeaways:\n");
    KLOG_INFO("  1. Without locks: multiple tasks can read-modify-write\n");
    KLOG_INFO("     the same data simultaneously, causing lost updates\n");
    KLOG_INFO("  2. With mutex: only one task can access the data at a time,\n");
    KLOG_INFO("     ensuring atomicity and correctness\n");
    KLOG_INFO("  3. The cost: mutex adds overhead due to blocking/context switch\n");
    KLOG_INFO("\n");

    KLOG_INFO("[summary] Test complete. Exiting...\n");
    task_exit();
}

/* ── 主测试入口 ───────────────────────────────────────────── */

void
run_mutex_comparison_test(void)
{
    KLOG_INFO("=== Mutex Comparison Test: No Lock vs With Lock ===\n");
    KLOG_INFO("\n");

    /* 阶段1：无锁测试 */
    run_no_lock_test();

    /* 等待无锁测试完成 */
    for (int i = 0; i < 30; i++) {
        task_yield();
    }

    /* 打印无锁测试结果 */
    print_results("NO LOCK (Phase 1)", &g_account_no_lock);

    /* 阶段2：有锁测试（会等待任务完成）*/
    run_locked_test();

    /* 打印有锁测试结果（任务已完成）*/
    print_results("WITH LOCK (Phase 2)", &g_account_locked);

    /* 创建总结任务 */
    task_create("summary", summary_task, NULL, 1);
}
