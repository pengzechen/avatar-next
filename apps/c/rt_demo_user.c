/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   apps/c/rt_demo_user.c — 用户态 RT 演示（/bin/rt-demo）

   演示现有 RT 设施的四件事：
     1. event 模板：supervisor post 事件，RT worker 在被唤醒后执行 step；
        测量 post→out_seq 可见的延迟（包含 syscall 与调度）。
     2. 周期 worker：优先尝试 1ms（x86 oneshot），不支持则回退 10ms；
        通过共享页写 input，统计 ticks/misses/exec_max/dev_writes。
     3. watchdog 模板：停掉被监控的周期 worker 后，输出切到安全值。
     4. /proc/rtworkers 快照（EVENTS/DROPS 列）。

   运行：/bin/rt-demo（root）。结束输出 "=== Done ===" 供测试框架解析。
*/

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "rtctl_abi.h"

static int g_fail;

static void pass(const char *what)
{
    printf("[PASS] %s\n", what);
}

static void fail(const char *what)
{
    printf("[FAIL] %s\n", what);
    g_fail = 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int rt_stats(int fd, uint32_t id, struct rtw_stats *st)
{
    memset(st, 0, sizeof(*st));
    st->id = id;
    return ioctl(fd, RTW_IOC_STATS, st);
}

static int rt_start(int fd, uint32_t id)
{
    struct rtw_id_req r = { .id = id };
    return ioctl(fd, RTW_IOC_START, &r);
}

static int rt_stop(int fd, uint32_t id)
{
    struct rtw_id_req r = { .id = id };
    return ioctl(fd, RTW_IOC_STOP, &r);
}

static int rt_destroy(int fd, uint32_t id)
{
    struct rtw_id_req r = { .id = id };
    return ioctl(fd, RTW_IOC_DESTROY, &r);
}

static int rt_post(int fd, uint32_t id, uint64_t value)
{
    struct rtw_io_req r = { .id = id, .value = value };
    return ioctl(fd, RTW_IOC_POST, &r);
}

static int rt_get(int fd, uint32_t id, uint64_t *value)
{
    struct rtw_io_req r = { .id = id };
    if (ioctl(fd, RTW_IOC_GET_OUT, &r) < 0)
        return -1;
    if (value)
        *value = r.value;
    return 0;
}

static int create_worker(int fd, const char *name, uint32_t tpl, uint8_t prio, uint32_t cpu, uint64_t period_ns,
                         const uint64_t *params, const char *dev)
{
    struct rtw_create_req cr;
    memset(&cr, 0, sizeof(cr));
    cr.abi_version = RTW_ABI_VERSION;
    cr.template_id = tpl;
    snprintf(cr.name, sizeof(cr.name), "%s", name);
    snprintf(cr.dev, sizeof(cr.dev), "%s", dev ? dev : "");
    cr.priority = prio;
    cr.cpu = cpu;
    cr.period_ns = period_ns;
    if (params) {
        for (int i = 0; i < RTW_PARAM_MAX; i++)
            cr.params[i] = params[i];
    }
    if (ioctl(fd, RTW_IOC_CREATE, &cr) < 0)
        return -1;
    return (int)cr.id;
}

static struct rtw_shm *map_shm(int fd, uint32_t id)
{
    struct rtw_shm *s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)(id << 12));
    if (s == MAP_FAILED)
        return NULL;
    if (s->magic != RTSHM_MAGIC || s->version != RTSHM_VERSION) {
        munmap(s, 4096);
        return NULL;
    }
    return s;
}

static void stop_destroy(int fd, int id)
{
    if (id < 0)
        return;
    (void)rt_stop(fd, (uint32_t)id);
    struct rtw_stats st;
    for (int i = 0; i < 1000; i++) {
        if (rt_stats(fd, (uint32_t)id, &st) == 0 && (st.state == RTW_STATE_STOPPED || st.state == RTW_STATE_CREATED))
            break;
        usleep(1000);
    }
    (void)rt_destroy(fd, (uint32_t)id);
}

static void print_proc_rtworkers(void)
{
    int fd = open("/proc/rtworkers", O_RDONLY);
    char buf[1024];

    if (fd < 0) {
        printf("(cannot open /proc/rtworkers: %d)\n", errno);
        return;
    }
    printf("--- /proc/rtworkers ---\n");
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
    printf("-----------------------\n");
}

