/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/time.h — 时间基准的平台配置

   阶段 1 只提供 cfg；公共时间 API 目前仍是 driver/timer/timer.h（阶段 2
   迁移到本头文件，函数名保持不变）。
*/

#ifndef DRIVER_TIME_H
#define DRIVER_TIME_H

#include "types.h"

typedef struct {
    uint32_t freq_hz;    /* 目标 tick 频率 */
    uint32_t tick_ms;    /* tick 周期 */
    uint32_t cntp;       /* AArch64 CNTP PPI 号（其它架构为 0） */
    uint64_t counter_hz; /* 硬件计数器频率 */
} time_cfg_t;

#endif /* DRIVER_TIME_H */
