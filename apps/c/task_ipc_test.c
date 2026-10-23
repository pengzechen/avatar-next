/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   task_ipc_test.c — task 基础设施的用户态验收(A/B 对比用)

   目的：证明「有新的跨核唤醒/抢占基础设施」和「没有」是可观测地不同的。
   只用标准 syscall(fork/mmap/getcpu/futex/clock_gettime), 因此同一份
   二进制可以同时跑在新内核和旧内核上做对照。

   用法：
     task-ipc dist <n>
         创建 n 个子进程, 每个报告自己落在哪颗 CPU。验证 SMP 下任务确实
         按 round-robin 摊开(两核时两边都应该 >0)。

     task-ipc ping <iters> <attempts>
         父/子用 MAP_SHARED 内存 + futex 乒乓。若两者在不同核上, 且
         内核没有「远端唤醒立即 kick」/ IPI, 被唤醒方要等自己那颗核的
         下一次 timer tick(本内核 100Hz = 10ms)。新内核用 resched IPI
         立即唤醒。每次 attempt 打印两端 CPU 和平均往返延迟(us)。

   为什么不用 pipe：pipe 的 blocked_reader/blocked_writer 是无锁检查,
   存在已知的 lost-wakeup(见 apps/c/racetest.c 的 pipe 子命令)。用它
   测 task 基础设施, 失败时无法区分是 IPI 坏了还是 pipe 丢了唤醒。
   futex 的 wait/wake 有全局锁, 且新内核里唤醒统一走 sched_unblock()。

   输出统一带 "TASKIPC " 前缀, 方便脚本 grep 和 A/B 对账。
*/
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_getcpu
#if defined(__x86_64__)
#define SYS_getcpu 309
#else
#define SYS_getcpu 168
#endif
#endif

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#endif

#define MAX_CHILDREN 64

static int cpu_now(void)
{
    unsigned cpu = 0;

    if (syscall(SYS_getcpu, &cpu, NULL, NULL) != 0)
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

static void busy_wait_ns(uint64_t ns)
{
    uint64_t until = now_ns() + ns;

    while (now_ns() < until)
        ;
}

static int futex_wait(volatile int *uaddr, int val)
{
    return (int)syscall(SYS_futex, (int *)uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static int futex_wake(volatile int *uaddr, int n)
{
    return (int)syscall(SYS_futex, (int *)uaddr, FUTEX_WAKE, n, NULL, NULL, 0);
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

/* ── dist：任务是否真的摊到不同核 ─────────────────────────────── */

struct dist_shm {
    volatile int count;
    volatile int cpus[MAX_CHILDREN];
};

static int mode_dist(int n)
{
    struct dist_shm *shm;
    int hist[16];
    int got;

    if (n < 1 || n > MAX_CHILDREN)
        n = 8;
    memset(hist, 0, sizeof(hist));

    shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    shm->count = 0;
    for (int i = 0; i < n; i++)
        shm->cpus[i] = -1;

    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p < 0) {
            perror("fork");
            break;
        }
        if (p == 0) {
            shm->cpus[i] = cpu_now();
            __atomic_fetch_add(&shm->count, 1, __ATOMIC_RELEASE);
            _exit(0);
        }
    }

    while (__atomic_load_n(&shm->count, __ATOMIC_ACQUIRE) < n)
        sched_yield();

    got = n;
    for (int i = 0; i < n; i++) {
        int c = shm->cpus[i];
        if (c >= 0 && c < (int)(sizeof(hist) / sizeof(hist[0])))
            hist[c]++;
    }
    while (waitpid(-1, NULL, 0) > 0)
        ;

    printf("TASKIPC dist requested=%d reported=%d", n, got);
    for (int i = 0; i < (int)(sizeof(hist) / sizeof(hist[0])); i++) {
        if (hist[i] || i < 2)
            printf(" cpu%d=%d", i, hist[i]);
    }
    printf("\n");
    munmap(shm, 4096);
    return 0;
}

/* ── ping：跨核唤醒延迟(futex 乒乓)──────────────────────────── */

struct ping_shm {
    volatile int ready;
    volatile int child_cpu;
    volatile int ping; /* 父 → 子 */
    volatile int pong; /* 子 → 父 */
};

static int ping_once(int iters, int attempt)
{
    struct ping_shm *shm;
    pid_t pid;
    int pcpu;
    int ccpu;
    uint64_t t0, t1, total = 0;
    double avg_us;

    shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    shm->ready = 0;
    shm->child_cpu = -1;
    shm->ping = 0;
    shm->pong = 0;

    pid = fork();
    if (pid < 0) {
        perror("fork");
        munmap(shm, 4096);
        return 1;
    }
    if (pid == 0) {
        ccpu = cpu_now();
        shm->child_cpu = ccpu;
        __atomic_store_n(&shm->ready, 1, __ATOMIC_RELEASE);
        futex_wake(&shm->ready, 1);

        for (int r = 1; r <= iters; r++) {
            while (__atomic_load_n(&shm->ping, __ATOMIC_ACQUIRE) < r)
                futex_wait(&shm->ping, shm->ping);
            __atomic_store_n(&shm->pong, r, __ATOMIC_RELEASE);
            futex_wake(&shm->pong, 1);
        }
        _exit(0);
    }

    while (!__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE)) {
        int v = shm->ready;
        futex_wait(&shm->ready, v);
    }
    ccpu = shm->child_cpu;
    pcpu = cpu_now();

    /*
       让子进程真正进入 futex_wait 再开始。否则第一批 ping 可能在它睡下
       之前就到达, 测到的是「根本没睡」。5ms 足够覆盖双核调度。
    */
    busy_wait_ns(5000000ULL);

    for (int r = 1; r <= iters; r++) {
        t0 = now_ns();
        __atomic_store_n(&shm->ping, r, __ATOMIC_RELEASE);
        futex_wake(&shm->ping, 1);
        while (__atomic_load_n(&shm->pong, __ATOMIC_ACQUIRE) < r)
            futex_wait(&shm->pong, shm->pong);
        t1 = now_ns();
        total += t1 - t0;
    }

    avg_us = (double)total / 1000.0 / (double)iters;
    printf("TASKIPC ping attempt=%d parent_cpu=%d child_cpu=%d cross=%d "
           "iters=%d avg_us=%.1f\n",
           attempt, pcpu, ccpu, (pcpu != ccpu), iters, avg_us);

    waitpid(pid, NULL, 0);
    munmap(shm, 4096);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "dist";

    setvbuf(stdout, NULL, _IONBF, 0);

    if (strcmp(mode, "dist") == 0)
        return mode_dist(parse_int(argc > 2 ? argv[2] : NULL, 8));

    if (strcmp(mode, "ping") == 0) {
        int iters = parse_int(argc > 2 ? argv[2] : NULL, 100);
        int attempts = parse_int(argc > 3 ? argv[3] : NULL, 3);
        int rc = 0;

        for (int i = 0; i < attempts; i++)
            rc |= ping_once(iters, i);
        return rc;
    }

    printf("usage: task-ipc dist <n> | ping <iters> <attempts>\n");
    return 2;
}
