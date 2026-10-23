# SPDX-License-Identifier: MIT
#
# dma-buf / dma-heap 服务。默认随 NPU=rknpu 启用。

ifeq ($(DMA),1)
    DRIVER_SRCS_DMA += driver/dma/dma_buf.c driver/dma/dma_heap.c
    CFLAGS           += -DDRIVER_DMA_BUF=1
endif
