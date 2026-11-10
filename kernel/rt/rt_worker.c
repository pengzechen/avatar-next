/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/rt/rt_worker.c — 用户可配置的 RT worker（模板执行器）

   数据面：每个 worker 是一个 KERNEL + SCHED_FIFO 的内核任务，按
   period_ns 周期调用模板的 step()。step 必须：
     - 有界；无分配；无睡眠；无 VFS；不长时间持锁。
   控制面：/dev/rtctl 的 ioctl（kernel/fs/pseudofs/rtctl_dev.c）。

   V1 的周期由 100Hz tick 驱动（task->sleep_expire_ns），所以
   period_ns 必须是 10ms 的整数倍。要支持 1kHz，需要先有 oneshot hrtimer。
*/

#include "rt_worker.h"
#include "rtdev.h"
#include "hrtimer.h"
#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"
#include "driver/time.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"

#define RTW_EINVAL 22
#define RTW_ENOENT 2
#define RTW_EBUSY  16
#define RTW_ENODEV 19
#define RTW_EPERM  1

#define RTW_TICK_NS 10000000ULL /* 当前内核的 100Hz tick */

#define RTW_I64_MAX     ((int64_t)0x7fffffffffffffffLL)
#define RTW_I64_MIN     ((int64_t)(-0x7fffffffffffffffLL - 1))
#define RTW_INPUT_LIMIT ((int64_t)1 << 40)

struct rt_worker;

struct rt_template_ops {
    const char *name;
    uint32_t id;
    void (*init)(struct rt_worker *w);
    uint64_t (*step)(struct rt_worker *w, uint64_t in);
};

struct rtw_pid_state {
    int64_t integ;
    int64_t prev_err;
    bool initialized;
};

struct rt_worker {
    uint32_t id;
    uint8_t template_id;
    uint8_t priority;
    uint32_t cpu;
    uint64_t period_ns;
    uint64_t budget_ns;
    uint64_t params[RTW_PARAM_MAX];

    char name[RTW_NAME_LEN];
    char dev_name[RTW_DEV_NAME_LEN];
    struct rtdev *dev;
    task_t *task;

    volatile uint32_t state;
    volatile bool stop;

    volatile uint64_t input;
    volatile uint64_t output;
    volatile uint64_t out_seq;

    volatile uint64_t ticks;
    volatile uint64_t misses;
    volatile uint64_t overruns;
    volatile uint64_t exec_last_ns;
    volatile uint64_t exec_max_ns;
    volatile uint64_t wake_late_last_ns;
    volatile uint64_t wake_late_max_ns;
    volatile uint64_t dev_writes;

    union {
        struct rtw_pid_state pid;
    } tpl;
};

static struct rt_worker g_workers[RTW_MAX_WORKERS];
static spinlock_t g_rtw_lock = SPINLOCK_INIT;
static bool g_rtw_inited;

