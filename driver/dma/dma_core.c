/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/dma/dma_core.c — dma-buf 服务注册点

   永远参与构建。DMA=1 时 driver/dma/dma_buf.c 的 probe 调 dma_buf_bind()；
   否则 g_dma_buf_ops 保持 NULL，include/dma_buf.h 的内联包装统一返回失败。
*/

#include "dma_buf.h"

const dma_buf_ops_t *g_dma_buf_ops;

void dma_buf_bind(const dma_buf_ops_t *ops)
{
    g_dma_buf_ops = ops;
}
