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
KLOG_OBJECT  := $(BUILD_DIR)/klog.o
VSNPRINTF_OBJECT := $(BUILD_DIR)/vsnprintf.o
STRING_OBJECT := $(BUILD_DIR)/string.o

# 内核源文件
KERNEL_SOURCES := $(KERNEL_DIR)/main.c
KERNEL_OBJECTS := $(KERNEL_SOURCES:$(KERNEL_DIR)/%.c=$(BUILD_DIR)/kernel_%.o)

# 测试源文件
TESTS_SOURCES := $(wildcard $(TESTS_DIR)/*.c)
TESTS_OBJECTS := $(TESTS_SOURCES:$(TESTS_DIR)/%.c=$(BUILD_DIR)/tests_%.o)

# 平台源文件
PLATFORM_SOURCES := $(PLATFORM_DIR)/qemu/platform.c
PLATFORM_OBJECTS := $(PLATFORM_SOURCES:$(PLATFORM_DIR)/%.c=$(BUILD_DIR)/platform_%.o)

# 启动汇编源文件
BOOT_SOURCES := $(BOOT_DIR)/$(ARCH)/boot.S
BOOT_OBJECTS := $(BOOT_SOURCES:$(BOOT_DIR)/$(ARCH)/%.S=$(BUILD_DIR)/boot_%.o)

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
    QEMU_FLAGS    := -machine q35 -m 1G -nographic -kernel $(KERNEL_BIN)
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
    QEMU_FLAGS    := -cpu cortex-a72 -M virt -m 1G -nographic -kernel $(KERNEL_BIN)
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
    KLOG_TARGET := $(BUILD_DIR)(BUILD_DIR)/libklog_riscv64.a
    KERNEL_TARGET := $(BUILD_DIR)/kernel_riscv64.elf
    KERNEL_BIN    := $(BUILD_DIR)/kernel_riscv64.bin
    QEMU          := qemu-system-riscv64
    QEMU_FLAGS    := -M virt -m 1G -nographic -bios default -kernel $(KERNEL_BIN)
else
    $(error Unsupported architecture: $(ARCH). Use ARCH=x86_64, aarch64 or riscv64)
endif

# 通用编译标志
CFLAGS  += -nostdinc
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

# 链接内核 ELF 文件
$(KERNEL_TARGET): $(BOOT_OBJECTS) $(KERNEL_OBJECTS) $(TESTS_OBJECTS) $(PLATFORM_OBJECTS) $(KLOG_OBJECT) $(VSNPRINTF_OBJECT) $(STRING_OBJECT) | $(BUILD_DIR)
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
