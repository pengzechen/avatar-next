/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   apps/c/rt_event_test.c — /dev/rtctl event 模板的用户态端到端测试

   流程（与内核 rt_event_test 的容量测试一致）：
     1. create event worker（period=0，无限等待事件，gain=2.0）；
     2. start 之前投 100 条事件：前 64 条入环，后 36 条 ENOSPC；
     3. start，等 processed=64，output=last(64)*2=128；
     4. 再投 20 条（101..120），等 processed=84、output=240；
     5. mmap 共享页核对环计数；
     6. stop（worker 此时阻塞在事件等待）→ 等 STOPPED → destroy。

   成功输出 [PASS] 行和 "=== Done ==="，供 tests/suites/apps.py 解析。
*/

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rtctl_abi.h"

static int g_fail;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("[PASS] %s\n", what);
    } else {
        printf("[FAIL] %s\n", what);
        g_fail = 1;
    }
}

static int rt_stats(int fd, uint32_t id, struct rtw_stats *st)
{
    memset(st, 0, sizeof(*st));
    st->id = id;
    return ioctl(fd, RTW_IOC_STATS, st);
}

static int wait_processed(int fd, uint32_t id, uint64_t want, int timeout_ms)
{
    struct rtw_stats st;

    for (int i = 0; i < timeout_ms; i++) {
        if (rt_stats(fd, id, &st) == 0 && st.events_processed >= want)
            return 0;
        usleep(1000);
    }
    return -1;
}

int main(void)
{
    int fd = open("/dev/rtctl", O_RDWR);
    if (fd < 0) {
        printf("[FAIL] open /dev/rtctl: %d\n", errno);
        printf("=== Done ===\n");
        return 1;
    }

    struct rtw_create_req cr;
    memset(&cr, 0, sizeof(cr));
    cr.abi_version = RTW_ABI_VERSION;
    cr.template_id = RTW_TPL_EVENT;
    snprintf(cr.name, sizeof(cr.name), "%s", "evt-user");
    cr.priority = 50;
    cr.cpu = 0;
    cr.period_ns = 0;      /* 无限等待事件 */
    cr.params[0] = 131072; /* gain 2.0 */

    if (ioctl(fd, RTW_IOC_CREATE, &cr) < 0) {
        printf("[FAIL] create event worker: %d\n", errno);
        close(fd);
        printf("=== Done ===\n");
        return 1;
    }
    uint32_t id = cr.id;
    check(id >= 1, "create event worker");

    int ok = 0, dropped = 0, other = 0;
    for (uint64_t i = 1; i <= 100; i++) {
        struct rtw_io_req r;
        memset(&r, 0, sizeof(r));
        r.id = id;
        r.value = i;
        if (ioctl(fd, RTW_IOC_POST, &r) == 0)
            ok++;
        else if (errno == ENOSPC)
            dropped++;
        else
            other++;
    }
    check(ok == 64 && dropped == 36 && other == 0, "ring capacity: 64 accepted / 36 ENOSPC");

    struct rtw_stats st;
    check(rt_stats(fd, id, &st) == 0 && st.events_posted == 64 && st.events_processed == 0 && st.events_dropped == 36,
          "stats before start");

    struct rtw_id_req ir;
    memset(&ir, 0, sizeof(ir));
    ir.id = id;
    check(ioctl(fd, RTW_IOC_START, &ir) == 0, "start");

    check(wait_processed(fd, id, 64, 5000) == 0, "64 events processed");
    check(rt_stats(fd, id, &st) == 0 && st.last_output == 128 && st.ticks == 64, "output=128 after 64 events");

    for (uint64_t i = 101; i <= 120; i++) {
        struct rtw_io_req r;
        memset(&r, 0, sizeof(r));
        r.id = id;
        r.value = i;
        if (ioctl(fd, RTW_IOC_POST, &r) < 0) {
            printf("[FAIL] post %llu: %d\n", (unsigned long long)i, errno);
            g_fail = 1;
            break;
        }
    }
    check(wait_processed(fd, id, 84, 5000) == 0, "20 more events processed");
    check(rt_stats(fd, id, &st) == 0 && st.last_output == 240 && st.events_posted == 84 && st.events_processed == 84 &&
              st.events_dropped == 36,
          "stats after 84 events");

    /* 共享页：环计数与 magic/version 对用户态可见 */
    struct rtw_shm *shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)(id << 12));
    if (shm == MAP_FAILED) {
        printf("[FAIL] mmap shared page: %d\n", errno);
        g_fail = 1;
    } else {
        struct rtw_event_ring *ev = rtw_shm_events(shm);
        check(shm->magic == RTSHM_MAGIC && shm->version == RTSHM_VERSION, "shared page magic/version");
        check(ev->posted == 84 && __atomic_load_n(&ev->processed, __ATOMIC_ACQUIRE) == 84 && ev->dropped == 36 &&
                  ev->last_value == 120,
              "shared event ring counters");
        munmap(shm, 4096);
    }

    check(ioctl(fd, RTW_IOC_STOP, &ir) == 0, "stop blocked event worker");
    int stopped = 0;
    for (int i = 0; i < 2000; i++) {
        if (rt_stats(fd, id, &st) == 0 && st.state == RTW_STATE_STOPPED) {
            stopped = 1;
            break;
        }
        usleep(1000);
    }
    check(stopped, "worker reaches STOPPED");

    check(ioctl(fd, RTW_IOC_DESTROY, &ir) == 0, "destroy");
    close(fd);

    printf("=== Done ===\n");
    return g_fail;
}
