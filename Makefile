# Makefile for spinlock library
# Usage: make ARCH=x86_64|aarch64|riscv64 [LOG=none|error|warn|info|debug|trace] [ASSERT=panic|off] [target]

# 架构配置
ARCH ?= aarch64

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
VM_C_SOURCES := $(KERNEL_DIR)/mm/pmm.c $(KERNEL_DIR)/mm/pmm_test.c
VM_C_OBJECTS := $(BUILD_DIR)/kernel_mm_pmm.o $(BUILD_DIR)/kernel_mm_pmm_test.o

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

# 架构特定的上下文切换汇编
ifeq ($(ARCH),aarch64)
    TASK_S_SRC := $(KERNEL_DIR)/task/aarch64/switch.S
else ifeq ($(ARCH),riscv64)
    TASK_S_SRC := $(KERNEL_DIR)/task/riscv64/switch.S
else ifeq ($(ARCH),x86_64)
    TASK_S_SRC := $(KERNEL_DIR)/task/x86_64/switch.S
endif
TASK_S_OBJ := $(BUILD_DIR)/task_switch.o

# 测试源文件
TESTS_SOURCES := $(wildcard $(TESTS_DIR)/*.c)
TESTS_OBJECTS := $(TESTS_SOURCES:$(TESTS_DIR)/%.c=$(BUILD_DIR)/tests_%.o)

# 平台源文件
PLATFORM_SOURCES := $(PLATFORM_DIR)/qemu/platform.c
PLATFORM_OBJECTS := $(PLATFORM_SOURCES:$(PLATFORM_DIR)/%.c=$(BUILD_DIR)/platform_%.o)

# 驱动源文件（按架构选择 UART 驱动）
ifeq ($(ARCH),aarch64)
    DRIVER_UART_SRC := driver/uart/uart_pl011.c
    DRIVER_IRQ_SRC := driver/irq/gicv2.c
    DRIVER_TIMER_SRC := driver/timer/timer.c
else ifeq ($(ARCH),riscv64)
    DRIVER_UART_SRC := driver/uart/uart_dw.c
    DRIVER_IRQ_SRC :=
    DRIVER_TIMER_SRC := driver/timer/timer.c
else ifeq ($(ARCH),x86_64)
    DRIVER_UART_SRC := driver/uart/uart_x86.c
    DRIVER_IRQ_SRC :=
    DRIVER_TIMER_SRC := driver/timer/timer.c
endif
DRIVER_OBJECTS := $(patsubst driver/%.c,$(BUILD_DIR)/drv_%.o,$(DRIVER_UART_SRC))
ifeq ($(ARCH),aarch64)
    DRIVER_OBJECTS += $(BUILD_DIR)/gicv2.o $(BUILD_DIR)/timer.o
else ifeq ($(ARCH),riscv64)
    DRIVER_OBJECTS += $(BUILD_DIR)/timer.o
else ifeq ($(ARCH),x86_64)
    DRIVER_OBJECTS += $(BUILD_DIR)/timer.o $(BUILD_DIR)/lapic.o
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
    EXCEPTION_SOURCES := $(BOOT_DIR)/x86_64/exception.S $(BOOT_DIR)/x86_64/exception.c
    EXCEPTION_OBJECTS := $(BUILD_DIR)/x86_exception_asm.o $(BUILD_DIR)/x86_exception.o
else
    EXCEPTION_SOURCES :=
    EXCEPTION_OBJECTS :=
endif

# 工具（支持交叉编译）
ifeq ($(ARCH),x86_64)
    CC      := /home/ajax/SoftWare/compiler/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc
    AR      := /home/ajax/SoftWare/compiler/x86_64-linux-musl-cross/bin/x86_64-linux-musl-ar
    OBJCOPY := /home/ajax/SoftWare/compiler/x86_64-linux-musl-cross/bin/x86_64-linux-musl-objcopy
    CFLAGS  := -Wall -Wextra -O2 -g
    CFLAGS  += -D__x86_64__
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
    CFLAGS  := -Wall -Wextra -O2 -g
    CFLAGS  += -D__aarch64__
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
    QEMU_FLAGS    := -cpu cortex-a72 -M virt -m 2G -nographic -kernel $(KERNEL_BIN)
else ifeq ($(ARCH),riscv64)
    CC      := riscv64-linux-musl-gcc
    AR      := riscv64-linux-musl-ar
    OBJCOPY := riscv64-linux-musl-objcopy
    CFLAGS  := -Wall -Wextra -O2 -g
    CFLAGS  += -march=rv64gc -mabi=lp64
    CFLAGS  += -D__riscv -D__riscv_xlen=64
    CFLAGS  += -I$(INCLUDE_DIR)
    CFLAGS  += -I$(INCLUDE_DIR)/riscv64
    CFLAGS  += -I$(BOOT_DIR)/common
    CFLAGS  += $(LOG_DEFINE)
    CFLAGS  += $(ASSERT_DEFINE)
    CFLAGS  += -mcmodel=medany
    CFLAGS  += -fno-pic  # 明确禁用位置无关代码
    CFLAGS  += -ffreestanding -fno-builtin
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

# UART 驱动选择（与架构解耦）
# 用法：make ARCH=aarch64 UART=dw kernel
# 不指定时由 driver_cfg.h 按架构选默认值
UART ?=
ifeq ($(UART),pl011)
    CFLAGS  += -DDRIVER_UART_PL011=1
else ifeq ($(UART),dw)
    CFLAGS  += -DDRIVER_UART_DW=1
else ifneq ($(UART),)
    $(error Invalid UART. Use: pl011 or dw)
endif

# GIC 版本选择（仅 aarch64）
# 用法：make ARCH=aarch64 GIC=v3 kernel
GIC ?=
ifeq ($(GIC),v3)
    CFLAGS  += -DDRIVER_GIC_V3=1
else ifeq ($(GIC),v2)
    CFLAGS  += -DDRIVER_GIC_V2=1
else ifneq ($(GIC),)
    $(error Invalid GIC. Use: v2 or v3)
endif

CFLAGS  += -MMD -MP
MKDIR   := mkdir -p

# 目标
.PHONY: all clean help klog kernel run

all: $(TARGET) klog

kernel: kernel_clean $(KERNEL_BIN)

# 在切换架构时自动清理
.PHONY: kernel_clean
kernel_clean:
	@if [ -f .arch ]; then \
		if [ "$$(cat .arch)" != "$(ARCH)" ]; then \
			echo "Switching architecture from $$(cat .arch) to $(ARCH), cleaning..."; \
			rm -rf $(BUILD_DIR); \
		fi \
	fi
	@echo "$(ARCH)" > .arch

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

# LAPIC 驱动编译规则（x86_64）
$(BUILD_DIR)/lapic.o: driver/irq/lapic.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# GIC 和 timer 驱动编译规则（AArch64）
$(BUILD_DIR)/gicv2.o: driver/irq/gicv2.c | $(BUILD_DIR)
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

$(BUILD_DIR)/bitmap.o: $(LIB_DIR)/bitmap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_mm_mmu.o: $(VM_S_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 链接内核 ELF 文件
$(KERNEL_TARGET): $(BOOT_OBJECTS) $(KERNEL_OBJECTS) $(TASK_C_OBJECTS) $(TASK_S_OBJ) $(VM_C_OBJECTS) $(VM_S_OBJ) $(TESTS_OBJECTS) $(PLATFORM_OBJECTS) $(DRIVER_OBJECTS) $(EXCEPTION_OBJECTS) $(KLOG_OBJECT) $(VSNPRINTF_OBJECT) $(STRING_OBJECT) $(BITMAP_OBJECT) | $(BUILD_DIR)
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

clean:
	rm -rf build/*

help:
	@echo "Avatar OS Makefile"
	@echo ""
	@echo "Usage: make ARCH=<arch> [LOG=<level>] [ASSERT=<mode>] [target]"
	@echo ""
	@echo "Architectures:"
	@echo "  ARCH=x86_64    Build for x86_64 (AMD64/Intel 64)"
	@echo "  ARCH=aarch64   Build for AArch64 (ARM 64-bit)"
	@echo "  ARCH=riscv64   Build for RISC-V 64-bit"
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
	@echo "  make ARCH=aarch64 LOG=debug"
	@echo "  make ARCH=riscv64 LOG=trace"
	@echo "  make ARCH=x86_64 ASSERT=off"
	@echo "  make ARCH=aarch64 kernel"
	@echo "  make ARCH=aarch64 run"
	@echo "  make ARCH=aarch64 LOG=debug ASSERT=panic"
	@echo "  make ARCH=riscv64 clean"

# 包含依赖文件
-include $(DEPS)
