# mk/config.mk — 基本参数 / 平台配置生成 / 日志标志 / 目录与默认目标
# 必须最先 include：后续片段都依赖这里生成的 platform.mk 与目录变量。

# ─── §1  基本参数 ─────────────────────────────────────────────────────────────
# 平台配置：平台唯一决定架构，正常构建只需要传 PLATFORM=<platform>。
# 兼容旧命令：如果只传 ARCH=<arch> 而不传 PLATFORM，则映射到 qemu-virt-<arch>。
ifeq ($(origin PLATFORM), undefined)
ifeq ($(origin ARCH), command line)
PLATFORM := qemu-virt-$(ARCH)
else
PLATFORM := qemu-virt-aarch64
endif
endif

# 日志级别配置
LOG ?= info

# QEMU vCPU 数量（用于 run / run-fs / test-*）
# 既传给 QEMU 的 -smp，也经 -DCONFIG_SMP_CPUS 传给内核。
# 默认 1：此时 cpu_bring_up_all() 直接返回，从核不启动，内核单核运行。
SMP ?= 1
ifeq ($(filter $(SMP),1 2 3 4 5 6 7 8),)
$(error Invalid SMP value '$(SMP)'. Use SMP=1..8)
endif

# guest 的 vCPU 数量（aarch64）。
#   GUEST_VCPUS=1：默认单 vCPU。
#   GUEST_VCPUS=2/4/8/12：选对应 CPU 数的 DTB（linux-gicv3-smpN.dtb），并让
#                  guest_loader 建 N 个 vCPU 任务。宿主 SMP 可以小于它
#                  （比如 SMP=2 + GUEST_VCPUS=8），vCPU 任务在宿主核上分时。
# 它改变 CFLAGS 和 rootfs 里的 DTB，纳入 _CFG_SIG 强制全量重编。
GUEST_VCPUS ?= 1
ifeq ($(filter $(GUEST_VCPUS),1 2 4 8 12),)
$(error Invalid GUEST_VCPUS '$(GUEST_VCPUS)'. Use 1, 2, 4, 8 or 12)
endif

# 帧指针（backtrace 用）：FP=1 加 -fno-omit-frame-pointer，FP=0 不加。
#
# panic / 内核异常时的调用栈靠帧指针链走出来（kernel/debug/backtrace.c）。
# 默认开着 —— 默认关的话 backtrace 一上来只能给启发式扫描的噪声，看着像坏
# 了一样；要压性能时 FP=0 一行关掉。
#
# 代价：占掉一个通用寄存器（x86_64 rbp / aarch64 x29 / riscv64 s0），
# 全内核大约 1-3% 性能损失。FP=0 时 backtrace 退化为栈扫描 —— 能出东西，
# 但会有误报、也可能漏帧。
FP ?= 1
ifeq ($(filter $(FP),0 1),)
$(error Invalid FP value '$(FP)'. Use FP=0 or FP=1)
endif

# 目录设置
LIB_DIR         := lib
BUILD_ROOT      := build
BUILD_DIR       := $(BUILD_ROOT)/$(PLATFORM)
THIRD_PARTY_BUILD_DIR := $(BUILD_DIR)/third_party
INCLUDE_DIR     := include
BOOT_DIR        := boot
KERNEL_DIR      := kernel
PLATFORM_DIR    := platforms
TESTS_DIR       := tests
UNIT_TEST_DIR   := $(TESTS_DIR)/unit-test
TOOLS_DIR       := tools
FS_DIR          := fs
THIRD_PARTY_DIR := third_party

# 宿主 Python：tools/ 下的生成脚本（gen_platform.py / gen_kallsyms.py）用它
PYTHON ?= python3

# ─── §2  平台配置生成 ────────────────────────────────────────────────────────────
# 平台配置全部在 platforms/$(PLATFORM)/platform.conf 的 platform 表中。
# gen_platform.py 解析该文件，生成 platform.mk 和 platform_static.c。

_PLATFORM_CONF      := $(PLATFORM_DIR)/$(PLATFORM)/platform.conf
_HAVE_PLATFORM_CONF := $(wildcard $(_PLATFORM_CONF))

ifeq ($(_HAVE_PLATFORM_CONF),)
$(error No platform.conf found for PLATFORM=$(PLATFORM). Expected: $(_PLATFORM_CONF))
endif

override ARCH := $(shell sed -n 's/^[[:space:]]*arch[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' $(_PLATFORM_CONF) | head -1)
ifeq ($(strip $(ARCH)),)
$(error Failed to infer ARCH from $(_PLATFORM_CONF))
endif

PLATFORM_MK  := $(BUILD_DIR)/platform.mk
$(shell python3 $(TOOLS_DIR)/gen_platform.py $(_PLATFORM_CONF) $(PLATFORM_MK) $(INCLUDE_DIR))
-include $(PLATFORM_MK)

# 平台配置依赖：凡是吃 PLATFORM_* / 内存布局 CFLAGS 的对象都挂到这两个文件上。
PLATFORM_CONFIG_DEPS := $(PLATFORM_MK) $(_PLATFORM_CONF)

ifeq ($(strip $(MEM_RAM_BASE)),)
$(error Failed to generate platform config from $(_PLATFORM_CONF))
endif

# ─── §3  日志标志 ────────────────────────────────────────────────────────────────
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

# ASSERT 开关已移除：断言恒为启用（见 include/libc/assert.h）。
# 显式传入时直接报错，而不是静默忽略——否则调用方会以为断言被关掉了。
ifneq ($(origin ASSERT),undefined)
    $(error ASSERT= is no longer supported; assertions are always enabled)
endif

MKDIR   := mkdir -p

# 顶层默认目标：早期 include 的 mk/*.mk 会定义目标，显式钉住 all（klog）。
.DEFAULT_GOAL := all
