/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/npu.h — NPU 的平台配置

   阶段 1 只提供 cfg；NPU 专属 API（DRM UAPI、宿主让出/收回）仍在
   include/rknpu_drm.h / driver/npu/rknpu.h，阶段 2 再收敛能力接口。
*/

#ifndef DRIVER_NPU_H
#define DRIVER_NPU_H

#include "types.h"

typedef struct {
    uintptr_t base[3]; /* NPU core0/1/2 原始物理基址 */
    uint32_t irq[3];
    uintptr_t pmu; /* PMU1 基址 */
} npu_cfg_t;

#endif /* DRIVER_NPU_H */
