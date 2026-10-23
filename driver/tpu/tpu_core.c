/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/tpu/tpu_core.c — TPU 服务注册点

   永远参与构建。TPU=cvitpu 时 driver/tpu/cvi_tpu.c 的 probe 调 tpu_bind()；
   否则 g_tpu_ops 保持 NULL，include/driver/tpu.h 的内联包装统一返回失败。
*/

#include "driver/tpu.h"

const tpu_ops_t *g_tpu_ops;

void tpu_bind(const tpu_ops_t *ops)
{
    g_tpu_ops = ops;
}
