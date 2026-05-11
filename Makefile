# Makefile for spinlock library
# Usage: make ARCH=x86_64|aarch64|riscv64 [LOG=none|error|warn|info|debug|trace] [ASSERT=panic|off] [target]

# 架构配置
ARCH ?= aarch64

# 平台配置（为空时按架构使用内存布局表中的默认平台）
PLATFORM ?=

# 日志级别配置
LOG ?= info

# 断言配置
ASSERT ?= panic

# 目录设置
SRC_DIR     := examples
LIB_DIR     := lib
BUILD_DIR   := build
INCLUDE_DIR := include
BOOT_DIR    := boot
KERNEL_DIR  := kernel
PLATFORM_DIR := platforms
TESTS_DIR   := tests
CONFIG_DIR  := config
TOOLS_DIR   := tools

# 内存布局（单一真源 -> 自动生成 C 头和 Makefile 片段）
MEM_LAYOUT_SRC := $(CONFIG_DIR)/mem_layout.table
MEM_LAYOUT_GEN := $(TOOLS_DIR)/gen_mem_layout.sh
MEM_LAYOUT_MK  := $(BUILD_DIR)/mem_layout.mk
MEM_LAYOUT_HDR := $(INCLUDE_DIR)/mem_layout.h

# 设备配置（单一真源 -> 自动生成 C 头和 Makefile 片段）
DEVICE_PROFILE_SRC := $(CONFIG_DIR)/device_profile.table
DEVICE_PROFILE_GEN := $(TOOLS_DIR)/gen_device_profile.sh
DEVICE_PROFILE_MK  := $(BUILD_DIR)/device_profile.mk
DEVICE_PROFILE_HDR := $(INCLUDE_DIR)/device_profile.h

$(shell mkdir -p $(BUILD_DIR) >/dev/null 2>&1)
$(shell sh $(MEM_LAYOUT_GEN) $(MEM_LAYOUT_SRC) $(MEM_LAYOUT_MK) $(MEM_LAYOUT_HDR))
-include $(MEM_LAYOUT_MK)

ifeq ($(strip $(MEM_RAM_BASE)),)
$(error Failed to resolve memory layout for ARCH=$(ARCH) PLATFORM=$(PLATFORM))
endif

# 归一化平台名：空 PLATFORM 时回落到该架构默认平台
PLATFORM := $(MEM_LAYOUT_PLATFORM)

$(shell sh $(DEVICE_PROFILE_GEN) $(DEVICE_PROFILE_SRC) $(DEVICE_PROFILE_MK) $(DEVICE_PROFILE_HDR))
-include $(DEVICE_PROFILE_MK)

ifeq ($(strip $(DEV_UART_SRC)),)
$(error Failed to resolve device profile for ARCH=$(ARCH) PLATFORM=$(PLATFORM))
endif

# 日志级别映射
ifeq ($(LOG),none)
    LOG_LEVEL := 0
    LOG_DEFINE := -DLOG_LEVEL=0 -DLOG_NONE
else ifeq ($(LOG),error)
    LOG_LEVEL := 1
    LOG_DEFINE := -DLOG_LEVEL=1
else ifeq ($(LOG),warn)
    LOG_LEVEL := 2
    LOG_DEFINE := -DLOG_LEVEL=2
else ifeq ($(LOG),info)
    LOG_LEVEL := 3
    LOG_DEFINE := -DLOG_LEVEL=3
else ifeq ($(LOG),debug)
    LOG_LEVEL := 4
    LOG_DEFINE := -DLOG_LEVEL=4
else ifeq ($(LOG),trace)
    LOG_LEVEL := 5
    LOG_DEFINE := -DLOG_LEVEL=5
else
    $(error Invalid LOG level. Use: none, error, warn, info, debug, or trace)
endif

# 断言配置映射
ifeq ($(ASSERT),panic)
    ASSERT_DEFINE :=
else ifeq ($(ASSERT),off)
    ASSERT_DEFINE := -DASSERT_OFF
else
    $(error Invalid ASSERT setting. Use: panic or off)
endif

# 源文件
SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS    := $(OBJECTS:.o=.d)

# klog 库源文件
KLOG_SOURCES := $(LIB_DIR)/klog.c
VSNPRINTF_SOURCES := $(LIB_DIR)/vsnprintf.c
STRING_SOURCES := $(LIB_DIR)/string.c
BITMAP_SOURCES := $(LIB_DIR)/bitmap.c
KLOG_OBJECT  := $(BUILD_DIR)/klog.o
VSNPRINTF_OBJECT := $(BUILD_DIR)/vsnprintf.o
STRING_OBJECT := $(BUILD_DIR)/string.o
BITMAP_OBJECT := $(BUILD_DIR)/bitmap.o

# 内核源文件
KERNEL_SOURCES := $(KERNEL_DIR)/main.c
KERNEL_OBJECTS := $(KERNEL_SOURCES:$(KERNEL_DIR)/%.c=$(BUILD_DIR)/kernel_%.o)

