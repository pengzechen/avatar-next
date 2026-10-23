# SPDX-License-Identifier: MIT
#
# 根块设备选择：ramblk（QEMU/RK3588 内存盘）或 sdblk（SG2002 SD 卡）。

BLOCK ?= $(DEV_BLOCK_TYPE)

ifeq ($(BLOCK),ramblk)
    DRIVER_SRCS_BLK += driver/blk/ramblk.c
else ifeq ($(BLOCK),sdblk)
    DRIVER_SRCS_BLK += driver/blk/sdblk.c
    CFLAGS           += -DDRIVER_SDBLK_SG2002=1
    ifneq ($(ARCH),riscv64)
        $(error BLOCK=sdblk is only valid on riscv64)
    endif
else ifneq ($(BLOCK),)
    $(error Invalid BLOCK driver: $(BLOCK). Use: ramblk or sdblk)
endif
