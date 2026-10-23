/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/tpu.h — TPU 的平台配置
*/

#ifndef DRIVER_TPU_H
#define DRIVER_TPU_H

#include "types.h"

typedef struct {
    uintptr_t tdma_base;
    uintptr_t tiu_base;
    uint32_t irq;
} tpu_cfg_t;

#endif /* DRIVER_TPU_H */