# MMU 和 VM 模块
VM_C_SOURCES := $(KERNEL_DIR)/mm/pmm.c $(KERNEL_DIR)/mm/pmm_test.c $(KERNEL_DIR)/mm/vm_user.c
VM_C_OBJECTS := $(BUILD_DIR)/kernel_mm_pmm.o $(BUILD_DIR)/kernel_mm_pmm_test.o $(BUILD_DIR)/kernel_mm_vm_user.o

# 架构特定的 VM 模块
ifeq ($(ARCH),aarch64)
    VM_C_SOURCES += $(KERNEL_DIR)/mm/aarch64/vm_early.c
    VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_vm_early.o
    VM_C_SOURCES += $(KERNEL_DIR)/mm/aarch64/vmm.c
    VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_vmm.o
else ifeq ($(ARCH),riscv64)
    # RISC-V VM 模块（如果有的话）
    # VM_C_SOURCES += $(KERNEL_DIR)/mm/riscv64/vm_early.c
    # VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_vm_early.o
else ifeq ($(ARCH),x86_64)
    # x86_64 VM 模块（如果有的话）
    # VM_C_SOURCES += $(KERNEL_DIR)/mm/x86_64/vm_early.c
    # VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_vm_early.o
endif

# 架构特定的 MMU 汇编
ifeq ($(ARCH),aarch64)
    VM_S_SRC := $(KERNEL_DIR)/mm/aarch64/mmu.S
    VM_EARLY_C_SRC := $(KERNEL_DIR)/mm/aarch64/vm_early.c
else ifeq ($(ARCH),riscv64)
    VM_S_SRC := $(KERNEL_DIR)/mm/riscv64/mmu.S
    # VM_EARLY_C_SRC := $(KERNEL_DIR)/mm/riscv64/vm_early.c
else ifeq ($(ARCH),x86_64)
    VM_S_SRC := $(KERNEL_DIR)/mm/x86_64/mmu.S
    # VM_EARLY_C_SRC := $(KERNEL_DIR)/mm/x86_64/vm_early.c
endif
VM_S_OBJ := $(BUILD_DIR)/kernel_mm_mmu.o

# 如果存在架构特定的 VM 早期初始化代码，添加到编译列表
ifdef VM_EARLY_C_SRC
    VM_C_SOURCES += $(VM_EARLY_C_SRC)
    VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_vm_early.o
endif

# task 模块源文件
TASK_C_SOURCES := $(KERNEL_DIR)/task/task.c $(KERNEL_DIR)/task/sched.c $(KERNEL_DIR)/task/mutex.c
TASK_C_OBJECTS := $(BUILD_DIR)/kernel_task_task.o $(BUILD_DIR)/kernel_task_sched.o $(BUILD_DIR)/kernel_task_mutex.o

# loader 模块源文件
LOADER_C_SOURCES := $(KERNEL_DIR)/loader/bin_loader.c $(KERNEL_DIR)/loader/elf_loader.c
LOADER_C_OBJECTS := $(BUILD_DIR)/kernel_loader_bin_loader.o $(BUILD_DIR)/kernel_loader_elf_loader.o

# syscall 模块源文件
SYSCALL_C_SOURCES := $(KERNEL_DIR)/syscall/syscall.c
SYSCALL_C_OBJECTS := $(BUILD_DIR)/kernel_syscall_syscall.o
SYSCALL_S_SRC := $(LIB_DIR)/syscall.S
SYSCALL_S_OBJ := $(BUILD_DIR)/syscall_wrapper.o
TASK_S_OBJ := $(BUILD_DIR)/task_switch.o
ifeq ($(ARCH),aarch64)
TASK_USER_TEST_OBJ := $(BUILD_DIR)/user_test.o
TASK_USER_HELLO_OBJ := $(BUILD_DIR)/hello.o
TASK_USER_TESTEXECVE_OBJ :=
else ifeq ($(ARCH),riscv64)
TASK_USER_TEST_OBJ := $(BUILD_DIR)/user_test.o
TASK_USER_HELLO_OBJ := $(BUILD_DIR)/hello.o
TASK_USER_TESTEXECVE_OBJ :=
else ifeq ($(ARCH),x86_64)
TASK_USER_TEST_OBJ := $(BUILD_DIR)/user_test.o
TASK_USER_HELLO_OBJ := $(BUILD_DIR)/hello.o
TASK_USER_TESTEXECVE_OBJ := $(BUILD_DIR)/test_execve.o
else
TASK_USER_TEST_OBJ :=
TASK_USER_HELLO_OBJ :=
TASK_USER_TESTEXECVE_OBJ :=
endif

