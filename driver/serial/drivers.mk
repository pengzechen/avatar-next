# SPDX-License-Identifier: MIT
#
# 串口驱动选择。UART= 保留为对外命令（脚本/文档已在用）。

UART ?= $(DEV_UART_TYPE)

ifeq ($(UART),pl011)
    DRIVER_SRCS_CFLAGS += driver/uart/uart_pl011.c
    ifneq ($(ARCH),aarch64)
        $(error UART=pl011 is only valid on aarch64)
    endif
else ifeq ($(UART),dw)
    DRIVER_SRCS_CFLAGS += driver/uart/uart_dw.c
else ifeq ($(UART),x86)
    DRIVER_SRCS_CFLAGS += driver/uart/uart_x86.c
    ifneq ($(ARCH),x86_64)
        $(error UART=x86 is only valid on x86_64)
    endif
else ifneq ($(UART),)
    $(error Invalid UART. Use: pl011, dw or x86)
endif
