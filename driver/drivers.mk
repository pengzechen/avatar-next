# SPDX-License-Identifier: MIT
#
# driver/drivers.mk — 驱动选择汇总
#
# 每个 class 一个片段，片段只负责：
#   1. 把选中的源文件追加到对应的 DRIVER_SRCS_* 组；
#   2. 加上该驱动需要的 -D（阶段 1 仍被现有 #if 使用）。
#
# 顶层 Makefile 负责把源文件映射成 build/<platform>/driver/<basename>.o。
# 命令行覆盖（UART= / GIC= / IRQ= / ETH= / NPU= / TPU= / DMA= / ION= / BLOCK=）
# 仍然有效。

NPU ?= $(DEV_NPU_TYPE)
TPU ?= $(DEV_TPU_TYPE)
DMA ?= $(if $(filter rknpu,$(NPU)),1,0)
ION ?= $(if $(filter cvitpu,$(TPU)),1,0)

include driver/serial/drivers.mk
include driver/irq/drivers.mk
include driver/time/drivers.mk
include driver/storage/drivers.mk
include driver/net/drivers.mk
include driver/dma/drivers.mk
include driver/ion/drivers.mk
include driver/npu/drivers.mk
include driver/tpu/drivers.mk
