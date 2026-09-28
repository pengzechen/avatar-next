/*
 * racetest.c — 并发竞争态复现工具（用户态，静态链接）
 *
 * 用途：把内核里几处「已定位但没复现」的时序竞争打成可观察的失败。
 * 三个子命令各打一条，都需要 **SMP>1**（单核下用户进程的内核侧
 * 不会被抢占，见 kernel/task/sched.c 的 trap_preempt_kernel_side）。
 *
 *   racetest slot <procs> <iters>
 *       多个进程各自循环 fork+wait。打的是 fork 路径里内联的
 *       任务槽「回收 → 认领」循环（kernel/syscall/core/proc_lifecycle.c），
 *       它无锁：两个核可能同时 reap 同一个 DEAD 任务
 *       （→ vm_destroy_user_process 双重释放页表 / PMM 双重 free），
 *       或同时认领同一个空槽（→ 两个任务共用一块 TCB 和一块栈）。
 *
 *       失败特征：内核打印 "PMM: invalid free address"、
 *                 "[sched] CORRUPT runqueue"，或直接 panic / 挂死。
 *
 *   racetest pipe <iters>
 *       反复建管道，reader 阻塞读、writer 立刻写。打的是
 *       kernel/syscall/fs/pipe.c 的丢唤醒：reader 在「读到 count==0」
 *       与「写 p->blocked_reader」之间被另一核的 writer 插进来
 *       （writer 写完数据、检查 blocked_reader 还是 NULL），
 *       reader 随后睡死，虽然环里躺着数据。
 *
 *       失败特征：进度停在某一轮不再前进（本程序会每轮打印心跳）。
 *
 *   racetest wake <iters>
 *       一个管道上放一个阻塞的 writer + 两个并发 reader。打的是
 *       kernel/task/task.c 的 task_unblock 无锁：两个 reader 同时
 *       看到 p->blocked_writer 非空、同时调 task_unblock，
 *       两边都在对方写 state 之前读到 TASK_BLOCKED → 两次
 *       sched_enqueue 同一个节点 → 运行队列双向链表损坏。
 *
 *       失败特征："[sched] CORRUPT runqueue"、野指针崩溃、挂死。
 *
 * 输出约定：每完成一轮打一个点，便于把「挂着不动」和「还在跑」区分开；
 *           全部跑完打 "DONE"。
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * 内核管道的容量，抄自 kernel/syscall/fs/pipe.h 的 PIPE_BUF_SIZE。
 * 灌满它需要一次不多不少 —— 多写一个字节就会阻塞在测例自己身上。
 */
#define PIPE_BUF_SIZE 4096

static int parse_int(const char *s, int fallback)
{
    if (!s || !*s)
        return fallback;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 100000000L)
        return fallback;
    return (int)v;
}

/* ── slot：并发 fork 打任务槽的回收/认领 ─────────────────────────── */

static void slot_worker(int iters, int tag)
{
    int fork_fail = 0, wait_fail = 0;
    int last_wait_errno = 0;
    for (int i = 0; i < iters; i++) {
        pid_t p = fork();
        if (p == 0) {
            /* 子进程立刻退出 —— 目的就是制造 DEAD 槽给父进程回收 */
            _exit(0);
        }
        if (p < 0) {
            fork_fail++;
            if (fork_fail <= 3)
                printf("[w%d] fork failed at %d: %s (%d)\n", tag, i,
                       strerror(errno), errno);
            continue;
        }
        int st = 0;
        if (waitpid(p, &st, 0) < 0) {
            wait_fail++;
            last_wait_errno = errno;
            if (wait_fail <= 3)
                printf("[w%d] waitpid(%d) failed at %d: %s (%d)\n", tag, (int)p,
                       i, strerror(errno), errno);
        }
    }
    printf("[w%d] done: fork_fail=%d wait_fail=%d/%d last_errno=%d\n", tag,
           fork_fail, wait_fail, iters, last_wait_errno);
}

