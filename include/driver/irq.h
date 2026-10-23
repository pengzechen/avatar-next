/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/irq.h — 中断控制器的平台配置

   阶段 1 只提供 cfg；统一的 irq_ops_t（ack/eoi/register/enable/ipi）与
   irq_virt_ops_t 在阶段 2 落地，届时三套 arch handler 表收敛到这里。
*/

#ifndef DRIVER_IRQ_H
#define DRIVER_IRQ_H

#include "types.h"

typedef struct {
    uintptr_t gicd;
    uintptr_t gicr;
    uintptr_t gicc;
    uintptr_t gich;
    uintptr_t plic;
    uintptr_t clint;
    bool virt; /* 是否提供虚拟中断接口（GICv3） */
} irq_cfg_t;

#endif /* DRIVER_IRQ_H */
