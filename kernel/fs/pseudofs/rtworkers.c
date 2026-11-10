/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   /proc/rtworkers — 用户可配置 RT worker 的快照

   /dev/rtctl 的 STATS/LIST 需要 ioctl；这个 proc 文件让 shell 直接
   `cat /proc/rtworkers`，也给 supervisor 一个无 syscall 的观察面。

   列宽是固定的；表头和每一行都走同一组 put_col/put_num_col，改列宽时
   只需要改下面的 W_* 宏，避免表头和内容各写一套 padding 迟早对不上。
*/

#include "pseudofs_internal.h"
#include "rt_worker.h"

#define W_ID       2
#define W_NAME     16
#define W_STATE    8
#define W_TPL      12
#define W_PRIO     4
#define W_CPU      3
#define W_PERIOD   9
#define W_TICKS    10
#define W_MISSES   6
#define W_OVERRUNS 8
#define W_EXEC_MAX 11
#define W_WAKE_MAX 11
#define W_DEV      4

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

/*
   str 按 width 对齐；right=true 右对齐（数字），false 左对齐（文本）。
   超长时直接原样输出，不截断 —— 宁可某一行错位，也不要悄悄吞掉字段。
*/
static size_t put_col(char *buf, size_t pos, size_t bufsz, const char *str, int width, bool right)
{
    int len = (int)pfs_strlen(str);
    int pad = width > len ? width - len : 0;

    if (right) {
        for (int i = 0; i < pad; i++)
            pos += (size_t)pfs_puts(buf, pos, bufsz, " ");
        pos += (size_t)pfs_puts(buf, pos, bufsz, str);
    } else {
        pos += (size_t)pfs_puts(buf, pos, bufsz, str);
        for (int i = 0; i < pad; i++)
            pos += (size_t)pfs_puts(buf, pos, bufsz, " ");
    }
    return pos;
}

static size_t put_num_col(char *buf, size_t pos, size_t bufsz, uint64_t v, int width)
{
    char nbuf[24];
    u64_to_dec(nbuf, v);
    return put_col(buf, pos, bufsz, nbuf, width, true);
}

static size_t put_sep(char *buf, size_t pos, size_t bufsz)
{
    return pos + (size_t)pfs_puts(buf, pos, bufsz, " ");
}

int rtworkers_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;

    char tmp[2048];
    size_t pos = 0;

    pos = put_col(tmp, pos, sizeof(tmp), "ID", W_ID, true);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "NAME", W_NAME, false);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "STATE", W_STATE, false);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "TPL", W_TPL, false);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "PRIO", W_PRIO, true);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "CPU", W_CPU, true);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "PERIOD_NS", W_PERIOD, true);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "TICKS", W_TICKS, true);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "MISSES", W_MISSES, true);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "OVERRUNS", W_OVERRUNS, true);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "EXEC_MAX_NS", W_EXEC_MAX, true);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "WAKE_MAX_NS", W_WAKE_MAX, true);
    pos = put_sep(tmp, pos, sizeof(tmp));
    pos = put_col(tmp, pos, sizeof(tmp), "DEV", W_DEV, false);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "\n");

    for (uint32_t id = 1; id <= RTW_MAX_WORKERS; id++) {
        struct rtw_stats st;
        if (rt_worker_get_stats(id, &st) < 0)
            continue;

        pos = put_num_col(tmp, pos, sizeof(tmp), st.id, W_ID);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_col(tmp, pos, sizeof(tmp), st.name, W_NAME, false);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_col(tmp, pos, sizeof(tmp), rtw_state_name(st.state), W_STATE, false);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_col(tmp, pos, sizeof(tmp), rtw_tpl_name(st.template_id), W_TPL, false);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_num_col(tmp, pos, sizeof(tmp), st.priority, W_PRIO);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_num_col(tmp, pos, sizeof(tmp), st.cpu, W_CPU);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_num_col(tmp, pos, sizeof(tmp), st.period_ns, W_PERIOD);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_num_col(tmp, pos, sizeof(tmp), st.ticks, W_TICKS);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_num_col(tmp, pos, sizeof(tmp), st.misses, W_MISSES);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_num_col(tmp, pos, sizeof(tmp), st.overruns, W_OVERRUNS);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_num_col(tmp, pos, sizeof(tmp), st.exec_max_ns, W_EXEC_MAX);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_num_col(tmp, pos, sizeof(tmp), st.wake_late_max_ns, W_WAKE_MAX);
        pos = put_sep(tmp, pos, sizeof(tmp));
        pos = put_col(tmp, pos, sizeof(tmp), st.dev[0] ? st.dev : "-", W_DEV, false);
        pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "\n");
    }

    return pfs_copy_out(off, buf, len, tmp, pos);
}
