/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/npu/npu_core.c — NPU 服务注册点

   永远参与构建。NPU=rknpu 时 driver/npu/rknpu.c 的 probe 调 npu_bind()；
   否则 g_npu_ops 保持 NULL，include/driver/npu.h 的内联包装统一返回
   失败/空操作。
*/

#include "driver/npu.h"

const npu_ops_t *g_npu_ops;

void npu_bind(const npu_ops_t *ops)
{
    g_npu_ops = ops;
}
