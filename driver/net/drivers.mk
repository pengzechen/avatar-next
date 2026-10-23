# SPDX-License-Identifier: MIT
#
# 网卡驱动选择。

ETH ?= $(DEV_ETH_TYPE)

ifeq ($(ETH),virtio)
    ifeq ($(filter $(ARCH),riscv64 aarch64 x86_64),)
        $(error ETH=virtio currently supports ARCH=riscv64, ARCH=aarch64 or ARCH=x86_64)
    endif
    DRIVER_SRCS_CFLAGS += driver/eth/virtio_net.c
    CFLAGS             += -DDRIVER_ETH_VIRTIO=1
else ifeq ($(ETH),cvitek)
    ifeq ($(ARCH),riscv64)
        DRIVER_SRCS_CFLAGS += driver/eth/cvitek_eth.c
        CFLAGS             += -DDRIVER_ETH_CVITEK=1
    else
        $(error ETH=cvitek is only supported on ARCH=riscv64)
    endif
else ifeq ($(ETH),none)
else ifeq ($(ETH),)
else
    $(error Invalid ETH. Use: none, virtio or cvitek)
endif
