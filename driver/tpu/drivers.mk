# SPDX-License-Identifier: MIT
#
# TPU 驱动选择。

ifeq ($(TPU),cvitpu)
    ifneq ($(ARCH),riscv64)
        $(error TPU=cvitpu is only supported on riscv64)
    endif
    DRIVER_SRCS_TPU += driver/tpu/cvi_tpu.c
    CFLAGS          += -DDRIVER_TPU_CVITPU=1
else ifeq ($(TPU),)
else
    $(error Invalid TPU driver: $(TPU). Use: cvitpu or empty)
endif
