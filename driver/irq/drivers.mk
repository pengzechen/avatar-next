# SPDX-License-Identifier: MIT
#
# 中断控制器选择。
#
# 对外仍兼容 GIC=v2|v3|none；内部统一成 IRQ=gicv2|gicv3|plic|lapic|none，
# 默认来自 platform.conf 的 irq.driver。PLIC/LAPIC 不再按 ARCH/布尔推断。

ifdef GIC
    ifeq ($(GIC),v2)
        IRQ := gicv2
    else ifeq ($(GIC),v3)
        IRQ := gicv3
    else ifeq ($(GIC),none)
        IRQ := none
    else
        $(error Invalid GIC. Use: v2, v3 or none)
    endif
endif
IRQ ?= $(DEV_IRQ_TYPE)

ifeq ($(IRQ),gicv3)
    DRIVER_SRCS_CFLAGS += driver/irq/gicv3.c
    CFLAGS             += -DDRIVER_GIC_V3=1
    ifneq ($(ARCH),aarch64)
        $(error IRQ=gicv3 is only valid on aarch64)
    endif
else ifeq ($(IRQ),gicv2)
    DRIVER_SRCS_CFLAGS += driver/irq/gicv2.c
    CFLAGS             += -DDRIVER_GIC_V2=1
    ifneq ($(ARCH),aarch64)
        $(error IRQ=gicv2 is only valid on aarch64)
    endif
else ifeq ($(IRQ),plic)
    DRIVER_SRCS_PLIC += driver/irq/plic.c
    ifneq ($(ARCH),riscv64)
        $(error IRQ=plic is only valid on riscv64)
    endif
else ifeq ($(IRQ),lapic)
    DRIVER_SRCS_CFLAGS += driver/irq/lapic.c
    ifneq ($(ARCH),x86_64)
        $(error IRQ=lapic is only valid on x86_64)
    endif
else ifeq ($(IRQ),none)
    # 无中断控制器：timer/驱动只能轮询
else ifneq ($(IRQ),)
    $(error Invalid IRQ. Use: gicv2, gicv3, plic, lapic or none)
endif
