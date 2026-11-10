/*
   SPDX-License-Identifier: MIT

   rtctl — /dev/rtctl 用户态控制面

   ABI 必须与 include/rt_worker.h 逐字节一致（结构体尺寸被编进 ioctl 号，
   尺寸不同就是另一个命令）。内核那份是权威定义；这里只做用户态副本。

   用法：
     rtctl create <name> <tpl> <prio> <cpu> <period_ms> [p0..p7] [dev:<name>]
     rtctl start <id>
     rtctl stop <id>
     rtctl destroy <id>
     rtctl set <id> <value>
     rtctl get <id>
     rtctl stats <id>
     rtctl list

   模板：
     periodic_io  p0=gain_q16 p1=offset p2=min p3=max
     pid          p0=kp_q16 p1=ki_q16 p2=kd_q16 p3=out_min
                  p4=out_max p5=setpoint p6=integral_limit
*/

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define RTW_NAME_LEN     16
#define RTW_DEV_NAME_LEN 16
#define RTW_MAX_WORKERS  8
#define RTW_PARAM_MAX    8

enum {
    RTW_TPL_PERIODIC_IO = 1,
    RTW_TPL_PID = 2
};
enum {
    RTW_STATE_FREE = 0,
    RTW_STATE_CREATED,
    RTW_STATE_RUNNING,
    RTW_STATE_STOPPED,
    RTW_STATE_FAULTED
};

struct rtw_create_req {
    uint32_t abi_version;
    uint32_t template_id;
    char name[RTW_NAME_LEN];
    char dev[RTW_DEV_NAME_LEN];
    uint8_t priority;
    uint8_t _pad0[3];
    uint32_t cpu;
    uint64_t period_ns;
    uint64_t budget_ns;
    uint64_t params[RTW_PARAM_MAX];
    uint32_t id;
    uint32_t _pad1;
};

struct rtw_id_req {
    uint32_t id;
    uint32_t _pad;
};

struct rtw_io_req {
    uint32_t id;
    uint32_t _pad;
    uint64_t seq;
    uint64_t value;
};

struct rtw_stats {
    uint32_t id;
    uint32_t state;
    uint32_t template_id;
    uint32_t priority;
    uint32_t cpu;
    uint32_t _pad;
    uint64_t period_ns;
    uint64_t budget_ns;
    uint64_t ticks;
    uint64_t misses;
    uint64_t overruns;
    uint64_t exec_last_ns;
    uint64_t exec_max_ns;
    uint64_t last_input;
    uint64_t last_output;
    uint64_t out_seq;
    uint64_t dev_writes;
    char name[RTW_NAME_LEN];
    char dev[RTW_DEV_NAME_LEN];
};

struct rtw_list_entry {
    uint32_t id;
    uint32_t state;
    uint32_t template_id;
    uint32_t priority;
    uint32_t cpu;
    uint32_t _pad;
    uint64_t period_ns;
    uint64_t ticks;
    uint64_t misses;
    char name[RTW_NAME_LEN];
};

struct rtw_list_req {
    uint32_t count;
    uint32_t capacity;
    struct rtw_list_entry entries[RTW_MAX_WORKERS];
};

_Static_assert(sizeof(struct rtw_create_req) == 136, "rtw_create_req ABI mismatch");
_Static_assert(sizeof(struct rtw_io_req) == 24, "rtw_io_req ABI mismatch");
_Static_assert(sizeof(struct rtw_stats) == 144, "rtw_stats ABI mismatch");
_Static_assert(sizeof(struct rtw_list_entry) == 64, "rtw_list_entry ABI mismatch");
_Static_assert(sizeof(struct rtw_list_req) == 520, "rtw_list_req ABI mismatch");

#ifndef _IOC
#define _IOC(dir, t, nr, sz) (((uint32_t)(dir) << 30) | ((uint32_t)(t) << 8) | (uint32_t)(nr) | ((uint32_t)(sz) << 16))
#define _IOC_WRITE           1U
#define _IOC_READ            2U
#define _IOWR(t, nr, T)      _IOC(_IOC_READ | _IOC_WRITE, (t), (nr), sizeof(T))
#endif

#define RTW_IOC_CREATE  _IOWR('R', 1, struct rtw_create_req)
#define RTW_IOC_DESTROY _IOWR('R', 2, struct rtw_id_req)
#define RTW_IOC_START   _IOWR('R', 3, struct rtw_id_req)
#define RTW_IOC_STOP    _IOWR('R', 4, struct rtw_id_req)
#define RTW_IOC_SET_IN  _IOWR('R', 5, struct rtw_io_req)
#define RTW_IOC_GET_OUT _IOWR('R', 6, struct rtw_io_req)
#define RTW_IOC_STATS   _IOWR('R', 7, struct rtw_stats)
#define RTW_IOC_LIST    _IOWR('R', 8, struct rtw_list_req)

