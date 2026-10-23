/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/serial.h — 串口设备的平台配置

   阶段 1 只管理生命周期与配置；串口数据接口（serial_ops_t）在阶段 2
   移到本头文件，现有 uart_* 调用点届时统一切换。
*/

#ifndef DRIVER_SERIAL_H
#define DRIVER_SERIAL_H

#include "types.h"

typedef struct {
    uintptr_t base;         /* 原始物理地址；驱动用 device_mmio() 换算 */
    uint32_t reg_shift;     /* 0 = 字节寻址；2 = 4 字节步长 */
    uint32_t irq;           /* 0 = 暂不接中断 */
    bool irq_mode;          /* true：在 DRV_PHASE_IRQ 的 start 里切中断模式 */
    const char *compatible; /* 可选 sanity check，如 "arm,pl011" */
} serial_cfg_t;

#endif /* DRIVER_SERIAL_H */
