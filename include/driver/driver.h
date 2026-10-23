/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/driver.h — 轻量驱动框架核心

   设计见 docs/driver/DRIVER_FRAMEWORK.md。

   三条约定：
     1. 驱动用 DRIVER_ENTRY() 自注册到 .drivers 只读段；
     2. 平台设备由 platform.conf 生成 g_platform_devices[]；
     3. 一次构建每个 class 只有一个后端，绑定只按 class。

   这里故意不实现 bus/probe 匹配、热插拔、deferred probe。驱动是静态的，
   初始化顺序由 6 个 phase 表达。
*/

#ifndef DRIVER_DRIVER_H
#define DRIVER_DRIVER_H

#include "types.h"

typedef enum {
    DRV_SERIAL = 0, /* 控制台串口 */
    DRV_IRQ,        /* 中断控制器 */
    DRV_TIME,       /* 时间基准 */
    DRV_STORAGE,    /* 根块设备 */
    DRV_NET,        /* 网卡 */
    DRV_DMA,        /* service：dma-buf / dma-heap，无设备 */
    DRV_ION,        /* service：ION，无设备 */
    DRV_NPU,        /* NPU */
    DRV_TPU,        /* TPU */
    DRV_CLASS_COUNT
} drv_class_t;

typedef enum {
    DRV_PHASE_EARLY = 0, /* 串口：先于 klog */
    DRV_PHASE_IRQ,       /* 中断控制器；串口切中断模式 */
    DRV_PHASE_TIME,      /* 时间基准 */
    DRV_PHASE_STORAGE,   /* 根块设备：先于 fs_init */
    DRV_PHASE_CORE,      /* DMA/ION/NPU/TPU */
    DRV_PHASE_NET,       /* 调度器就绪之后 */
    DRV_PHASE_COUNT
} drv_phase_t;

typedef struct device {
    const char *name; /* "uart0" —— 仅用于日志/诊断 */
    drv_class_t cls;
    const void *cfg; /* 指向该 class 的 cfg 结构 */
} device_t;

typedef struct driver {
    const char *name; /* "pl011" —— 仅用于日志/诊断 */
    drv_class_t cls;
    drv_phase_t phase;       /* probe 阶段 */
    drv_phase_t start_phase; /* start 阶段；DRV_PHASE_COUNT 表示没有 */
    int (*probe)(const device_t *dev);
    int (*start)(const device_t *dev);
} driver_t;

/*
   把驱动描述符放进 .drivers 段。三个 link.ld 在 .rodata 内收集该段，
   因此 __start_drivers/__stop_drivers 可直接遍历。

   用法：
     DRIVER_ENTRY(driver_pl011) = {
         .name = "pl011", .cls = DRV_SERIAL,
         .phase = DRV_PHASE_EARLY, .start_phase = DRV_PHASE_COUNT,
         .probe = pl011_probe,
     };
*/
#define DRIVER_ENTRY(sym) const driver_t sym __attribute__((used, section(".drivers"), aligned(8)))

/*
   drivers_init — 执行某个阶段

   同一阶段内分两遍：先所有 probe，再所有 start。这样 UART 的
   "早期 probe / 中断模式 start" 和 GIC 的 "probe" 可以同处 IRQ 阶段
   而不会因注册顺序打架。
*/
void drivers_init(drv_phase_t phase);

/* 该 class 是否已有驱动 probe 成功 */
bool driver_present(drv_class_t cls);

/* 按 class 取平台设备；没有返回 NULL */
const device_t *driver_device(drv_class_t cls);

/*
   device_mmio — 把 platform.conf 里的原始物理地址换算成内核可访问地址
   （按平台的 mmio_vma 规则）。驱动应统一走这个函数，不要自己加 KERNEL_VMA。
*/
uintptr_t device_mmio(uintptr_t raw_pa);

/* 由 gen_platform.py 生成到 build/<platform>/platform_static.c */
extern const device_t g_platform_devices[];
extern const unsigned g_platform_device_count;

#endif /* DRIVER_DRIVER_H */
