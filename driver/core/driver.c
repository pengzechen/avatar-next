/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/core/driver.c — 驱动注册表与初始化阶段

   驱动通过 DRIVER_ENTRY() 放进 .drivers 段，链接脚本把它收在 .rodata 内，
   于是这里可以直接用 __start_drivers/__stop_drivers 遍历。

   一次构建每个 class 只有一个后端，所以绑定只按 class；driver 名字仅用于
   日志/诊断。命令行覆盖（make UART=dw 等）只改变被编译的后端，不会和
   platform.conf 里的设备表失配。
*/

#include "driver/driver.h"
#include "klog.h"
#include "platform_ops.h" /* platform_panic() */

extern const driver_t __start_drivers[];
extern const driver_t __stop_drivers[];

static const driver_t *g_bound[DRV_CLASS_COUNT];

/* 没有设备、但需要初始化的服务 */
static bool driver_is_service(drv_class_t cls)
{
    return cls == DRV_DMA || cls == DRV_ION;
}

/* 有设备就必须有驱动的 class */
static bool driver_is_required(drv_class_t cls)
{
    return cls == DRV_SERIAL || cls == DRV_IRQ || cls == DRV_TIME || cls == DRV_STORAGE;
}

/*
   每个必需 class 的 probe 阶段。设备表里出现了该类设备，但构建里没有对应
   驱动时，在这个阶段直接 panic，而不是带着半初始化状态继续启动。
*/
static const drv_phase_t g_required_phase[DRV_CLASS_COUNT] = {
    [DRV_SERIAL] = DRV_PHASE_EARLY,
    [DRV_IRQ] = DRV_PHASE_IRQ,
    [DRV_TIME] = DRV_PHASE_TIME,
    [DRV_STORAGE] = DRV_PHASE_STORAGE,
};

static const driver_t *driver_of_class(drv_class_t cls)
{
    for (const driver_t *d = __start_drivers; d < __stop_drivers; d++) {
        if (d->cls == cls)
            return d;
    }
    return NULL;
}

static void driver_required_panic(const driver_t *d, const char *what, int rc)
{
    if (rc)
        KLOG_ERROR("[drv] %s: %s failed (%d)\n", d->name, what, rc);
    else
        KLOG_ERROR("[drv] %s: %s failed\n", d->name, what);
    platform_panic();
}

void drivers_init(drv_phase_t phase)
{
    if ((unsigned)phase >= DRV_PHASE_COUNT)
        return;

    /*
       第一次进入（EARLY）时把"平台声明了设备、但没有驱动"的情况喊出来。
       必需类在下面按阶段 panic；可选类只告警（例如 ETH=none 但设备表
       仍然写了 virtio 时，能一眼看到配置不一致）。
    */
    if (phase == DRV_PHASE_EARLY) {
        for (unsigned i = 0; i < g_platform_device_count; i++) {
            const device_t *dev = &g_platform_devices[i];
            if (!driver_of_class(dev->cls))
                KLOG_WARN("[drv] device '%s' has no driver in this build\n", dev->name);
        }
    }

    /* 必需设备：在它的 probe 阶段检查驱动是否存在 */
    for (unsigned i = 0; i < g_platform_device_count; i++) {
        const device_t *dev = &g_platform_devices[i];
        if ((unsigned)dev->cls >= DRV_CLASS_COUNT)
            continue;
        if (g_required_phase[dev->cls] != phase)
            continue;
        if (!driver_of_class(dev->cls)) {
            KLOG_ERROR("[drv] required device '%s' (class %d) has no driver\n", dev->name, (int)dev->cls);
            platform_panic();
        }
    }

    /* 第一遍：probe */
    for (const driver_t *d = __start_drivers; d < __stop_drivers; d++) {
        if (d->phase != phase)
            continue;

        const device_t *dev = driver_device(d->cls);
        if (!dev && !driver_is_service(d->cls)) {
            KLOG_WARN("[drv] %s: no matching device, skipped\n", d->name);
            continue;
        }

        int rc = d->probe ? d->probe(dev) : 0;
        if (rc != 0) {
            if (driver_is_required(d->cls))
                driver_required_panic(d, "probe", rc);
            KLOG_ERROR("[drv] %s: probe failed (%d), skipped\n", d->name, rc);
            continue;
        }

        g_bound[d->cls] = d;
        KLOG_INIT("[drv] %s ready (%s)\n", d->name, dev ? dev->name : "service");
    }

    /* 第二遍：start（保证同阶段内先 probe 后 start） */
    for (const driver_t *d = __start_drivers; d < __stop_drivers; d++) {
        if (d->start_phase != phase || !d->start)
            continue;

        int rc = d->start(driver_device(d->cls));
        if (rc != 0) {
            if (driver_is_required(d->cls))
                driver_required_panic(d, "start", rc);
            KLOG_ERROR("[drv] %s: start failed (%d)\n", d->name, rc);
        }
    }
}

bool driver_present(drv_class_t cls)
{
    return (unsigned)cls < DRV_CLASS_COUNT && g_bound[cls] != NULL;
}
