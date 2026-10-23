# SPDX-License-Identifier: MIT
#
# 时间基准驱动选择。当前三个架构共用 driver/timer/timer.c，
# 具体实现由 timer.c 按 ARCH 选择。

TIMER ?= $(DEV_TIMER_TYPE)

ifneq ($(TIMER),)
    ifeq ($(filter $(TIMER),aarch64 rv x86),)
        $(error Invalid TIMER driver: $(TIMER))
    endif
    DRIVER_SRCS_CFLAGS += driver/timer/timer.c
endif