static int64_t rtw_clamp_i64(int64_t v, int64_t lo, int64_t hi)
{
    if (lo >= hi)
        return v; /* 没配置限幅 */
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static int64_t rtw_sat_add(int64_t a, int64_t b)
{
    if (b > 0 && a > RTW_I64_MAX - b)
        return RTW_I64_MAX;
    if (b < 0 && a < RTW_I64_MIN - b)
        return RTW_I64_MIN;
    return a + b;
}

static int64_t rtw_input(int64_t v)
{
    return rtw_clamp_i64(v, -RTW_INPUT_LIMIT, RTW_INPUT_LIMIT);
}

static int64_t rtw_gain(uint64_t v)
{
    /* 增益限成 int32，乘上 ±2^40 的输入后不会溢出 int64。 */
    return (int64_t)(int32_t)v;
}

/* ── 模板：periodic_io ────────────────────────────────────── */

static void periodic_io_init(struct rt_worker *w)
{
    (void)w;
}

static uint64_t periodic_io_step(struct rt_worker *w, uint64_t in)
{
    int64_t x = rtw_input((int64_t)in);
    int64_t k = rtw_gain(w->params[0]);
    int64_t y = (x * k) >> 16;
    y = rtw_sat_add(y, (int64_t)w->params[1]);
    y = rtw_clamp_i64(y, (int64_t)w->params[2], (int64_t)w->params[3]);
    return (uint64_t)y;
}

/* ── 模板：pid ────────────────────────────────────────────── */

static void pid_init(struct rt_worker *w)
{
    w->tpl.pid.integ = 0;
    w->tpl.pid.prev_err = 0;
    w->tpl.pid.initialized = false;
}

static uint64_t pid_step(struct rt_worker *w, uint64_t in)
{
    int64_t sp = (int64_t)w->params[5];
    int64_t meas = rtw_input((int64_t)in);
    int64_t err = rtw_input(sp - meas);
    int64_t kp = rtw_gain(w->params[0]);
    int64_t ki = rtw_gain(w->params[1]);
    int64_t kd = rtw_gain(w->params[2]);
    int64_t out_min = (int64_t)w->params[3];
    int64_t out_max = (int64_t)w->params[4];
    int64_t ilim = (int64_t)w->params[6];

    if (ilim <= 0)
        ilim = RTW_INPUT_LIMIT;
    ilim = rtw_clamp_i64(ilim, 0, RTW_INPUT_LIMIT);

    if (!w->tpl.pid.initialized) {
        w->tpl.pid.prev_err = err;
        w->tpl.pid.initialized = true;
    }

    w->tpl.pid.integ = rtw_clamp_i64(rtw_sat_add(w->tpl.pid.integ, err), -ilim, ilim);
    int64_t deriv = err - w->tpl.pid.prev_err;
    w->tpl.pid.prev_err = err;

    int64_t out = ((kp * err) + (ki * w->tpl.pid.integ) + (kd * deriv)) >> 16;
    out = rtw_clamp_i64(out, out_min, out_max);
    return (uint64_t)out;
}

static const struct rt_template_ops g_templates[] = {
    { "periodic_io", RTW_TPL_PERIODIC_IO, periodic_io_init, periodic_io_step },
    { "pid", RTW_TPL_PID, pid_init, pid_step },
};

static const struct rt_template_ops *rtw_template(uint32_t id)
{
    for (uint32_t i = 0; i < sizeof(g_templates) / sizeof(g_templates[0]); i++) {
        if (g_templates[i].id == id)
            return &g_templates[i];
    }
    return NULL;
}

/* ── 周期睡眠（100Hz tick 粒度）────────────────────────────── */

static bool rtw_sleep_until(struct rt_worker *w, uint64_t deadline)
{
    task_t *cur = task_current();

    while (!__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE) && timer_get_ns() < deadline) {
        cur->sleep_expire_ns = deadline;
        task_block(NULL);
    }
    cur->sleep_expire_ns = 0;
    return !__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE);
}

/* ── worker 主循环 ────────────────────────────────────────── */

static void rt_worker_entry(void *arg)
{
    struct rt_worker *w = arg;
    const struct rt_template_ops *tpl = rtw_template(w->template_id);
    uint64_t next = timer_get_ns() + w->period_ns;

    if (!tpl) {
        KLOG_ERROR("[rtw] worker '%s': template %u disappeared\n", w->name, w->template_id);
        __atomic_store_n(&w->state, RTW_STATE_FAULTED, __ATOMIC_RELEASE);
        task_exit();
        return;
    }

    for (;;) {
        if (hrtimer_oneshot_supported())
            hrtimer_sleep_until(next);
        else if (!rtw_sleep_until(w, next))
            break; /* stop */
        if (__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE))
            break;

        uint64_t t0 = timer_get_ns();
        uint64_t late = (t0 > next) ? (t0 - next) : 0;
        __atomic_store_n(&w->wake_late_last_ns, late, __ATOMIC_RELAXED);
        uint64_t prev_late = __atomic_load_n(&w->wake_late_max_ns, __ATOMIC_RELAXED);
        if (late > prev_late)
            __atomic_store_n(&w->wake_late_max_ns, late, __ATOMIC_RELAXED);

        uint64_t in = __atomic_load_n(&w->input, __ATOMIC_RELAXED);
        uint64_t out = tpl->step(w, in);
        __atomic_store_n(&w->output, out, __ATOMIC_RELEASE);
        __atomic_add_fetch(&w->out_seq, 1, __ATOMIC_RELEASE);

        if (w->dev && rtdev_write(w->dev, &out, sizeof(out)) == 0)
            __atomic_add_fetch(&w->dev_writes, 1, __ATOMIC_RELAXED);

        uint64_t t1 = timer_get_ns();
        uint64_t exec = (t1 > t0) ? (t1 - t0) : 0;
        __atomic_store_n(&w->exec_last_ns, exec, __ATOMIC_RELAXED);
        uint64_t prev_max = __atomic_load_n(&w->exec_max_ns, __ATOMIC_RELAXED);
        if (exec > prev_max)
            __atomic_store_n(&w->exec_max_ns, exec, __ATOMIC_RELAXED);
        __atomic_add_fetch(&w->ticks, 1, __ATOMIC_RELAXED);

        if (w->budget_ns != 0 && exec > w->budget_ns)
            __atomic_add_fetch(&w->overruns, 1, __ATOMIC_RELAXED);

        next += w->period_ns;
        uint64_t now = timer_get_ns();
        /*
           oneshot 下唤醒精度是 µs 级，超过半个周期才算 miss；
           退化为 tick 唤醒时，相位可能让一次唤醒晚最多一个 tick。
        */
        uint64_t tol = hrtimer_oneshot_supported() ? (w->period_ns / 2) : RTW_TICK_NS;
        if (now > next + tol) {
            __atomic_add_fetch(&w->misses, 1, __ATOMIC_RELAXED);
            next = now + w->period_ns;
        }
    }

    if (w->dev)
        rtdev_safe_state(w->dev);
    __atomic_store_n(&w->state, RTW_STATE_STOPPED, __ATOMIC_RELEASE);
    task_exit();
}