static int cmd_slot(int nprocs, int iters)
{
    printf("racetest slot: procs=%d iters=%d\n", nprocs, iters);
    fflush(stdout);

    pid_t kids[64];
    if (nprocs > 64)
        nprocs = 64;

    for (int i = 0; i < nprocs; i++) {
        pid_t p = fork();
        if (p < 0) {
            printf("fork worker failed: %s\n", strerror(errno));
            break;
        }
        if (p == 0) {
            slot_worker(iters, i);
            _exit(0);
        }
        kids[i] = p;
    }
    for (int i = 0; i < nprocs; i++) {
        if (kids[i] > 0)
            waitpid(kids[i], NULL, 0);
    }
    printf("DONE\n");
    fflush(stdout);
    return 0;
}

/* ── seq：单进程「先 fork 两个、再一起 wait」─────────────────────────
 *
 * 这一条**不需要并发**。fork 路径里的回收循环只判 state == TASK_DEAD，
 * 不判 parent_id —— 建第二个子进程时会把第一个子进程（若已退出）的僵尸
 * 顺手收掉，于是后面 waitpid(first) 拿到 ECHILD。
 * shell 的 `a | b` 就是这个形态：两个子进程都 fork 完才 wait。
 */
static int cmd_seq(int iters)
{
    printf("racetest seq: iters=%d\n", iters);
    fflush(stdout);

    int stolen = 0;
    for (int i = 0; i < iters; i++) {
        pid_t a = fork();
        if (a == 0)
            _exit(0); /* 立刻死，制造僵尸 */

        /* 不等 a，直接 fork b —— 这一步的回收循环可能把 a 收走 */
        pid_t b = fork();
        if (b == 0)
            _exit(0);

        int st = 0;
        if (waitpid(a, &st, 0) < 0) {
            stolen++;
            if (stolen <= 3)
                printf("  iter %d: waitpid(%d) -> %s (%d)  <- 僵尸被偷\n", i,
                       (int)a, strerror(errno), errno);
        }
        waitpid(b, &st, 0);

        if (i % 20 == 0) {
            printf(".");
            fflush(stdout);
        }
    }
    printf("\nDONE: stolen=%d/%d\n", stolen, iters);
    fflush(stdout);
    return 0;
}

/* ── seqw：对照组 —— fork 一个、立刻 wait 一个 ──────────────────────
 *
 * 和 seq 的唯一区别是**不重叠**：任何时候最多只有一个子进程活着。
 * 如果这条干净而 seq 崩，就说明问题出在「建新子进程时顺手回收别人的
 * 僵尸」这条路径上。
 */
static int cmd_seqw(int iters)
{
    printf("racetest seqw: iters=%d\n", iters);
    fflush(stdout);

    int bad = 0;
    for (int i = 0; i < iters; i++) {
        pid_t p = fork();
        if (p == 0)
            _exit(0);
        int st = 0;
        if (waitpid(p, &st, 0) < 0) {
            bad++;
            if (bad <= 3)
                printf("  iter %d: waitpid(%d) -> %s (%d)\n", i, (int)p,
                       strerror(errno), errno);
        }
        if (i % 200 == 0) {
            printf(".");
            fflush(stdout);
        }
    }
    printf("\nDONE: wait_fail=%d/%d\n", bad, iters);
    fflush(stdout);
    return 0;
}

/* ── pipe：读写端丢唤醒 ──────────────────────────────────────────── */

static int cmd_pipe(int iters)
{
    printf("racetest pipe: iters=%d\n", iters);
    fflush(stdout);

    int hung = 0;
    for (int i = 0; i < iters; i++) {
        int fd[2];
        if (pipe(fd) != 0) {
            printf("pipe failed: %s\n", strerror(errno));
            return 1;
        }

        pid_t r = fork();
        if (r == 0) {
            /* reader：立刻阻塞读一个字节 */
            close(fd[1]);
            char c = 0;
            ssize_t n = read(fd[0], &c, 1);
            close(fd[0]);
            _exit(n == 1 ? 0 : 1);
        }

        close(fd[0]);
        /* writer 抢在 reader 发布 blocked_reader 之前把数据写进去 */
        char c = 'x';
        ssize_t wn = write(fd[1], &c, 1);
        (void)wn;
        close(fd[1]);

        int st = 0;
        waitpid(r, &st, 0);

        if (i % 50 == 0) {
            printf(".");
            fflush(stdout);
        }
    }
    (void)hung;
    printf("\nDONE\n");
    fflush(stdout);
    return 0;
}

