/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/core/device.c — 平台设备表访问

   g_platform_devices[] 由 tools/gen_platform.py 生成到
   build/<platform>/platform_static.c。这里只做按 class 查找和 MMIO 地址换算。
*/

#include "driver/driver.h"
#include "platform_cfg.h"
#include "mm_vm.h" /* KERNEL_VMA */

const device_t *driver_device(drv_class_t cls)
{
    if ((unsigned)cls >= DRV_CLASS_COUNT)
        return NULL;

    for (unsigned i = 0; i < g_platform_device_count; i++) {
        if (g_platform_devices[i].cls == cls)
            return &g_platform_devices[i];
    }
    return NULL;
}

uintptr_t device_mmio(uintptr_t raw_pa)
{
    if (raw_pa == 0)
        return 0;
    return g_mmio_needs_vma ? raw_pa + KERNEL_VMA : raw_pa;
}