# 测试源文件
TESTS_SOURCES := $(wildcard $(TESTS_DIR)/*.c)
TESTS_OBJECTS := $(TESTS_SOURCES:$(TESTS_DIR)/%.c=$(BUILD_DIR)/tests_%.o)

# 用户应用程序源文件（按架构子目录组织）
APPS_DIR := apps/$(ARCH)
APPS_LD := $(APPS_DIR)/app.ld
# crt0.S 是 C 程序的启动文件，不单独编译为二进制，需排除
APPS_SOURCES := $(filter-out $(APPS_DIR)/crt0.S, $(wildcard $(APPS_DIR)/*.S))
APPS_OBJECTS := $(APPS_SOURCES:$(APPS_DIR)/%.S=$(BUILD_DIR)/apps_%.o)
APPS_BINS := $(APPS_SOURCES:$(APPS_DIR)/%.S=$(BUILD_DIR)/%.bin)

# C 语言用户程序（每个 foo.c 搭配 crt0.S，链接为 foo.elf 放入 rootfs）
APPS_C_SOURCES := $(wildcard $(APPS_DIR)/*.c)
APPS_C_ELFS    := $(APPS_C_SOURCES:$(APPS_DIR)/%.c=$(BUILD_DIR)/%.elf)
CRT0_SRC := $(APPS_DIR)/crt0.S
CRT0_OBJ := $(BUILD_DIR)/apps_crt0.o
SYSCALL_WRAPPER_OBJ := $(BUILD_DIR)/syscall_wrapper.o

# 架构特定的上下文切换汇编
ifeq ($(ARCH),aarch64)
    TASK_S_SRC := $(KERNEL_DIR)/task/aarch64/switch.S
    TASK_USER_TEST_SRC := apps/aarch64/user_test.S
    TASK_USER_HELLO_SRC := apps/aarch64/hello.S
    TASK_USER_LD := $(KERNEL_DIR)/task/user.ld
else ifeq ($(ARCH),riscv64)
    TASK_S_SRC := $(KERNEL_DIR)/task/riscv64/switch.S
    TASK_USER_TEST_SRC := apps/riscv64/user_test.S
    TASK_USER_HELLO_SRC := apps/riscv64/hello.S
    TASK_USER_LD := $(KERNEL_DIR)/task/user.ld
else ifeq ($(ARCH),x86_64)
    TASK_S_SRC := $(KERNEL_DIR)/task/x86_64/switch.S
    TASK_USER_TEST_SRC := apps/x86_64/user_test.S
    TASK_USER_HELLO_SRC := apps/x86_64/hello.S
    TASK_USER_LD := $(KERNEL_DIR)/task/user.ld
endif

# 平台源文件（按 PLATFORM 选择，可扩展到真机）
PLATFORM_SOURCES := $(PLATFORM_DIR)/$(PLATFORM)/platform.c
PLATFORM_OBJECTS := $(PLATFORM_SOURCES:$(PLATFORM_DIR)/%.c=$(BUILD_DIR)/platform_%.o)

ifeq ($(wildcard $(PLATFORM_SOURCES)),)
$(error Missing platform source: $(PLATFORM_SOURCES))
endif

# 驱动源文件（按 ARCH + PLATFORM 设备画像选择）
DRIVER_UART_SRC := $(DEV_UART_SRC)
DRIVER_IRQ_SRC := $(DEV_IRQ_SRC)
DRIVER_TIMER_SRC := $(DEV_TIMER_SRC)

DRIVER_OBJECTS := $(patsubst driver/%.c,$(BUILD_DIR)/drv_%.o,$(DRIVER_UART_SRC))

ifneq ($(strip $(DRIVER_IRQ_SRC)),)
ifeq ($(DRIVER_IRQ_SRC),driver/irq/gicv2.c)
	DRIVER_OBJECTS += $(BUILD_DIR)/gicv2.o
else ifeq ($(DRIVER_IRQ_SRC),driver/irq/gicv3.c)
	DRIVER_OBJECTS += $(BUILD_DIR)/gicv3.o
endif
endif

ifneq ($(strip $(DRIVER_TIMER_SRC)),)
	DRIVER_OBJECTS += $(BUILD_DIR)/timer.o
endif

ifeq ($(DEV_NEED_LAPIC),1)
	DRIVER_OBJECTS += $(BUILD_DIR)/lapic.o
endif

# 启动汇编源文件
BOOT_SOURCES := $(BOOT_DIR)/$(ARCH)/boot.S
BOOT_OBJECTS := $(BOOT_SOURCES:$(BOOT_DIR)/$(ARCH)/%.S=$(BUILD_DIR)/boot_%.o)

# 异常处理源文件
ifeq ($(ARCH),aarch64)
    EXCEPTION_SOURCES := $(BOOT_DIR)/aarch64/exception.S $(BOOT_DIR)/aarch64/exception.c
    EXCEPTION_OBJECTS := $(BUILD_DIR)/exception_asm.o $(BUILD_DIR)/exception.o
else ifeq ($(ARCH),riscv64)
    EXCEPTION_SOURCES := $(BOOT_DIR)/riscv64/exception.S $(BOOT_DIR)/riscv64/exception.c
    EXCEPTION_OBJECTS := $(BUILD_DIR)/rv_exception_asm.o $(BUILD_DIR)/rv_exception.o
else ifeq ($(ARCH),x86_64)
    EXCEPTION_SOURCES := $(BOOT_DIR)/x86_64/exception.S $(BOOT_DIR)/x86_64/exception.c $(BOOT_DIR)/x86_64/syscall_wrapper.S $(BOOT_DIR)/x86_64/tss.c
    EXCEPTION_OBJECTS := $(BUILD_DIR)/x86_exception_asm.o $(BUILD_DIR)/x86_exception.o $(BUILD_DIR)/kernel_syscall_entry.o $(BUILD_DIR)/x86_tss.o
else
    EXCEPTION_SOURCES :=
    EXCEPTION_OBJECTS :=
endif

# 工具（支持交叉编译）
ifeq ($(ARCH),x86_64)
    CC      := /home/ajax/SoftWare/compiler/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc
    AR      := /home/ajax/SoftWare/compiler/x86_64-linux-musl-cross/bin/x86_64-linux-musl-ar
    OBJCOPY := /home/ajax/SoftWare/compiler/x86_64-linux-musl-cross/bin/x86_64-linux-musl-objcopy
    NM      := /home/ajax/SoftWare/compiler/x86_64-linux-musl-cross/bin/x86_64-linux-musl-nm
    CFLAGS  := -Wall -Wextra -O2 -g
	CFLAGS  += -DARCH_X86_64=1
    CFLAGS  += -I$(INCLUDE_DIR)
    CFLAGS  += -I$(INCLUDE_DIR)/x86_64
    CFLAGS  += -I$(BOOT_DIR)/common
    CFLAGS  += $(LOG_DEFINE)
    CFLAGS  += $(ASSERT_DEFINE)
    CFLAGS  += -fno-pie -fno-stack-protector -fno-stack-clash-protection -U_FORTIFY_SOURCE
    CFLAGS  += -ffreestanding -fno-builtin
    CFLAGS  += -mcmodel=large
    CFLAGS  += -mno-mmx -mno-sse
    LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -no-pie
    LDFLAGS += -Wl,-z,max-page-size=0x1000
    TARGET  := $(BUILD_DIR)/spinlock_x86_64.a
    KLOG_TARGET := $(BUILD_DIR)/libklog_x86_64.a
    KERNEL_TARGET := $(BUILD_DIR)/kernel_x86_64.elf
    KERNEL_BIN    := $(BUILD_DIR)/kernel_x86_64.bin
    KERNEL_IMAGE  := $(BUILD_DIR)/kernel_x86_64.img
    QEMU          := qemu-system-x86_64
    QEMU_FLAGS    := -machine q35 -m 2G -nographic -kernel $(KERNEL_BIN)
else ifeq ($(ARCH),aarch64)
    CC      := aarch64-linux-musl-gcc
    AR      := aarch64-linux-musl-ar
    OBJCOPY := aarch64-linux-musl-objcopy
    NM      := aarch64-linux-musl-nm
    CFLAGS  := -Wall -Wextra -O2 -g
	CFLAGS  += -DARCH_AARCH64=1
    CFLAGS  += -I$(INCLUDE_DIR)
    CFLAGS  += -I$(INCLUDE_DIR)/aarch64
    CFLAGS  += -I$(BOOT_DIR)/common
    CFLAGS  += $(LOG_DEFINE)
    CFLAGS  += $(ASSERT_DEFINE)
    CFLAGS  += -fno-pie
    CFLAGS  += -mgeneral-regs-only  # 只使用通用寄存器，禁用 SIMD/FP
    CFLAGS  += -ffreestanding -fno-builtin  # 禁用内置函数和标准库
    TARGET  := $(BUILD_DIR)/spinlock_aarch64.a
    KLOG_TARGET := $(BUILD_DIR)/libklog_aarch64.a
    KERNEL_TARGET := $(BUILD_DIR)/kernel_aarch64.elf
    KERNEL_BIN    := $(BUILD_DIR)/kernel_aarch64.bin
    QEMU          := qemu-system-aarch64
    QEMU_FLAGS    := -cpu cortex-a76 -M virt,virtualization=on -m 2G -nographic -kernel $(KERNEL_BIN)
else ifeq ($(ARCH),riscv64)
    CC      := riscv64-linux-musl-gcc
    AR      := riscv64-linux-musl-ar
    OBJCOPY := riscv64-linux-musl-objcopy
    NM      := riscv64-linux-musl-nm
    CFLAGS  := -Wall -Wextra -O2 -g
    CFLAGS  += -march=rv64gc -mabi=lp64
	CFLAGS  += -DARCH_RISCV64=1
    CFLAGS  += -I$(INCLUDE_DIR)
    CFLAGS  += -I$(INCLUDE_DIR)/riscv64
    CFLAGS  += -I$(BOOT_DIR)/common
    CFLAGS  += $(LOG_DEFINE)
    CFLAGS  += $(ASSERT_DEFINE)
    CFLAGS  += -mcmodel=medany
    CFLAGS  += -fno-pic -fno-pie
    CFLAGS  += -ffreestanding -fno-builtin
    # -no-pie：让链接器输出非 PIE 静态可执行文件，避免生成
    # R_RISCV_RELATIVE 重定位条目（裸机内核无动态链接器处理它们）
    LDFLAGS := -no-pie
    TARGET  := $(BUILD_DIR)/spinlock_riscv64.a
    KLOG_TARGET := $(BUILD_DIR)/libklog_riscv64.a
    KERNEL_TARGET := $(BUILD_DIR)/kernel_riscv64.elf
    KERNEL_BIN    := $(BUILD_DIR)/kernel_riscv64.bin
    QEMU          := qemu-system-riscv64
    QEMU_FLAGS    := -M virt -m 2G -nographic -bios default -kernel $(KERNEL_BIN)
else
    $(error Unsupported architecture: $(ARCH). Use ARCH=x86_64, aarch64 or riscv64)
endif

# 通用编译标志
CFLAGS  += -nostdinc
CFLAGS  += -Idriver
CFLAGS  += -Ikernel
CFLAGS  += -Ikernel/mm
CFLAGS  += -DAVATAR_HAS_FILESYSTEM
CFLAGS  += -DPLATFORM_$(MEM_PLATFORM_DEFINE)=1

# UART 驱动选择（与架构解耦）
# 用法：make ARCH=aarch64 PLATFORM=qemu UART=dw kernel
# 不指定时使用设备画像默认值（DEV_DEFAULT_UART）
UART ?= $(DEV_DEFAULT_UART)
ifeq ($(UART),pl011)
	DRIVER_UART_SRC := driver/uart/uart_pl011.c
    CFLAGS  += -DDRIVER_UART_PL011=1
else ifeq ($(UART),dw)
	DRIVER_UART_SRC := driver/uart/uart_dw.c
    CFLAGS  += -DDRIVER_UART_DW=1
else ifeq ($(UART),x86)
	DRIVER_UART_SRC := driver/uart/uart_x86.c
	CFLAGS  += -DDRIVER_UART_X86=1
else ifneq ($(UART),)
	$(error Invalid UART. Use: pl011, dw or x86)
endif

# GIC 版本选择
# 用法：make ARCH=aarch64 PLATFORM=qemu GIC=v3 kernel
# 非 GIC 平台建议设为 none；不指定时取设备画像默认值
GIC ?= $(DEV_DEFAULT_GIC)
ifeq ($(GIC),v3)
	DRIVER_IRQ_SRC := driver/irq/gicv3.c
    CFLAGS  += -DDRIVER_GIC_V3=1
else ifeq ($(GIC),v2)
	DRIVER_IRQ_SRC := driver/irq/gicv2.c
    CFLAGS  += -DDRIVER_GIC_V2=1
else ifeq ($(GIC),none)
	DRIVER_IRQ_SRC :=
else ifneq ($(GIC),)
	$(error Invalid GIC. Use: v2, v3 or none)
endif

ifneq ($(ARCH),aarch64)
ifneq ($(GIC),none)
$(error GIC is only valid on aarch64. Use GIC=none for ARCH=$(ARCH))
endif
endif

ifneq ($(ARCH),x86_64)
ifeq ($(UART),x86)
$(error UART=x86 is only valid on x86_64)
endif
endif

ifneq ($(ARCH),aarch64)
ifeq ($(UART),pl011)
$(error UART=pl011 is only valid on aarch64)
endif
endif

# 覆盖后重新组装驱动对象列表
DRIVER_OBJECTS := $(patsubst driver/%.c,$(BUILD_DIR)/drv_%.o,$(DRIVER_UART_SRC))
ifneq ($(strip $(DRIVER_IRQ_SRC)),)
ifeq ($(DRIVER_IRQ_SRC),driver/irq/gicv2.c)
	DRIVER_OBJECTS += $(BUILD_DIR)/gicv2.o
else ifeq ($(DRIVER_IRQ_SRC),driver/irq/gicv3.c)
	DRIVER_OBJECTS += $(BUILD_DIR)/gicv3.o
endif
endif
ifneq ($(strip $(DRIVER_TIMER_SRC)),)
	DRIVER_OBJECTS += $(BUILD_DIR)/timer.o
endif
ifeq ($(DEV_NEED_LAPIC),1)
	DRIVER_OBJECTS += $(BUILD_DIR)/lapic.o
endif

CFLAGS  += -MMD -MP
MKDIR   := mkdir -p

# ─── lwext4 文件系统 ────────────────────────────────────────────────────────
FS_DIR          := fs
LWEXT4_DIR      := $(FS_DIR)/lwext4
LWEXT4_PORT_DIR := $(FS_DIR)/lwext4_port
LWEXT4_COMPAT   := $(FS_DIR)/compat

# lwext4 库源文件（第三方代码）
LWEXT4_SRCS     := $(wildcard $(LWEXT4_DIR)/src/*.c)
LWEXT4_OBJS     := $(patsubst $(LWEXT4_DIR)/src/%.c,$(BUILD_DIR)/lwext4_%.o,$(LWEXT4_SRCS))

# lwext4 移植胶水代码（属于本项目，使用 LWEXT4_CFLAGS）
LWEXT4_PORT_OBJS := $(BUILD_DIR)/lwext4_port_kmalloc.o \
                   $(BUILD_DIR)/lwext4_port_libc_stub.o \
                   $(BUILD_DIR)/drv_blk_ramblk.o \
                   $(BUILD_DIR)/lwext4_port_fs_init.o

# lwext4 专用编译标志（在通用 CFLAGS 基础上添加）
LWEXT4_CFLAGS  := $(CFLAGS)
LWEXT4_CFLAGS  += -I$(LWEXT4_DIR)/include   # lwext4 头文件
LWEXT4_CFLAGS  += -I$(LWEXT4_PORT_DIR)      # generated/ext4_config.h 所在目录
LWEXT4_CFLAGS  += -I$(LWEXT4_COMPAT)        # compat 标准库头文件
LWEXT4_CFLAGS  += -DCONFIG_USE_DEFAULT_CFG=0  # 使用自定义 ext4_config.h
# 以下定义与 generated/ext4_config.h 保持一致，防止默认值覆盖
LWEXT4_CFLAGS  += -DCONFIG_HAVE_OWN_ERRNO=1
LWEXT4_CFLAGS  += -DCONFIG_HAVE_OWN_OFLAGS=1
LWEXT4_CFLAGS  += -DCONFIG_DEBUG_PRINTF=0
LWEXT4_CFLAGS  += -DCONFIG_DEBUG_ASSERT=0
LWEXT4_CFLAGS  += -DCONFIG_HAVE_OWN_ASSERT=1
LWEXT4_CFLAGS  += -DCONFIG_USE_USER_MALLOC=1
LWEXT4_CFLAGS  += -w   # 屏蔽第三方代码警告

# ─── Rootfs 配置 ────────────────────────────────────────────────────────────────────
ROOTFS_IMG       := $(BUILD_DIR)/rootfs.img
# ROOTFS_SIZE_MB / ROOTFS_PHYS_ADDR 来自自动生成的 $(MEM_LAYOUT_MK)

# 目标
.PHONY: all clean help klog kernel run rootfs run-fs

all: $(TARGET) klog

kernel: | kernel_clean
kernel: $(KERNEL_BIN)

# 在切换架构时自动清理
.PHONY: kernel_clean
kernel_clean:
	@if [ -f .arch ]; then \
		if [ "$$(cat .arch)" != "$(ARCH)" ]; then \
			echo "Switching architecture from $$(cat .arch) to $(ARCH), cleaning..."; \
			rm -rf $(BUILD_DIR) && echo "$(ARCH)" > .arch; \
		fi \
	else \
		echo "$(ARCH)" > .arch; \
	fi

klog: $(KLOG_TARGET)

$(BUILD_DIR):
	$(MKDIR) $(BUILD_DIR)

$(TARGET): $(OBJECTS) | $(BUILD_DIR)
	$(AR) rcs $@ $^

$(KLOG_TARGET): $(KLOG_OBJECT) | $(BUILD_DIR)
	$(AR) rcs $@ $^

$(BUILD_DIR)/klog.o: $(LIB_DIR)/klog.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vsnprintf.o: $(LIB_DIR)/vsnprintf.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/string.o: $(LIB_DIR)/string.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 内核编译规则
$(BUILD_DIR)/kernel_%.o: $(KERNEL_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests_%.o: $(TESTS_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/platform_%.o: $(PLATFORM_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/boot_%.o: $(BOOT_DIR)/$(ARCH)/%.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 异常处理编译规则（AArch64）
$(BUILD_DIR)/exception_asm.o: $(BOOT_DIR)/aarch64/exception.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/exception.o: $(BOOT_DIR)/aarch64/exception.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 异常处理编译规则（RISC-V）
$(BUILD_DIR)/rv_exception_asm.o: $(BOOT_DIR)/riscv64/exception.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rv_exception.o: $(BOOT_DIR)/riscv64/exception.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 异常处理编译规则（x86_64）
$(BUILD_DIR)/x86_exception_asm.o: $(BOOT_DIR)/x86_64/exception.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/x86_exception.o: $(BOOT_DIR)/x86_64/exception.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_entry.o: $(BOOT_DIR)/x86_64/syscall_wrapper.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/x86_tss.o: $(BOOT_DIR)/x86_64/tss.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# LAPIC 驱动编译规则（x86_64）
$(BUILD_DIR)/lapic.o: driver/irq/lapic.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# GIC 和 timer 驱动编译规则（AArch64）
$(BUILD_DIR)/gicv2.o: driver/irq/gicv2.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gicv3.o: driver/irq/gicv3.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/timer.o: driver/timer/timer.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 驱动编译规则
$(BUILD_DIR)/drv_%.o: driver/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# task 模块编译规则
$(BUILD_DIR)/kernel_task_task.o: $(KERNEL_DIR)/task/task.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_task_sched.o: $(KERNEL_DIR)/task/sched.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_task_mutex.o: $(KERNEL_DIR)/task/mutex.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/task_switch.o: $(TASK_S_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# loader 模块编译规则
$(BUILD_DIR)/kernel_loader_bin_loader.o: $(KERNEL_DIR)/loader/bin_loader.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_loader_elf_loader.o: $(KERNEL_DIR)/loader/elf_loader.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

# syscall 模块编译规则
$(BUILD_DIR)/kernel_syscall_syscall.o: $(KERNEL_DIR)/syscall/syscall.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/syscall_wrapper.o: $(SYSCALL_S_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 用户测试程序编译规则
$(BUILD_DIR)/user_test.o: $(TASK_USER_TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# hello 用户程序编译规则
$(BUILD_DIR)/hello.o: $(TASK_USER_HELLO_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# test_execve 用户程序编译规则（x86_64 only）
ifeq ($(ARCH),x86_64)
$(BUILD_DIR)/test_execve.o: apps/x86_64/test_execve.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

# 用户应用程序编译规则（从文件系统加载）
$(BUILD_DIR)/apps_%.o: $(APPS_DIR)/%.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DAPP_ELF=1 -c $< -o $@

# 生成应用程序二进制文件
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/apps_%.o $(APPS_LD) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -nostdlib -nostartfiles -nodefaultlibs -T $(APPS_LD) -o $@.elf $<
	$(OBJCOPY) -O binary $@.elf $@
	@echo "App binary created: $@"
	@echo "  Entry point: $(shell $(NM) $@.elf 2>/dev/null | grep ' _start')"

# crt0 编译规则
$(CRT0_OBJ): $(CRT0_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# C 用户程序编译规则（crt0 + foo.c + syscall_wrapper → foo.elf，放入 rootfs）
$(BUILD_DIR)/%.elf: $(APPS_DIR)/%.c $(CRT0_OBJ) $(SYSCALL_WRAPPER_OBJ) $(APPS_LD) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/apps_$*.o
	$(CC) $(CFLAGS) -static -nostdlib -nostartfiles -nodefaultlibs \
		-T $(APPS_LD) \
		$(CRT0_OBJ) $(BUILD_DIR)/apps_$*.o $(SYSCALL_WRAPPER_OBJ) \
		-o $@
	@echo "C app ELF created: $@"
$(TASK_USER_BIN): $(BUILD_DIR)/user_test.o $(TASK_USER_LD) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -nostdlib -nostartfiles -nodefaultlibs -T $(TASK_USER_LD) -o $@.elf $<
	$(OBJCOPY) -O binary $@.elf $@
	@echo "User program linked at: $(shell aarch64-linux-musl-nm $@.elf | grep user_test_program)"
	@echo "User data at: $(shell aarch64-linux-musl-nm $@.elf | grep msg_hello)"

# VM 模块编译规则
$(BUILD_DIR)/kernel_mm_vm_early.o: $(VM_EARLY_C_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_mm_pmm.o: $(KERNEL_DIR)/mm/pmm.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_mm_pmm_test.o: $(KERNEL_DIR)/mm/pmm_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 架构特定的 VMM 模块（仅 AArch64）
ifeq ($(ARCH),aarch64)
$(BUILD_DIR)/kernel_mm_vmm.o: $(KERNEL_DIR)/mm/aarch64/vmm.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

$(BUILD_DIR)/kernel_mm_vm_user.o: $(KERNEL_DIR)/mm/vm_user.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/bitmap.o: $(LIB_DIR)/bitmap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_mm_mmu.o: $(VM_S_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ─── lwext4 编译规则 ──────────────────────────────────────────────────────────
# 第三方 lwext4 源文件：使用包含 compat 路径的专用 LWEXT4_CFLAGS
$(BUILD_DIR)/lwext4_%.o: $(LWEXT4_DIR)/src/%.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

# lwext4 移植胶水代码：属于本项目，使用普通 CFLAGS
$(BUILD_DIR)/lwext4_port_kmalloc.o: $(LWEXT4_PORT_DIR)/kmalloc.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lwext4_port_libc_stub.o: $(LWEXT4_PORT_DIR)/libc_stub.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# RAM 块设备和 FS 初始化（需要 lwext4 头文件，使用 LWEXT4_CFLAGS）
$(BUILD_DIR)/drv_blk_ramblk.o: driver/blk/ramblk.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWEXT4_CFLAGS) -Idriver -c $< -o $@

$(BUILD_DIR)/lwext4_port_fs_init.o: $(LWEXT4_PORT_DIR)/fs_init.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -Ifs/lwext4_port -c $< -o $@

# 链接内核 ELF 文件
$(KERNEL_TARGET): $(BOOT_OBJECTS) $(KERNEL_OBJECTS) $(TASK_C_OBJECTS) $(TASK_S_OBJ) $(TASK_USER_TEST_OBJ) $(TASK_USER_HELLO_OBJ) $(TASK_USER_TESTEXECVE_OBJ) $(LOADER_C_OBJECTS) $(SYSCALL_C_OBJECTS) $(SYSCALL_S_OBJ) $(VM_C_OBJECTS) $(VM_S_OBJ) $(TESTS_OBJECTS) $(PLATFORM_OBJECTS) $(DRIVER_OBJECTS) $(EXCEPTION_OBJECTS) $(KLOG_OBJECT) $(VSNPRINTF_OBJECT) $(STRING_OBJECT) $(BITMAP_OBJECT) $(LWEXT4_OBJS) $(LWEXT4_PORT_OBJS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -nostartfiles -nodefaultlibs -T $(BOOT_DIR)/$(ARCH)/link.ld -o $@ $^

# 转换为二进制文件
$(KERNEL_BIN): $(KERNEL_TARGET)
	$(OBJCOPY) -O binary $< $@

# 创建软盘镜像（1.44MB）
$(KERNEL_IMAGE): $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=1024 count=1440
	dd if=$< of=$@ bs=512 conv=notrunc

# 运行内核
run: kernel
	@echo "Starting QEMU for $(ARCH)..."
	$(QEMU) $(QEMU_FLAGS)

# 创建 ext4 rootfs 镜像
# 依赖：Host 已安装 e2fsprogs（mkfs.ext4）
$(ROOTFS_IMG): | $(BUILD_DIR)
	@echo "Creating $(ROOTFS_SIZE_MB)MB ext4 rootfs at $(ROOTFS_IMG)..."
	dd if=/dev/zero of=$@ bs=1M count=$(ROOTFS_SIZE_MB)
	mkfs.ext4 -b 1024 -L "avatarfs" $@
	@echo "Rootfs created: $@"

rootfs: $(ROOTFS_IMG) $(APPS_BINS) $(APPS_C_ELFS)
	@echo "=================================="
	@echo "Rootfs and applications built!"
	@echo "=================================="
	@echo ""
	@echo "Installing applications to rootfs..."
	@mkdir -p /tmp/avatar_mnt
	@sudo mount -o loop $(ROOTFS_IMG) /tmp/avatar_mnt
	@if [ -f apps/busybox-$(ARCH) ]; then \
		sudo cp apps/busybox-$(ARCH) /tmp/avatar_mnt/busybox; \
		sudo chmod +x /tmp/avatar_mnt/busybox; \
		echo "  [busybox installed]"; \
	fi
	@if [ -f build/test_exec.bin ]; then \
		sudo cp build/test_exec.bin /tmp/avatar_mnt/test_exec; \
		sudo chmod +x /tmp/avatar_mnt/test_exec; \
		echo "  [test_exec installed]"; \
	fi
	@if [ -f build/init.elf ]; then \
		sudo cp build/init.elf /tmp/avatar_mnt/init; \
		sudo chmod +x /tmp/avatar_mnt/init; \
		echo "  [init installed]"; \
	fi
	@sudo umount /tmp/avatar_mnt
	@sudo rmdir /tmp/avatar_mnt 2>/dev/null || true
	@echo ""
	@echo "Rootfs ready! Run: make ARCH=$(ARCH) run-fs"
	@echo ""

# 运行内核 + 加载 rootfs 酷像到 QEMU 客户机内存
run-fs: kernel rootfs
	@echo "Starting QEMU for $(ARCH) with rootfs at $(ROOTFS_PHYS_ADDR)..."
	$(QEMU) $(QEMU_FLAGS) \
		-device loader,file=$(ROOTFS_IMG),addr=$(ROOTFS_PHYS_ADDR),force-raw=on

clean:
	rm -rf build/*

help:
	@echo "Avatar OS Makefile"
	@echo ""
	@echo "Usage: make ARCH=<arch> [PLATFORM=<platform>] [LOG=<level>] [ASSERT=<mode>] [target]"
	@echo ""
	@echo "Architectures:"
	@echo "  ARCH=x86_64    Build for x86_64 (AMD64/Intel 64)"
	@echo "  ARCH=aarch64   Build for AArch64 (ARM 64-bit)"
	@echo "  ARCH=riscv64   Build for RISC-V 64-bit"
	@echo ""
	@echo "Platforms:"
	@echo "  PLATFORM=qemu  QEMU virt platform (default for all arch now)"
	@echo "  (Future real boards can be added in config/mem_layout.table)"
	@echo ""
	@echo "Log Levels:"
	@echo "  LOG=none      Disable all logging (default: info)"
	@echo "  LOG=error     Show only errors"
	@echo "  LOG=warn      Show warnings and errors"
	@echo "  LOG=info      Show info, warnings and errors (default)"
	@echo "  LOG=debug     Show debug info and above"
	@echo "  LOG=trace     Show all logs including trace"
	@echo ""
	@echo "Assert Modes:"
	@echo "  ASSERT=panic  Enable assertions, panic on failure (default)"
	@echo "  ASSERT=off    Disable all assertions (release mode)"
	@echo ""
	@echo "Cross-compiler:"
	@echo "  CC=<compiler>  Specify compiler (x86_64: gcc, aarch64: aarch64-linux-musl-gcc, riscv64: riscv64-linux-musl-gcc)"
	@echo ""
	@echo "Targets:"
	@echo "  all           Build static libraries (default)"
	@echo "  klog          Build klog library only"
	@echo "  kernel        Build kernel image"
	@echo "  run           Build and run kernel in QEMU"
	@echo "  clean         Remove build artifacts"
	@echo "  help          Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make ARCH=aarch64"
	@echo "  make ARCH=aarch64 PLATFORM=qemu"
	@echo "  make ARCH=aarch64 LOG=debug"
	@echo "  make ARCH=riscv64 LOG=trace"
	@echo "  make ARCH=x86_64 ASSERT=off"
	@echo "  make ARCH=aarch64 kernel"
	@echo "  make ARCH=aarch64 run"
	@echo "  make ARCH=aarch64 LOG=debug ASSERT=panic"
	@echo "  make ARCH=riscv64 clean"

# 包含依赖文件
-include $(DEPS)
