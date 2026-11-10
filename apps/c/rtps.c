/*
   SPDX-License-Identifier: MIT

   rtps — 观察 /proc 里的实时任务（RT policy + priority）

   背景：busybox ps 只读 /proc/<pid>/stat，看不到调度策略。内核的
   /proc/<pid>/status 现在提供：

       Policy:	1 (SCHED_FIFO)
       RtPrio:	1

   本程序枚举 /proc 下的数字 PID 目录，解析 status，按 PID 排序输出
   PID / PPID / POLICY / PRIO / STATE / NAME。用法：

       rtps          # 所有任务
       rtps -r       # 只看 RT 任务（policy != SCHED_NORMAL 或 prio > 0）

   静态链接（和 racetest 一样），不依赖 rootfs 里的动态 loader。
*/

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TASKS 256

struct task_info {
    int pid;
    int ppid;
    int prio;
    char policy[24];
    char state;
    char name[32];
};

static void usage(const char *argv0)
{
    printf("usage: %s [-r]\n", argv0);
    printf("  -r  only show RT tasks (SCHED_FIFO/SCHED_RR)\n");
    printf("  -h  show this help\n");
}

static void policy_name(int num, char *out, size_t outsz)
{
    switch (num) {
    case 1:
        snprintf(out, outsz, "SCHED_FIFO");
        break;
    case 2:
        snprintf(out, outsz, "SCHED_RR");
        break;
    default:
        snprintf(out, outsz, "SCHED_NORMAL");
        break;
    }
}

static int read_task(const char *pid_name, struct task_info *ti)
{
    char path[128];
    char line[256];
    int policy_num = 0;
    int saw_policy_name = 0;
    FILE *f;

    memset(ti, 0, sizeof(*ti));
    ti->state = '?';
    snprintf(ti->name, sizeof(ti->name), "?");
    snprintf(ti->policy, sizeof(ti->policy), "SCHED_NORMAL");

    /* PID 目录名只可能是数字；限宽避免 -Wformat-truncation 噪声 */
    snprintf(path, sizeof(path), "/proc/%.32s/status", pid_name);
    f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "Name:", 5)) {
            if (sscanf(line + 5, "%31s", ti->name) != 1)
                snprintf(ti->name, sizeof(ti->name), "?");
        } else if (!strncmp(line, "State:", 6)) {
            sscanf(line + 6, " %c", &ti->state);
        } else if (!strncmp(line, "Pid:", 4)) {
            sscanf(line + 4, "%d", &ti->pid);
        } else if (!strncmp(line, "PPid:", 5)) {
            sscanf(line + 5, "%d", &ti->ppid);
        } else if (!strncmp(line, "Policy:", 7)) {
            char pname[24] = "";
            if (sscanf(line + 7, " %d (%23[^)])", &policy_num, pname) >= 1) {
                if (pname[0]) {
                    snprintf(ti->policy, sizeof(ti->policy), "%s", pname);
                    saw_policy_name = 1;
                }
            }
        } else if (!strncmp(line, "RtPrio:", 7)) {
            sscanf(line + 7, "%d", &ti->prio);
        }
    }
    fclose(f);

    if (!saw_policy_name)
        policy_name(policy_num, ti->policy, sizeof(ti->policy));
    return 0;
}

static int cmp_pid(const void *a, const void *b)
{
    const struct task_info *ta = a;
    const struct task_info *tb = b;
    return (ta->pid > tb->pid) - (ta->pid < tb->pid);
}

static int is_rt(const struct task_info *ti)
{
    return ti->prio > 0 || strcmp(ti->policy, "SCHED_NORMAL") != 0;
}

int main(int argc, char **argv)
{
    struct task_info tasks[MAX_TASKS];
    int only_rt = 0;
    int n = 0;
    int shown = 0;
    DIR *dir;
    struct dirent *de;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) {
            only_rt = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "rtps: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    dir = opendir("/proc");
    if (!dir) {
        perror("rtps: opendir /proc");
        return 1;
    }

    while ((de = readdir(dir)) != NULL) {
        struct task_info ti;

        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        if (n >= MAX_TASKS)
            break;
        if (read_task(de->d_name, &ti) != 0 || ti.pid <= 0)
            continue;
        if (only_rt && !is_rt(&ti))
            continue;
        tasks[n++] = ti;
    }
    closedir(dir);

    qsort(tasks, (size_t)n, sizeof(tasks[0]), cmp_pid);

    printf("%5s %5s %-13s %4s %c %s\n", "PID", "PPID", "POLICY", "PRIO", 'S', "NAME");
    for (int i = 0; i < n; i++) {
        printf("%5d %5d %-13s %4d %c %s", tasks[i].pid, tasks[i].ppid, tasks[i].policy, tasks[i].prio, tasks[i].state,
               tasks[i].name);
        if (is_rt(&tasks[i]))
            printf("  [RT]");
        printf("\n");
        shown++;
    }

    if (only_rt && shown == 0)
        printf("(no RT tasks)\n");

    return 0;
}