static const char *tpl_name(uint32_t id)
{
    switch (id) {
    case RTW_TPL_PERIODIC_IO:
        return "periodic_io";
    case RTW_TPL_PID:
        return "pid";
    default:
        return "?";
    }
}

static const char *state_name(uint32_t s)
{
    switch (s) {
    case RTW_STATE_CREATED:
        return "CREATED";
    case RTW_STATE_RUNNING:
        return "RUNNING";
    case RTW_STATE_STOPPED:
        return "STOPPED";
    case RTW_STATE_FAULTED:
        return "FAULTED";
    default:
        return "FREE";
    }
}

static uint32_t tpl_id(const char *s)
{
    if (!strcmp(s, "periodic_io"))
        return RTW_TPL_PERIODIC_IO;
    if (!strcmp(s, "pid"))
        return RTW_TPL_PID;
    return 0;
}

static int open_dev(void)
{
    int fd = open("/dev/rtctl", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "rtctl: open /dev/rtctl: %s\n", strerror(errno));
        return -1;
    }
    return fd;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!s[0] || !end || *end)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int cmd_create(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: rtctl create <name> <tpl> <prio> <cpu> <period_ms> [p0..p7] [dev:<name>]\n");
        return 2;
    }

    struct rtw_create_req r;
    memset(&r, 0, sizeof(r));
    r.abi_version = 1;
    r.template_id = tpl_id(argv[3]);
    if (!r.template_id) {
        fprintf(stderr, "rtctl: unknown template '%s'\n", argv[3]);
        return 2;
    }
    snprintf(r.name, sizeof(r.name), "%s", argv[2]);

    uint32_t prio, cpu, period_ms;
    if (parse_u32(argv[4], &prio) || parse_u32(argv[5], &cpu) || parse_u32(argv[6], &period_ms)) {
        fprintf(stderr, "rtctl: bad prio/cpu/period\n");
        return 2;
    }
    if (prio < 1 || prio > 99 || period_ms == 0) {
        fprintf(stderr, "rtctl: prio 1..99 and period_ms > 0 required\n");
        return 2;
    }
    r.priority = (uint8_t)prio;
    r.cpu = cpu;
    r.period_ns = (uint64_t)period_ms * 1000000ULL;

    int np = 0;
    for (int i = 7; i < argc; i++) {
        if (!strncmp(argv[i], "dev:", 4)) {
            snprintf(r.dev, sizeof(r.dev), "%s", argv[i] + 4);
        } else if (np < RTW_PARAM_MAX) {
            char *end = NULL;
            unsigned long long v = strtoull(argv[i], &end, 0);
            if (!argv[i][0] || !end || *end) {
                fprintf(stderr, "rtctl: bad param '%s'\n", argv[i]);
                return 2;
            }
            r.params[np++] = (uint64_t)v;
        } else {
            fprintf(stderr, "rtctl: too many params (max %d)\n", RTW_PARAM_MAX);
            return 2;
        }
    }

    int fd = open_dev();
    if (fd < 0)
        return 1;
    if (ioctl(fd, RTW_IOC_CREATE, &r) < 0) {
        fprintf(stderr, "rtctl: create: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("created id=%u name=%s tpl=%s\n", r.id, r.name, tpl_name(r.template_id));
    close(fd);
    return 0;
}

static int cmd_id_op(const char *cmd, uint64_t req, int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: rtctl %s <id>\n", cmd);
        return 2;
    }
    struct rtw_id_req r;
    memset(&r, 0, sizeof(r));
    if (parse_u32(argv[2], &r.id)) {
        fprintf(stderr, "rtctl: bad id\n");
        return 2;
    }
    int fd = open_dev();
    if (fd < 0)
        return 1;
    if (ioctl(fd, req, &r) < 0) {
        fprintf(stderr, "rtctl: %s: %s\n", cmd, strerror(errno));
        close(fd);
        return 1;
    }
    printf("%s id=%u: ok\n", cmd, r.id);
    close(fd);
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: rtctl set <id> <value>\n");
        return 2;
    }
    struct rtw_io_req r;
    memset(&r, 0, sizeof(r));
    if (parse_u32(argv[2], &r.id)) {
        fprintf(stderr, "rtctl: bad id\n");
        return 2;
    }
    char *end = NULL;
    r.value = (uint64_t)strtoull(argv[3], &end, 0);
    if (!argv[3][0] || !end || *end) {
        fprintf(stderr, "rtctl: bad value\n");
        return 2;
    }
    int fd = open_dev();
    if (fd < 0)
        return 1;
    if (ioctl(fd, RTW_IOC_SET_IN, &r) < 0) {
        fprintf(stderr, "rtctl: set: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: rtctl get <id>\n");
        return 2;
    }
    struct rtw_io_req r;
    memset(&r, 0, sizeof(r));
    if (parse_u32(argv[2], &r.id)) {
        fprintf(stderr, "rtctl: bad id\n");
        return 2;
    }
    int fd = open_dev();
    if (fd < 0)
        return 1;
    if (ioctl(fd, RTW_IOC_GET_OUT, &r) < 0) {
        fprintf(stderr, "rtctl: get: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("id=%u value=%lld seq=%llu\n", r.id, (long long)r.value, (unsigned long long)r.seq);
    close(fd);
    return 0;
}

static int cmd_stats(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: rtctl stats <id>\n");
        return 2;
    }
    struct rtw_stats st;
    memset(&st, 0, sizeof(st));
    if (parse_u32(argv[2], &st.id)) {
        fprintf(stderr, "rtctl: bad id\n");
        return 2;
    }
    int fd = open_dev();
    if (fd < 0)
        return 1;
    if (ioctl(fd, RTW_IOC_STATS, &st) < 0) {
        fprintf(stderr, "rtctl: stats: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("id=%u name=%s state=%s tpl=%s prio=%u cpu=%u period=%llu ns\n", st.id, st.name, state_name(st.state),
           tpl_name(st.template_id), st.priority, st.cpu, (unsigned long long)st.period_ns);
    printf("ticks=%llu misses=%llu overruns=%llu exec_last=%llu ns exec_max=%llu ns\n", (unsigned long long)st.ticks,
           (unsigned long long)st.misses, (unsigned long long)st.overruns, (unsigned long long)st.exec_last_ns,
           (unsigned long long)st.exec_max_ns);
    printf("input=%lld output=%lld seq=%llu dev=%s dev_writes=%llu\n", (long long)st.last_input,
           (long long)st.last_output, (unsigned long long)st.out_seq, st.dev[0] ? st.dev : "-",
           (unsigned long long)st.dev_writes);
    close(fd);
    return 0;
}

static int cmd_list(void)
{
    struct rtw_list_req lst;
    memset(&lst, 0, sizeof(lst));
    lst.capacity = RTW_MAX_WORKERS;
    int fd = open_dev();
    if (fd < 0)
        return 1;
    if (ioctl(fd, RTW_IOC_LIST, &lst) < 0) {
        fprintf(stderr, "rtctl: list: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("ID STATE    TPL          PRIO CPU PERIOD_NS TICKS MISSES NAME\n");
    for (uint32_t i = 0; i < lst.count; i++) {
        struct rtw_list_entry *e = &lst.entries[i];
        printf("%2u %-8s %-12s %4u %3u %9llu %5llu %6llu %s\n", e->id, state_name(e->state), tpl_name(e->template_id),
               e->priority, e->cpu, (unsigned long long)e->period_ns, (unsigned long long)e->ticks,
               (unsigned long long)e->misses, e->name);
    }
    close(fd);
    return 0;
}

static void usage(const char *argv0)
{
    printf("usage:\n");
    printf("  %s create <name> <tpl> <prio> <cpu> <period_ms> [p0..p7] [dev:<name>]\n", argv0);
    printf("  %s start|stop|destroy <id>\n", argv0);
    printf("  %s set <id> <value>\n", argv0);
    printf("  %s get <id>\n", argv0);
    printf("  %s stats <id>\n", argv0);
    printf("  %s list\n", argv0);
    printf("templates: periodic_io, pid\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "create"))
        return cmd_create(argc, argv);
    if (!strcmp(argv[1], "start"))
        return cmd_id_op("start", RTW_IOC_START, argc, argv);
    if (!strcmp(argv[1], "stop"))
        return cmd_id_op("stop", RTW_IOC_STOP, argc, argv);
    if (!strcmp(argv[1], "destroy"))
        return cmd_id_op("destroy", RTW_IOC_DESTROY, argc, argv);
    if (!strcmp(argv[1], "set"))
        return cmd_set(argc, argv);
    if (!strcmp(argv[1], "get"))
        return cmd_get(argc, argv);
    if (!strcmp(argv[1], "stats"))
        return cmd_stats(argc, argv);
    if (!strcmp(argv[1], "list"))
        return cmd_list();
    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h")) {
        usage(argv[0]);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
