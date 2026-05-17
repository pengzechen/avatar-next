/*
 * smp_thread_test.c — Phase 4a 验证：多线程跨核分发
 *
 * 思路：
 *   1. 创建 N 个无亲和的纯计算内核线程，每个线程循环 yield 并把
 *      __atomic_fetch_add 累加到本核命中计数器上。
 *   2. BSP 等待固定毫秒后读取每核命中分布，并断言所有核 >0。
 *   3. 通过后线程主动 task_exit，回收 slot，不影响后续 busybox/vmm。
 *
 * 注意：
 *   - 仅作为 SMP 健康检查，不引入持久线程，确保 SMP=1 也直接通过。
 *   - 仅 kernel/main 调用一次（在所有 AP 进入 idle 之后）。
 */

#include "task/task.h"
#include "task/sched.h"
#include "task/cpu.h"
#include "timer/timer.h"
#include "klog.h"

static volatile uint64_t s_hits[AVATAR_MAX_CPUS];
static volatile uint32_t s_done;
static volatile uint32_t s_stop;

static void
smp_worker_fn(void *arg)
{
    uint32_t iters = (uint32_t)(uintptr_t)arg;
    for (uint32_t i = 0; i < iters && !s_stop; i++) {
        uint32_t cid = cpu_current()->cpu_id;
        if (cid < AVATAR_MAX_CPUS) {
            __atomic_fetch_add(&s_hits[cid], 1ULL, __ATOMIC_RELAXED);
        }
        /* 主动让出 CPU，给其他线程/核机会。 */
        task_yield();
    }
    __atomic_fetch_add(&s_done, 1U, __ATOMIC_RELAXED);
    task_exit();
}

/**
 * smp_thread_test_run — Phase 4a 多核线程分发自检
 * @nthreads:   要创建的内核线程数（建议 2*nr_cpus）
 * @iters:      每个线程循环次数
 * @wait_ms:    BSP 等待的毫秒数（足够线程跑完）
 */
void
smp_thread_test_run(uint32_t nthreads, uint32_t iters, uint32_t wait_ms)
{
    for (uint32_t i = 0; i < AVATAR_MAX_CPUS; i++) {
        s_hits[i] = 0;
    }
    s_done = 0;
    s_stop = 0;

    KLOG_INFO("[smp-thr] start: nthreads=%u iters=%u wait=%ums (ncpu=%u)\n",
              (unsigned)nthreads, (unsigned)iters, (unsigned)wait_ms,
              (unsigned)g_num_cpus);

    for (uint32_t i = 0; i < nthreads; i++) {
        char name[8];
        name[0] = 's'; name[1] = 'm'; name[2] = 'p';
        name[3] = 'w'; name[4] = '0' + (char)(i / 10);
        name[5] = '0' + (char)(i % 10); name[6] = '\0';
        task_t *t = task_create(name, smp_worker_fn,
                                (void *)(uintptr_t)iters, 10);
        if (!t) {
            KLOG_ERROR("[smp-thr] task_create failed at i=%u\n", (unsigned)i);
            break;
        }
    }

    /* 等线程跑：BSP 不参与本队列，由 timer ISR 推动调度。 */
    timer_delay_ms(wait_ms);
    s_stop = 1;

    /* 再给一小段时间让线程退出（避免 task_exit 期间释放冲突日志）。 */
    timer_delay_ms(100);

    uint32_t n = g_num_cpus ? g_num_cpus : 1U;
    bool all_hit = true;
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = s_hits[i];
        total += v;
        KLOG_INFO("[smp-thr] cpu%u hits=%llu\n",
                  (unsigned)i, (unsigned long long)v);
        if (v == 0ULL) {
            all_hit = false;
        }
    }
    KLOG_INFO("[smp-thr] total=%llu done=%u\n",
              (unsigned long long)total, (unsigned)s_done);

    if (n == 1U) {
        if (total > 0ULL) {
            KLOG_INFO("[smp-thr] PASS (single-cpu)\n");
        } else {
            KLOG_ERROR("[smp-thr] FAIL: no hits\n");
        }
        return;
    }

    if (all_hit) {
        KLOG_INFO("[smp-thr] PASS: all %u cpu(s) executed worker\n",
                  (unsigned)n);
    } else {
        KLOG_ERROR("[smp-thr] FAIL: some cpu(s) idle, check round-robin\n");
    }
}
