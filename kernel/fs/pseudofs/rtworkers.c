/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   /proc/rtworkers — 用户可配置 RT worker 的快照

   /dev/rtctl 的 STATS/LIST 需要 ioctl；这个 proc 文件让 shell 直接
   `cat /proc/rtworkers`，也给 supervisor 一个无 syscall 的观察面。
*/

#include "pseudofs_internal.h"
#include "rt_worker.h"

static const char *rtw_state_name(uint32_t s)
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

static const char *rtw_tpl_name(uint32_t t)
{
    switch (t) {
    case RTW_TPL_PERIODIC_IO:
        return "periodic_io";
    case RTW_TPL_PID:
        return "pid";
    case RTW_TPL_WATCHDOG:
        return "watchdog";
    default:
        return "?";
    }
}

static size_t rtw_put_num(char *buf, size_t pos, size_t bufsz, uint64_t v)
{
    char nbuf[24];
    u64_to_dec(nbuf, v);
    return pos + (size_t)pfs_puts(buf, pos, bufsz, nbuf);
}

int rtworkers_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;

    char tmp[2048];
    size_t pos = 0;

    pos += (size_t)pfs_puts(
        tmp, pos, sizeof(tmp),
        "ID NAME             STATE    TPL          PRIO CPU PERIOD_NS TICKS MISSES OVERRUNS EXEC_MAX_NS "
        "WAKE_MAX_NS DEV\n");
    for (uint32_t id = 1; id <= RTW_MAX_WORKERS; id++) {
        struct rtw_stats st;
        if (rt_worker_get_stats(id, &st) < 0)
            continue;

        pos = rtw_put_num(tmp, pos, sizeof(tmp), st.id);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), st.name);
        for (int pad = (int)pfs_strlen(st.name); pad < 16; pad++)
            pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), rtw_state_name(st.state));
        for (int pad = (int)pfs_strlen(rtw_state_name(st.state)); pad < 8; pad++)
            pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), rtw_tpl_name(st.template_id));
        for (int pad = (int)pfs_strlen(rtw_tpl_name(st.template_id)); pad < 12; pad++)
            pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos = rtw_put_num(tmp, pos, sizeof(tmp), st.priority);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos = rtw_put_num(tmp, pos, sizeof(tmp), st.cpu);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos = rtw_put_num(tmp, pos, sizeof(tmp), st.period_ns);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos = rtw_put_num(tmp, pos, sizeof(tmp), st.ticks);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos = rtw_put_num(tmp, pos, sizeof(tmp), st.misses);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos = rtw_put_num(tmp, pos, sizeof(tmp), st.overruns);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos = rtw_put_num(tmp, pos, sizeof(tmp), st.exec_max_ns);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos = rtw_put_num(tmp, pos, sizeof(tmp), st.wake_late_max_ns);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), st.dev[0] ? st.dev : "-");
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "\n");
    }

    return pfs_copy_out(off, buf, len, tmp, pos);
}