int main(void)
{
    int fd = open("/dev/rtctl", O_RDWR);
    if (fd < 0) {
        printf("[FAIL] open /dev/rtctl: %d\n", errno);
        printf("=== Done ===\n");
        return 1;
    }

    printf("=== Avatar RT demo ===\n");

    int evt_id = -1, pulse_id = -1, wd_id = -1;
    struct rtw_shm *evt_shm = NULL;
    struct rtw_shm *pulse_shm = NULL;

    /* ── 1. event 模板：post → worker → out_seq ───────────────── */
    uint64_t evt_params[RTW_PARAM_MAX] = { 131072, 0, 0, 0 }; /* gain 2.0 */
    evt_id = create_worker(fd, "evt", RTW_TPL_EVENT, 50, 0, 0, evt_params, NULL);
    if (evt_id < 0) {
        fail("create event worker");
        goto out;
    }
    evt_shm = map_shm(fd, (uint32_t)evt_id);
    if (!evt_shm) {
        fail("map event shared page");
        goto out;
    }
    if (rt_start(fd, (uint32_t)evt_id) < 0) {
        fail("start event worker");
        goto out;
    }

    const int EVENTS = 200;
    uint64_t lat_min = ~0ULL, lat_max = 0, lat_sum = 0;
    int lat_n = 0, timeouts = 0;
    for (int i = 1; i <= EVENTS; i++) {
        uint64_t seq0 = __atomic_load_n(&evt_shm->out_seq, __ATOMIC_ACQUIRE);
        uint64_t t0 = now_ns();
        if (rt_post(fd, (uint32_t)evt_id, (uint64_t)i) < 0) {
            fail("post event");
            break;
        }
        uint64_t deadline = t0 + 100000000ULL; /* 100ms 兜底 */
        while (__atomic_load_n(&evt_shm->out_seq, __ATOMIC_ACQUIRE) == seq0) {
            if (now_ns() >= deadline)
                break;
        }
        uint64_t t1 = now_ns();
        if (__atomic_load_n(&evt_shm->out_seq, __ATOMIC_ACQUIRE) == seq0) {
            timeouts++;
            continue;
        }
        uint64_t lat = t1 - t0;
        if (lat < lat_min)
            lat_min = lat;
        if (lat > lat_max)
            lat_max = lat;
        lat_sum += lat;
        lat_n++;
    }
    uint64_t evt_out = __atomic_load_n(&evt_shm->output, __ATOMIC_ACQUIRE);
    struct rtw_stats est;
    memset(&est, 0, sizeof(est));
    if (rt_stats(fd, (uint32_t)evt_id, &est) == 0) {
        printf("[event] n=%d min=%llu avg=%llu max=%llu ns timeouts=%d\n", lat_n, (unsigned long long)lat_min,
               (unsigned long long)(lat_n ? lat_sum / (uint64_t)lat_n : 0), (unsigned long long)lat_max, timeouts);
        printf("[event] ticks=%llu posted=%llu processed=%llu dropped=%llu output=%llu\n",
               (unsigned long long)est.ticks, (unsigned long long)est.events_posted,
               (unsigned long long)est.events_processed, (unsigned long long)est.events_dropped,
               (unsigned long long)evt_out);
    }
    if (lat_n == EVENTS && timeouts == 0 && evt_out == (uint64_t)EVENTS * 2 &&
        est.events_processed == (uint64_t)EVENTS && est.events_dropped == 0)
        pass("event-driven worker: 200 events, output = last event * 2");
    else
        fail("event-driven worker output/stats");

    /* ── 2. 周期 worker：1ms 优先，失败回退 10ms ─────────────── */
    uint64_t period_ns = 1000000ULL;
    uint64_t pulse_params[RTW_PARAM_MAX] = { 65536, 0, 0, 0 }; /* gain 1.0 */
    pulse_id = create_worker(fd, "pulse", RTW_TPL_PERIODIC_IO, 60, 0, period_ns, pulse_params, "vout");
    if (pulse_id < 0) {
        period_ns = 10000000ULL;
        pulse_id = create_worker(fd, "pulse", RTW_TPL_PERIODIC_IO, 60, 0, period_ns, pulse_params, "vout");
    }
    if (pulse_id < 0) {
        fail("create periodic worker");
        goto out;
    }
    pulse_shm = map_shm(fd, (uint32_t)pulse_id);
    if (!pulse_shm) {
        fail("map periodic shared page");
        goto out;
    }
    if (rt_start(fd, (uint32_t)pulse_id) < 0) {
        fail("start periodic worker");
        goto out;
    }
    __atomic_store_n(&pulse_shm->input, 123, __ATOMIC_RELEASE);
    usleep(1000000);

    struct rtw_stats pst;
    if (rt_stats(fd, (uint32_t)pulse_id, &pst) < 0) {
        fail("periodic stats");
    } else {
        printf("[periodic] period=%llu ns ticks=%llu misses=%llu exec_max=%llu ns wake_max=%llu ns "
               "dev_writes=%llu output=%llu\n",
               (unsigned long long)period_ns, (unsigned long long)pst.ticks, (unsigned long long)pst.misses,
               (unsigned long long)pst.exec_max_ns, (unsigned long long)pst.wake_late_max_ns,
               (unsigned long long)pst.dev_writes, (unsigned long long)pst.last_output);
        if (pst.ticks > 0 && pst.last_output == 123 && pst.dev_writes > 0)
            pass("periodic worker: shared input -> output, vout writes counted");
        else
            fail("periodic worker output/stats");
    }

    /* ── 3. watchdog：停掉 pulse 后输出安全值 ────────────────── */
    uint64_t wd_params[RTW_PARAM_MAX] = { (uint64_t)pulse_id, 50000000ULL, 999 };
    wd_id = create_worker(fd, "wd", RTW_TPL_WATCHDOG, 70, 0, period_ns, wd_params, NULL);
    if (wd_id < 0) {
        fail("create watchdog");
    } else if (rt_start(fd, (uint32_t)wd_id) < 0) {
        fail("start watchdog");
    } else {
        usleep(200000);
        uint64_t healthy = 0;
        (void)rt_get(fd, (uint32_t)wd_id, &healthy);
        printf("[watchdog] target healthy output=%llu\n", (unsigned long long)healthy);

        if (rt_stop(fd, (uint32_t)pulse_id) < 0) {
            fail("stop pulse");
        } else {
            usleep(150000);
            uint64_t safe = 0;
            (void)rt_get(fd, (uint32_t)wd_id, &safe);
            printf("[watchdog] after target stop output=%llu (safe=999)\n", (unsigned long long)safe);
            if (safe == 999)
                pass("watchdog: safe output after target stopped");
            else
                fail("watchdog safe output");
        }
    }

    print_proc_rtworkers();

out:
    /* 清理顺序：wd → pulse → evt */
    stop_destroy(fd, wd_id);
    stop_destroy(fd, pulse_id);
    stop_destroy(fd, evt_id);
    close(fd);

    printf("=== Done ===\n");
    return g_fail;
}
