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

# 日志级别映射
ifeq ($(LOG),none)
    LOG_LEVEL := 0
    LOG_DEFINE := -DLOG_LEVEL=LOG_LEVEL_NONE -DLOG_NONE
else ifeq ($(LOG),error)
    LOG_LEVEL := 1
    LOG_DEFINE := -DLOG_LEVEL=LOG_LEVEL_ERROR
else ifeq ($(LOG),warn)
    LOG_LEVEL := 2
    LOG_DEFINE := -DLOG_LEVEL=LOG_LEVEL_WARN
else ifeq ($(LOG),info)
    LOG_LEVEL := 3
    LOG_DEFINE := -DLOG_LEVEL=LOG_LEVEL_INFO
else ifeq ($(LOG),debug)
    LOG_LEVEL := 4
    LOG_DEFINE := -DLOG_LEVEL=LOG_LEVEL_DEBUG
else ifeq ($(LOG),trace)
    LOG_LEVEL := 5
    LOG_DEFINE := -DLOG_LEVEL=LOG_LEVEL_TRACE
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
KLOG_OBJECT  := $(BUILD_DIR)/klog.o

# 工具（支持交叉编译）
ifeq ($(ARCH),x86_64)
    CC      ?= gcc
    AR      := ar
    CFLAGS  := -Wall -Wextra -O2 -g
    CFLAGS  += -D__x86_64__
    CFLAGS  += -I$(INCLUDE_DIR)
    CFLAGS  += -I$(INCLUDE_DIR)/x86_64
    CFLAGS  += $(LOG_DEFINE)
    CFLAGS  += $(ASSERT_DEFINE)
    TARGET  := $(BUILD_DIR)/spinlock_x86_64.a
    KLOG_TARGET := $(BUILD_DIR)/libklog_x86_64.a
else ifeq ($(ARCH),aarch64)
    CC      := aarch64-linux-musl-gcc
    AR      := aarch64-linux-musl-ar
    CFLAGS  := -Wall -Wextra -O2 -g
    CFLAGS  += -D__aarch64__
    CFLAGS  += -I$(INCLUDE_DIR)
    CFLAGS  += -I$(INCLUDE_DIR)/aarch64
    CFLAGS  += $(LOG_DEFINE)
    CFLAGS  += $(ASSERT_DEFINE)
    TARGET  := $(BUILD_DIR)/spinlock_aarch64.a
    KLOG_TARGET := $(BUILD_DIR)/libklog_aarch64.a
else ifeq ($(ARCH),riscv64)
    CC      := riscv64-linux-musl-gcc
    AR      := riscv64-linux-musl-ar
    CFLAGS  := -Wall -Wextra -O2 -g
    CFLAGS  += -march=rv64gc -mabi=lp64
    CFLAGS  += -D__riscv -D__riscv_xlen=64
    CFLAGS  += -I$(INCLUDE_DIR)
    CFLAGS  += -I$(INCLUDE_DIR)/riscv64
    CFLAGS  += $(LOG_DEFINE)
    CFLAGS  += $(ASSERT_DEFINE)
    TARGET  := $(BUILD_DIR)/spinlock_riscv64.a
    KLOG_TARGET := $(BUILD_DIR)/libklog_riscv64.a
else
    $(error Unsupported architecture: $(ARCH). Use ARCH=x86_64, aarch64 or riscv64)
endif

# 通用编译标志
CFLAGS  += -nostdinc
CFLAGS  += -MMD -MP
MKDIR   := mkdir -p

# 目标
.PHONY: all clean help klog

all: $(TARGET) klog

klog: $(KLOG_TARGET)

$(BUILD_DIR):
	$(MKDIR) $(BUILD_DIR)

$(TARGET): $(OBJECTS) | $(BUILD_DIR)
	$(AR) rcs $@ $^

$(KLOG_TARGET): $(KLOG_OBJECT) | $(BUILD_DIR)
	$(AR) rcs $@ $^

$(BUILD_DIR)/klog.o: $(LIB_DIR)/klog.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Spinlock Makefile"
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
	@echo "  clean         Remove build artifacts"
	@echo "  help          Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make ARCH=aarch64"
	@echo "  make ARCH=aarch64 LOG=debug"
	@echo "  make ARCH=riscv64 LOG=trace"
	@echo "  make ARCH=x86_64 ASSERT=off"
	@echo "  make ARCH=aarch64 LOG=debug ASSERT=panic"
	@echo "  make ARCH=riscv64 clean"

# 包含依赖文件
-include $(DEPS)
