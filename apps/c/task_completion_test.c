/*
 * task_completion_test.c — completion 模块的用户态 A/B 验收
 *
 * 需要 TASK_TEST=1 的内核（暴露三个测试 syscall，见
 * kernel/syscall/syscall.c 的 TASK_INFRA_TEST 段）。
 *
 *   task-completion broadcast <n> <attempts>
 *
 * 每轮 fork n 个子进程，每个阻塞在同一个 completion 上；父进程等它们
 * 全部 armed 后，记录 t0，调用 COMPLETE 唤醒全部，子进程各自记录被唤醒
 * 的时刻。输出：
 *   - ready_cpu0/1：等待者分布在哪些核
 *   - total_us / max_us：COMPLETE 到最后一个等待者醒来的墙钟/最大延迟
 *
 * 判据：
 *   - 新内核 completion 唤醒走 sched_unblock() + resched IPI，跨核等待者
 *     应在几十~几百 us 内全醒；
 *   - 基线内核（同样的测试 syscall，但内部是裸 state=READY+sched_enqueue）
 *     跨核等待者要等目标核下一次 tick（100Hz = 10ms）。
 *
 * 用户态程序里手抄的伪 syscall 号必须与 kernel/syscall/syscall_internal.h
 * 的 AVATAR_SYS_COMPLETION_* 一致。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AVATAR_SYS_COMPLETION_WAIT     0x7FFF1000ULL
#define AVATAR_SYS_COMPLETION_COMPLETE 0x7FFF1001ULL
#define AVATAR_SYS_COMPLETION_RESET    0x7FFF1002ULL

#define MAX_WAITERS 64

static int cpu_now(void)
{
    unsigned cpu = 0;

    /* getcpu：x86_64 = 309，aarch64/riscv = 168（通用号）*/
#if defined(__x86_64__)
#define SYS_getcpu_local 309
#else
#define SYS_getcpu_local 168
#endif
    if (syscall(SYS_getcpu_local, &cpu, NULL, NULL) != 0)
        return -1;
    return (int)cpu;
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int parse_int(const char *s, int fallback)
{
    char *end = NULL;
    long v;

    if (!s || !*s)
        return fallback;
    v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 1000000L)
        return fallback;
    return (int)v;
}

static void yield_until(uint64_t ns)
{
    uint64_t until = now_ns() + ns;

    while (now_ns() < until)
        sched_yield();
}

struct comp_shm {
    volatile int armed;
    volatile int done;
    volatile int cpus[MAX_WAITERS];
    volatile uint64_t ts[MAX_WAITERS];
};

static int broadcast_once(int n, int attempt)
{
    struct comp_shm *shm;
    int hist[16];
    int ret;
    uint64_t t0, t1, max_ns = 0, sum_ns = 0;
    long cid = 0;

    memset(hist, 0, sizeof(hist));

    shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
               -1, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    shm->armed = 0;
    shm->done = 0;

    /* 上一轮的完成者已经全部退出；复位失败说明 completion 有残留等待者 */
    ret = (int)syscall(AVATAR_SYS_COMPLETION_RESET, cid);
    if (ret != 0) {
        printf("TASKCOMP attempt=%d reset failed rc=%d\n", attempt, ret);
        munmap(shm, 4096);
        return 1;
    }

    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p < 0) {
            perror("fork");
            break;
        }
        if (p == 0) {
            int c = cpu_now();
            shm->cpus[i] = c;
            __atomic_fetch_add(&shm->armed, 1, __ATOMIC_RELEASE);
            syscall(AVATAR_SYS_COMPLETION_WAIT, cid);
            shm->ts[i] = now_ns();
            __atomic_fetch_add(&shm->done, 1, __ATOMIC_RELEASE);
            _exit(0);
        }
    }

    /* 等全部进入等待态；yield 让与父进程同核的等待者也能跑到 wait */
    while (__atomic_load_n(&shm->armed, __ATOMIC_ACQUIRE) < n)
        sched_yield();
    yield_until(5000000ULL); /* 5ms：确保都已经睡在 completion 上 */

    t0 = now_ns();
    syscall(AVATAR_SYS_COMPLETION_COMPLETE, cid);
    while (__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) < n)
        sched_yield();
    t1 = now_ns();

    for (int i = 0; i < n; i++) {
        int c = shm->cpus[i];
        uint64_t ts = shm->ts[i];
        uint64_t d = (ts > t0) ? ts - t0 : 0;

        if (c >= 0 && c < (int)(sizeof(hist) / sizeof(hist[0])))
            hist[c]++;
        /*
         * 唤醒时所在 CPU 不一定等于就绪时；这里不再取一次 getcpu（子进程
         * 已经退出），只用时间戳判延迟。
         */
        if (d > max_ns)
            max_ns = d;
        sum_ns += d;
    }
    while (waitpid(-1, NULL, 0) > 0)
        ;

    printf("TASKCOMP broadcast attempt=%d n=%d ready_cpu0=%d ready_cpu1=%d "
           "total_us=%.1f avg_us=%.1f max_us=%.1f\n",
           attempt, n, hist[0], hist[1], (double)(t1 - t0) / 1000.0,
           (double)sum_ns / 1000.0 / (double)n, (double)max_ns / 1000.0);
    munmap(shm, 4096);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "broadcast";
    int n;
    int attempts;
    int rc = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (strcmp(mode, "broadcast") != 0) {
        printf("usage: task-completion broadcast <n> <attempts>\n");
        return 2;
    }

    n = parse_int(argc > 2 ? argv[2] : NULL, 8);
    attempts = parse_int(argc > 3 ? argv[3] : NULL, 5);
    if (n < 1 || n > MAX_WAITERS)
        n = 8;
    if (attempts < 1 || attempts > 100)
        attempts = 5;

    for (int i = 0; i < attempts; i++)
        rc |= broadcast_once(n, i);
    return rc;
}