/* ── 控制面 ───────────────────────────────────────────────── */

static struct rt_worker *rtw_get_locked(uint32_t id)
{
    if (id < 1 || id > RTW_MAX_WORKERS)
        return NULL;
    struct rt_worker *w = &g_workers[id - 1];
    return (w->state == RTW_STATE_FREE) ? NULL : w;
}

static void rtw_copy_name(char *dst, const char *src, size_t dstsz)
{
    size_t i = 0;
    while (i + 1 < dstsz && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void rt_worker_init(void)
{
    if (__atomic_load_n(&g_rtw_inited, __ATOMIC_ACQUIRE))
        return;
    rtdev_register_builtin();
    __atomic_store_n(&g_rtw_inited, true, __ATOMIC_RELEASE);
}

int rt_worker_create(const struct rtw_create_req *cfg)
{
    if (!cfg || cfg->abi_version != RTW_ABI_VERSION)
        return -RTW_EINVAL;
    if (!rtw_template(cfg->template_id))
        return -RTW_EINVAL;
    if (cfg->name[0] == '\0')
        return -RTW_EINVAL;

    if (cfg->priority < TASK_RT_PRIO_MIN || cfg->priority > TASK_RT_PRIO_MAX)
        return -RTW_EINVAL;
    if (cfg->cpu >= (g_num_cpus ? g_num_cpus : 1U))
        return -RTW_EINVAL;
    if (cfg->period_ns < RTW_MIN_PERIOD_NS)
        return -RTW_EINVAL;
    /* 目标核没有 oneshot 时只能按 100Hz tick 唤醒。 */
    if (!hrtimer_cpu_oneshot_supported(cfg->cpu) && (cfg->period_ns % RTW_TICK_NS) != 0) {
        KLOG_WARN("[rtw] cpu%u has no oneshot timer; period must be a multiple of %llu ns\n", cfg->cpu,
                  (unsigned long long)RTW_TICK_NS);
        return -RTW_EINVAL;
    }
    if (cfg->budget_ns > cfg->period_ns)
        return -RTW_EINVAL;

    rt_worker_init();

    uint64_t flags;
    struct rt_worker *w = NULL;
    uint32_t id = 0;

    spin_lock_irqsave(&g_rtw_lock, &flags);
    for (uint32_t i = 0; i < RTW_MAX_WORKERS; i++) {
        if (g_workers[i].state != RTW_STATE_FREE)
            continue;
        w = &g_workers[i];
        id = i + 1;
        memset(w, 0, sizeof(*w));
        w->id = id;
        w->template_id = (uint8_t)cfg->template_id;
        w->priority = cfg->priority;
        w->cpu = cfg->cpu;
        w->period_ns = cfg->period_ns;
        w->budget_ns = cfg->budget_ns;
        for (uint32_t p = 0; p < RTW_PARAM_MAX; p++)
            w->params[p] = cfg->params[p];
        rtw_copy_name(w->name, cfg->name, sizeof(w->name));
        rtw_copy_name(w->dev_name, cfg->dev, sizeof(w->dev_name));
        w->state = RTW_STATE_CREATED;
        rtw_template(w->template_id)->init(w);
        break;
    }
    spin_unlock_irqrestore(&g_rtw_lock, flags);

    if (!w)
        return -RTW_EBUSY;

    if (w->dev_name[0] != '\0') {
        struct rtdev *dev = rtdev_claim(w->dev_name, id);
        if (!dev) {
            spin_lock_irqsave(&g_rtw_lock, &flags);
            memset(w, 0, sizeof(*w));
            spin_unlock_irqrestore(&g_rtw_lock, flags);
            return -RTW_ENODEV;
        }
        w->dev = dev;
    }

    KLOG_INFO("[rtw] created '%s' id=%u tpl=%u prio=%u cpu=%u period=%llu ns\n", w->name, w->id, w->template_id,
              w->priority, w->cpu, (unsigned long long)w->period_ns);
    return (int)id;
}

int rt_worker_start(uint32_t id)
{
    uint64_t flags;
    spin_lock_irqsave(&g_rtw_lock, &flags);
    struct rt_worker *w = rtw_get_locked(id);
    if (!w) {
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_ENOENT;
    }
    if (w->state != RTW_STATE_CREATED && w->state != RTW_STATE_STOPPED) {
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_EBUSY;
    }
    __atomic_store_n(&w->stop, false, __ATOMIC_RELEASE);
    w->state = RTW_STATE_RUNNING;
    spin_unlock_irqrestore(&g_rtw_lock, flags);

    /*
       task_create_kernel_rt 可能触发 self-IPI / 调度，不能在 g_rtw_lock
       下调用。state=RUNNING 已经占住，别的 START 会看到 EBUSY。
    */
    task_t *t = task_create_kernel_rt(w->name, rt_worker_entry, w, w->priority, w->cpu);
    if (!t) {
        spin_lock_irqsave(&g_rtw_lock, &flags);
        w->state = RTW_STATE_CREATED;
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_EBUSY;
    }

    spin_lock_irqsave(&g_rtw_lock, &flags);
    w->task = t;
    spin_unlock_irqrestore(&g_rtw_lock, flags);
    return 0;
}

int rt_worker_stop(uint32_t id)
{
    uint64_t flags;
    spin_lock_irqsave(&g_rtw_lock, &flags);
    struct rt_worker *w = rtw_get_locked(id);
    if (!w) {
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_ENOENT;
    }
    if (w->state != RTW_STATE_RUNNING) {
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_EINVAL;
    }
    __atomic_store_n(&w->stop, true, __ATOMIC_RELEASE);
    task_t *t = w->task;
    spin_unlock_irqrestore(&g_rtw_lock, flags);

    /*
       如果 worker 正睡在 tick 上，清掉 sleep_expire 并唤醒；它醒来会在
       rtw_sleep_until() 里先检查 stop，然后退出。任务正在 step 里跑的话
       不需要额外动作 —— 本轮结束就会看到 stop。
    */
    if (t && t->state == TASK_BLOCKED) {
        t->sleep_expire_ns = 0;
        task_unblock(t);
    }
    return 0;
}

int rt_worker_destroy(uint32_t id)
{
    uint64_t flags;
    struct rtdev *dev = NULL;
    spin_lock_irqsave(&g_rtw_lock, &flags);
    struct rt_worker *w = rtw_get_locked(id);
    if (!w) {
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_ENOENT;
    }
    if (w->state == RTW_STATE_RUNNING) {
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_EBUSY;
    }
    dev = w->dev;
    memset(w, 0, sizeof(*w));
    spin_unlock_irqrestore(&g_rtw_lock, flags);

    if (dev)
        rtdev_release(dev, id);
    return 0;
}

int rt_worker_set_input(uint32_t id, uint64_t value)
{
    uint64_t flags;
    spin_lock_irqsave(&g_rtw_lock, &flags);
    struct rt_worker *w = rtw_get_locked(id);
    if (!w) {
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_ENOENT;
    }
    __atomic_store_n(&w->input, value, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_rtw_lock, flags);
    return 0;
}

int rt_worker_get_output(uint32_t id, uint64_t *value, uint64_t *seq)
{
    uint64_t flags;
    spin_lock_irqsave(&g_rtw_lock, &flags);
    struct rt_worker *w = rtw_get_locked(id);
    if (!w) {
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_ENOENT;
    }
    if (value)
        *value = __atomic_load_n(&w->output, __ATOMIC_ACQUIRE);
    if (seq)
        *seq = __atomic_load_n(&w->out_seq, __ATOMIC_ACQUIRE);
    spin_unlock_irqrestore(&g_rtw_lock, flags);
    return 0;
}

int rt_worker_get_stats(uint32_t id, struct rtw_stats *out)
{
    if (!out)
        return -RTW_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_rtw_lock, &flags);
    struct rt_worker *w = rtw_get_locked(id);
    if (!w) {
        spin_unlock_irqrestore(&g_rtw_lock, flags);
        return -RTW_ENOENT;
    }
    memset(out, 0, sizeof(*out));
    out->id = w->id;
    out->state = __atomic_load_n(&w->state, __ATOMIC_ACQUIRE);
    out->template_id = w->template_id;
    out->priority = w->priority;
    out->cpu = w->cpu;
    out->period_ns = w->period_ns;
    out->budget_ns = w->budget_ns;
    out->ticks = __atomic_load_n(&w->ticks, __ATOMIC_RELAXED);
    out->misses = __atomic_load_n(&w->misses, __ATOMIC_RELAXED);
    out->overruns = __atomic_load_n(&w->overruns, __ATOMIC_RELAXED);
    out->exec_last_ns = __atomic_load_n(&w->exec_last_ns, __ATOMIC_RELAXED);
    out->exec_max_ns = __atomic_load_n(&w->exec_max_ns, __ATOMIC_RELAXED);
    out->wake_late_last_ns = __atomic_load_n(&w->wake_late_last_ns, __ATOMIC_RELAXED);
    out->wake_late_max_ns = __atomic_load_n(&w->wake_late_max_ns, __ATOMIC_RELAXED);
    out->last_input = __atomic_load_n(&w->input, __ATOMIC_RELAXED);
    out->last_output = __atomic_load_n(&w->output, __ATOMIC_RELAXED);
    out->out_seq = __atomic_load_n(&w->out_seq, __ATOMIC_RELAXED);
    out->dev_writes = __atomic_load_n(&w->dev_writes, __ATOMIC_RELAXED);
    rtw_copy_name(out->name, w->name, sizeof(out->name));
    rtw_copy_name(out->dev, w->dev_name, sizeof(out->dev));
    spin_unlock_irqrestore(&g_rtw_lock, flags);
    return 0;
}

int rt_worker_list(struct rtw_list_req *list)
{
    if (!list)
        return -RTW_EINVAL;

    uint64_t flags;
    uint32_t n = 0;
    uint32_t cap = list->capacity;
    if (cap > RTW_MAX_WORKERS)
        cap = RTW_MAX_WORKERS;

    spin_lock_irqsave(&g_rtw_lock, &flags);
    for (uint32_t i = 0; i < RTW_MAX_WORKERS && n < cap; i++) {
        struct rt_worker *w = &g_workers[i];
        if (w->state == RTW_STATE_FREE)
            continue;
        struct rtw_list_entry *e = &list->entries[n];
        memset(e, 0, sizeof(*e));
        e->id = w->id;
        e->state = __atomic_load_n(&w->state, __ATOMIC_ACQUIRE);
        e->template_id = w->template_id;
        e->priority = w->priority;
        e->cpu = w->cpu;
        e->period_ns = w->period_ns;
        e->ticks = __atomic_load_n(&w->ticks, __ATOMIC_RELAXED);
        e->misses = __atomic_load_n(&w->misses, __ATOMIC_RELAXED);
        rtw_copy_name(e->name, w->name, sizeof(e->name));
        n++;
    }
    spin_unlock_irqrestore(&g_rtw_lock, flags);
    list->count = n;
    return 0;
}
