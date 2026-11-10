/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/debug/rt_latency.c — R0 实时延迟统计

   见 docs/realtime/ARCHITECTURE.md。这里只做“常开、低成本”的近似测量，
   真正的 irq-off trace 后续再上（R0.5）：
     - wake→run：sched_unblock() 记录 task->wake_ns，调度器切入时结算；
     - syscall：用户 syscall 返回路径结算内核执行时长；
     - tick_gap：timer tick 间隔，作为长关中断/长临界区的近似指示。
   数据经 /proc/rtstat 暴露。
*/

#include "rt_latency.h"
#include "task/task.h"
#include "task/cpu.h"
#include "driver/time.h"
#include "string.h"

typedef struct {
    uint64_t wake_max_ns;
    uint64_t wake_last_ns;
    uint64_t wake_count;

    uint64_t syscall_max_ns;
    uint64_t syscall_last_ns;
    uint64_t syscall_count;

    uint64_t tick_gap_max_ns;
    uint64_t tick_gap_last_ns;
    uint64_t tick_count;
    uint64_t last_tick_ns;
} rt_lat_cpu_t;

static rt_lat_cpu_t g_rt_lat[AVATAR_MAX_CPUS];

static inline rt_lat_cpu_t *rt_lat_cpu(void)
{
    uint32_t id = get_current_cpu_id();
    if (id >= AVATAR_MAX_CPUS)
        id = 0;
    return &g_rt_lat[id];
}

void rt_latency_wake(task_t *t)
{
    if (!t)
        return;
    t->wake_ns = timer_get_ns();
}

void rt_latency_run(task_t *t, uint64_t now_ns)
{
    if (!t || t->wake_ns == 0)
        return;

    uint64_t start = t->wake_ns;
    t->wake_ns = 0;
    uint64_t delta = (now_ns > start) ? (now_ns - start) : 0;

    rt_lat_cpu_t *s = rt_lat_cpu();
    s->wake_last_ns = delta;
    if (delta > s->wake_max_ns)
        s->wake_max_ns = delta;
    s->wake_count++;
}

void rt_latency_syscall(uint64_t duration_ns)
{
    rt_lat_cpu_t *s = rt_lat_cpu();
    s->syscall_last_ns = duration_ns;
    if (duration_ns > s->syscall_max_ns)
        s->syscall_max_ns = duration_ns;
    s->syscall_count++;
}

void rt_latency_tick(uint32_t cpu_id, uint64_t now_ns)
{
    if (cpu_id >= AVATAR_MAX_CPUS)
        return;
    rt_lat_cpu_t *s = &g_rt_lat[cpu_id];

    if (s->last_tick_ns != 0 && now_ns > s->last_tick_ns) {
        uint64_t gap = now_ns - s->last_tick_ns;
        s->tick_gap_last_ns = gap;
        if (gap > s->tick_gap_max_ns)
            s->tick_gap_max_ns = gap;
    }
    s->last_tick_ns = now_ns;
    s->tick_count++;
}

/* ── /proc/rtstat 文本生成 ───────────────────────────────── */

static size_t lat_puts(char *buf, size_t cap, size_t pos, const char *s)
{
    while (*s && pos + 1 < cap)
        buf[pos++] = *s++;
    if (pos < cap)
        buf[pos] = '\0';
    return pos;
}

static size_t lat_put_u64(char *buf, size_t cap, size_t pos, uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (int)(v % 10));
            v /= 10;
        }
    }
    while (n > 0 && pos + 1 < cap)
        buf[pos++] = tmp[--n];
    if (pos < cap)
        buf[pos] = '\0';
    return pos;
}

int rt_latency_snprintf(char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return 0;

    size_t pos = 0;
    pos = lat_puts(buf, cap, pos, "rtstat: per-CPU latency (ns)\n");
    pos =
        lat_puts(buf, cap, pos,
                 "cpu wake_max wake_last wake_n syscall_max syscall_last syscall_n tick_gap_max tick_gap_last ticks\n");

    uint32_t n = g_num_cpus ? g_num_cpus : 1U;
    for (uint32_t i = 0; i < n && i < AVATAR_MAX_CPUS; i++) {
        rt_lat_cpu_t *s = &g_rt_lat[i];
        pos = lat_puts(buf, cap, pos, "cpu");
        pos = lat_put_u64(buf, cap, pos, i);
        pos = lat_puts(buf, cap, pos, " ");
        pos = lat_put_u64(buf, cap, pos, s->wake_max_ns);
        pos = lat_puts(buf, cap, pos, " ");
        pos = lat_put_u64(buf, cap, pos, s->wake_last_ns);
        pos = lat_puts(buf, cap, pos, " ");
        pos = lat_put_u64(buf, cap, pos, s->wake_count);
        pos = lat_puts(buf, cap, pos, " ");
        pos = lat_put_u64(buf, cap, pos, s->syscall_max_ns);
        pos = lat_puts(buf, cap, pos, " ");
        pos = lat_put_u64(buf, cap, pos, s->syscall_last_ns);
        pos = lat_puts(buf, cap, pos, " ");
        pos = lat_put_u64(buf, cap, pos, s->syscall_count);
        pos = lat_puts(buf, cap, pos, " ");
        pos = lat_put_u64(buf, cap, pos, s->tick_gap_max_ns);
        pos = lat_puts(buf, cap, pos, " ");
        pos = lat_put_u64(buf, cap, pos, s->tick_gap_last_ns);
        pos = lat_puts(buf, cap, pos, " ");
        pos = lat_put_u64(buf, cap, pos, s->tick_count);
        pos = lat_puts(buf, cap, pos, "\n");
    }

    return (int)pos;
}
