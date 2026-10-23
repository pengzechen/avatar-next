# SPDX-License-Identifier: MIT
#
# NPU 驱动选择。rknpu 依赖 dma-buf（librknnrt 的缓冲区全部来自 dma_heap）。

ifeq ($(NPU),rknpu)
    ifeq ($(DMA),0)
        $(error NPU=rknpu requires DMA=1 (dma-buf/dma-heap))
    endif
    ifneq ($(ARCH),aarch64)
        $(error NPU=rknpu is only supported on aarch64)
    endif
    DRIVER_SRCS_CFLAGS += driver/npu/rknpu.c driver/npu/rkpm.c \
                          driver/npu/rknpu_drm.c driver/npu/npu_mmu.c
    CFLAGS             += -DDRIVER_NPU_RKNPU=1
else ifeq ($(NPU),)
else
    $(error Invalid NPU driver: $(NPU). Use: rknpu or empty)
endif