/* ── wake：两个并发 reader 打同一个 blocked writer ───────────────── */

static int cmd_wake(int iters)
{
    printf("racetest wake: iters=%d (pipe cap=%d)\n", iters, PIPE_BUF_SIZE);
    fflush(stdout);

    static char fill[PIPE_BUF_SIZE];
    memset(fill, 'a', sizeof(fill));

    for (int i = 0; i < iters; i++) {
        int fd[2];
        if (pipe(fd) != 0)
            return 1;

        /*
         * 灌满到**刚好满**（管道容量就是 PIPE_BUF_SIZE）。
         * 多写一个字节这里就会先阻塞住，测例自己挂 —— 之前就是这么错的。
         */
        ssize_t n = write(fd[1], fill, sizeof(fill));
        if (n != (ssize_t)sizeof(fill)) {
            printf("fill short: %zd\n", n);
            return 1;
        }

        /* writer：再写一个满 buf，管道已满 → 阻塞在 p->blocked_writer */
        pid_t w = fork();
        if (w == 0) {
            close(fd[0]);
            write(fd[1], fill, sizeof(fill));
            _exit(0);
        }

        /* 两个 reader 并发读 1 字节：读完之后都会尝试唤醒同一个
         * blocked_writer。两边如果同时看到非 NULL，就是双 unblock。 */
        pid_t r1 = fork();
        if (r1 == 0) {
            close(fd[1]);
            char c;
            read(fd[0], &c, 1);
            _exit(0);
        }
        pid_t r2 = fork();
        if (r2 == 0) {
            close(fd[1]);
            char c;
            read(fd[0], &c, 1);
            _exit(0);
        }

        close(fd[0]);
        close(fd[1]);
        waitpid(w, NULL, 0);
        waitpid(r1, NULL, 0);
        waitpid(r2, NULL, 0);

        if (i % 20 == 0) {
            printf(".");
            fflush(stdout);
        }
    }
    printf("\nDONE\n");
    fflush(stdout);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────── */

static void usage(void)
{
    printf(
        "usage:\n"
        "  racetest slot <procs> <iters>   # 并发 fork，打任务槽回收/认领\n"
        "  racetest seq <iters>            # 单进程 fork 两个再 wait，打僵尸被偷\n"
        "  racetest seqw <iters>           # 对照组：fork 一个 wait 一个\n"
        "  racetest pipe <iters>           # 管道丢唤醒\n"
        "  racetest wake <iters>           # 双唤醒源打 task_unblock\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }

    const char *mode = argv[1];

    if (strcmp(mode, "slot") == 0) {
        int procs = parse_int(argc > 2 ? argv[2] : NULL, 4);
        int iters = parse_int(argc > 3 ? argv[3] : NULL, 200);
        return cmd_slot(procs, iters);
    }
    if (strcmp(mode, "seq") == 0) {
        int iters = parse_int(argc > 2 ? argv[2] : NULL, 200);
        return cmd_seq(iters);
    }
    if (strcmp(mode, "seqw") == 0) {
        int iters = parse_int(argc > 2 ? argv[2] : NULL, 200);
        return cmd_seqw(iters);
    }
    if (strcmp(mode, "pipe") == 0) {
        int iters = parse_int(argc > 2 ? argv[2] : NULL, 2000);
        return cmd_pipe(iters);
    }
    if (strcmp(mode, "wake") == 0) {
        int iters = parse_int(argc > 2 ? argv[2] : NULL, 500);
        return cmd_wake(iters);
    }

    usage();
    return 2;
}
