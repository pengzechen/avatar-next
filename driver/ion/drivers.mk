# SPDX-License-Identifier: MIT
#
# ION 分配器。默认随 TPU=cvitpu 启用。

ifeq ($(ION),1)
    DRIVER_SRCS_ION += driver/ion/ion.c
    CFLAGS           += -DDRIVER_ION=1
endif
