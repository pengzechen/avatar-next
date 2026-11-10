/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/rt/rtdev.c — RT 模板可用的设备契约与注册表

   只有满足 rtdev_ops 约束的驱动才注册到这里。详见 include/rtdev.h。
   当前内置一个示例设备 "vout"：把每次写当成 u64 记录下来，用于验证
   “worker → rtdev → safe_state” 整条路径；真实 GPIO/PWM/UART 驱动按同一
   契约注册即可。
*/

#include "rtdev.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"

#define RTDEV_MAX 8

#define RTDEV_EINVAL 22
#define RTDEV_ENODEV 19
#define RTDEV_ENOSPC 28

static struct rtdev g_rtdevs[RTDEV_MAX];
static spinlock_t g_rtdev_lock = SPINLOCK_INIT;
static bool g_builtin_registered;

static struct rtdev *rtdev_find_locked(const char *name)
{
    for (uint32_t i = 0; i < RTDEV_MAX; i++) {
        if (g_rtdevs[i].ops && strcmp(g_rtdevs[i].name, name) == 0)
            return &g_rtdevs[i];
    }
    return NULL;
}

int rtdev_register(const char *name, const struct rtdev_ops *ops, void *ctx)
{
    if (!name || !ops || name[0] == '\0')
        return -RTDEV_EINVAL;
    if (strlen(name) >= RTDEV_NAME_LEN)
        return -RTDEV_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_rtdev_lock, &flags);
    if (rtdev_find_locked(name)) {
        spin_unlock_irqrestore(&g_rtdev_lock, flags);
        return -RTDEV_EINVAL;
    }
    for (uint32_t i = 0; i < RTDEV_MAX; i++) {
        if (g_rtdevs[i].ops)
            continue;
        memset(&g_rtdevs[i], 0, sizeof(g_rtdevs[i]));
        strncpy(g_rtdevs[i].name, name, RTDEV_NAME_LEN - 1);
        g_rtdevs[i].name[RTDEV_NAME_LEN - 1] = '\0';
        g_rtdevs[i].ops = ops;
        g_rtdevs[i].ctx = ctx;
        g_rtdevs[i].owner = 0;
        spin_unlock_irqrestore(&g_rtdev_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&g_rtdev_lock, flags);
    return -RTDEV_ENOSPC;
}

struct rtdev *rtdev_claim(const char *name, uint32_t owner)
{
    if (!name || name[0] == '\0')
        return NULL;

    uint64_t flags;
    spin_lock_irqsave(&g_rtdev_lock, &flags);
    struct rtdev *d = rtdev_find_locked(name);
    if (!d || d->owner != 0) {
        spin_unlock_irqrestore(&g_rtdev_lock, flags);
        return NULL;
    }
    d->owner = owner;
    spin_unlock_irqrestore(&g_rtdev_lock, flags);

    /*
       setup 允许睡眠/分配，必须在锁外调用。owner 已经占住，别的 worker
       不可能在 setup 期间抢走。
    */
    if (d->ops->setup && d->ops->setup(d, NULL, 0) != 0) {
        spin_lock_irqsave(&g_rtdev_lock, &flags);
        if (d->owner == owner)
            d->owner = 0;
        spin_unlock_irqrestore(&g_rtdev_lock, flags);
        return NULL;
    }
    return d;
}

void rtdev_release(struct rtdev *d, uint32_t owner)
{
    if (!d)
        return;

    /* 先让设备回安全态（RT-safe，不睡眠），再释放所有权。 */
    if (d->ops && d->ops->safe_state)
        d->ops->safe_state(d);
    if (d->ops && d->ops->teardown)
        d->ops->teardown(d);

    uint64_t flags;
    spin_lock_irqsave(&g_rtdev_lock, &flags);
    if (d->owner == owner)
        d->owner = 0;
    spin_unlock_irqrestore(&g_rtdev_lock, flags);
}

int rtdev_write(struct rtdev *d, const void *buf, uint32_t len)
{
    if (!d || !d->ops || !d->ops->write)
        return -RTDEV_ENODEV;
    return d->ops->write(d, buf, len);
}

int rtdev_read(struct rtdev *d, void *buf, uint32_t len)
{
    if (!d || !d->ops || !d->ops->read)
        return -RTDEV_ENODEV;
    return d->ops->read(d, buf, len);
}

void rtdev_safe_state(struct rtdev *d)
{
    if (d && d->ops && d->ops->safe_state)
        d->ops->safe_state(d);
}

/* ── 内置示例设备：vout ───────────────────────────────────── */

struct vout_ctx {
    volatile uint64_t last;
    volatile uint64_t writes;
};

static struct vout_ctx g_vout_ctx;

static int vout_setup(struct rtdev *d, const void *cfg, uint32_t cfg_len)
{
    (void)d;
    (void)cfg;
    (void)cfg_len;
    g_vout_ctx.last = 0;
    g_vout_ctx.writes = 0;
    return 0;
}

static int vout_write(struct rtdev *d, const void *buf, uint32_t len)
{
    if (!buf || len < sizeof(uint64_t))
        return -RTDEV_EINVAL;
    uint64_t v;
    memcpy(&v, buf, sizeof(v));
    struct vout_ctx *c = d->ctx;
    c->last = v;
    c->writes = c->writes + 1;
    return 0;
}

static int vout_read(struct rtdev *d, void *buf, uint32_t len)
{
    if (!buf || len < sizeof(uint64_t))
        return -RTDEV_EINVAL;
    struct vout_ctx *c = d->ctx;
    memcpy(buf, (const void *)&c->last, sizeof(uint64_t));
    return 0;
}

static void vout_safe_state(struct rtdev *d)
{
    struct vout_ctx *c = d->ctx;
    c->last = 0;
}

static void vout_teardown(struct rtdev *d)
{
    struct vout_ctx *c = d->ctx;
    KLOG_INFO("[rtdev] vout: %llu writes, last=%llu\n", (unsigned long long)c->writes, (unsigned long long)c->last);
}

static const struct rtdev_ops g_vout_ops = {
    .name = "vout",
    .setup = vout_setup,
    .write = vout_write,
    .read = vout_read,
    .safe_state = vout_safe_state,
    .teardown = vout_teardown,
};

void rtdev_register_builtin(void)
{
    if (g_builtin_registered)
        return;
    if (rtdev_register("vout", &g_vout_ops, &g_vout_ctx) == 0)
        g_builtin_registered = true;
}
