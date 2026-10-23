/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/ion/ion_core.c — ION 服务注册点

   永远参与构建。ION=1 时 driver/ion/ion.c 的 probe 调 ion_bind()；
   否则 g_ion_ops 保持 NULL，include/driver/ion.h 的内联包装统一返回失败。
*/

#include "driver/ion.h"

const ion_ops_t *g_ion_ops;

void ion_bind(const ion_ops_t *ops)
{
    g_ion_ops = ops;
}
