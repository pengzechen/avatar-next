# Avatar OS — 顶层 Makefile
# 用法: make PLATFORM=<platform> [LOG=none|error|warn|info|debug|trace] [target]
# 快速参考: make help

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
TOOLS_DIR       := tools
FS_DIR          := fs
THIRD_PARTY_DIR := third_party

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

# ASSERT 开关已移除：断言恒为启用（见 include/assert.h）。
# 显式传入时直接报错，而不是静默忽略——否则调用方会以为断言被关掉了。
ifneq ($(origin ASSERT),undefined)
    $(error ASSERT= is no longer supported; assertions are always enabled)
endif

# ─── §4  源文件与目标文件变量 ────────────────────────────────────────────────────
# klog 库源文件
KLOG_SOURCES := $(LIB_DIR)/klog.c
VSNPRINTF_SOURCES := $(LIB_DIR)/vsnprintf.c
STRING_SOURCES := $(LIB_DIR)/string.c
BITMAP_SOURCES := $(LIB_DIR)/bitmap.c
PLATFORM_CFG_SOURCES := $(LIB_DIR)/platform_cfg.c
KLOG_OBJECT         := $(BUILD_DIR)/klog.o
VSNPRINTF_OBJECT    := $(BUILD_DIR)/vsnprintf.o
STRING_OBJECT       := $(BUILD_DIR)/string.o
BITMAP_OBJECT       := $(BUILD_DIR)/bitmap.o
LIBC_OBJECT         := $(BUILD_DIR)/libc.o
PLATFORM_CFG_OBJECT := $(BUILD_DIR)/platform_cfg.o
PLATFORM_STATIC_OBJECT := $(BUILD_DIR)/platform_static.o

# ── §4a  内核源文件自动发现 ────────────────────────────────────────────────────
# kernel/ 下所有 .c/.S 由 find 递归发现，不再逐文件列举——新增内核源文件
# 不需要改 Makefile。
#
# 排除项只有两类：
#   1. 其它架构的 mm/task/vmm 子目录（每个架构只选一套）
#   2. kernel/vmm/vdev/ —— 它按「文件」而非目录区分架构，见下方按 ARCH 的列表
#
# 注意 $(filter-out) 的模式里**只有第一个 '%' 是通配符**，后面的 '%' 按字面量
# 匹配。所以只能用「目录前缀字面量 + 单个尾部 %」的形式；
# 写成 $(KERNEL_DIR)/%/x86_64/% 会一条都过滤不掉（已实测）。
_KERNEL_ARCHES       := aarch64 riscv64 x86_64
_KERNEL_ARCH_MODULES := mm task vmm
_KERNEL_OTHER_ARCH   := $(filter-out $(ARCH),$(_KERNEL_ARCHES))

_KERNEL_EXCLUDE_DIRS := $(foreach a,$(_KERNEL_OTHER_ARCH),$(foreach m,$(_KERNEL_ARCH_MODULES),$(KERNEL_DIR)/$(m)/$(a)/)) \
                        $(KERNEL_DIR)/vmm/vdev/
_KERNEL_EXCLUDE_PAT  := $(addsuffix %,$(_KERNEL_EXCLUDE_DIRS))

# 注：$(shell ...) 的输出按空白切词，路径不能含空格（kernel/ 下当前没有）。
_KERNEL_FOUND := $(patsubst ./%,%,$(shell find $(KERNEL_DIR) -type f \( -name '*.c' -o -name '*.S' \) 2>/dev/null | LC_ALL=C sort))

# vdev 下按文件选架构；guest_loader.c 位于共享目录但只有 aarch64 编译它。
ifeq ($(ARCH),aarch64)
    _KERNEL_VDEV_SRCS     := $(KERNEL_DIR)/vmm/vdev/vpl011.c \
                             $(KERNEL_DIR)/vmm/vdev/irq_route.c \
                             $(wildcard $(KERNEL_DIR)/vmm/vdev/vgic/*.c)
    _KERNEL_ARCHONLY_SRCS := $(KERNEL_DIR)/vmm/guest_loader.c
    # guest_test.S: embedded guest program (linked into kernel binary)
    GUEST_TEST_OBJ        := $(BUILD_DIR)/apps_guest_test.o \
                             $(BUILD_DIR)/apps_el0_loop.o
else ifeq ($(ARCH),riscv64)
    _KERNEL_VDEV_SRCS     := $(KERNEL_DIR)/vmm/vdev/vuart16550.c \
                             $(KERNEL_DIR)/vmm/vdev/vplic.c
    _KERNEL_ARCHONLY_SRCS :=
    # RISC-V VS-mode guest test program (linked into kernel binary)
    GUEST_TEST_OBJ        := $(BUILD_DIR)/apps_riscv_guest_test.o
else ifeq ($(ARCH),x86_64)
    _KERNEL_VDEV_SRCS     := $(KERNEL_DIR)/vmm/vdev/vuart16550.c
    _KERNEL_ARCHONLY_SRCS :=
    # x86_64 guest test program (linked into kernel binary)
    GUEST_TEST_OBJ        := $(BUILD_DIR)/apps_x86_guest_test.o
else
    _KERNEL_VDEV_SRCS     :=
    _KERNEL_ARCHONLY_SRCS :=
    GUEST_TEST_OBJ        :=
endif

KERNEL_SRCS := $(filter-out $(_KERNEL_EXCLUDE_PAT) $(KERNEL_DIR)/vmm/guest_loader.c,$(_KERNEL_FOUND))
KERNEL_SRCS += $(_KERNEL_VDEV_SRCS) $(_KERNEL_ARCHONLY_SRCS)

# 源路径 → 扁平对象名：kernel/mm/pmm.c → $(BUILD_DIR)/kernel_mm_pmm.o
kobj = $(BUILD_DIR)/$(subst /,_,$(basename $(1))).o
KERNEL_OBJECTS := $(foreach f,$(KERNEL_SRCS),$(call kobj,$(f)))

# ── §4a-2  内核编译标志分组 ────────────────────────────────────────────────────
# kernel/ 的编译标志无法按目录推导（同一目录里有些文件引用第三方头文件、
# 有些没有），所以引用第三方头文件的文件在这里显式列出，其余一律用 CFLAGS。
# 这些文件会带 -w（屏蔽第三方头文件自身的警告），因此名单必须准确：
# 列入 = 该文件的警告被屏蔽，漏列 = 编译失败（include 找不到），两者都看得见。
KERNEL_LWEXT4_SRCS := \
    $(KERNEL_DIR)/fs/vfs/vfs.c \
    $(KERNEL_DIR)/loader/bin_loader.c \
    $(KERNEL_DIR)/loader/elf_loader.c \
    $(KERNEL_DIR)/syscall/syscall.c \
    $(KERNEL_DIR)/syscall/core/proc_lifecycle.c \
    $(KERNEL_DIR)/syscall/fs/fd_pool.c \
    $(KERNEL_DIR)/syscall/fs/path.c \
    $(KERNEL_DIR)/syscall/fs/file_io.c \
    $(KERNEL_DIR)/syscall/fs/file_ops.c \
    $(KERNEL_DIR)/syscall/fs/file_stat.c \
    $(KERNEL_DIR)/syscall/fs/dir.c \
    $(KERNEL_DIR)/syscall/fs/ioctl.c \
    $(KERNEL_DIR)/syscall/fs/pipe.c \
    $(KERNEL_DIR)/syscall/fs/pty.c \
    $(KERNEL_DIR)/syscall/io/poll.c \
    $(KERNEL_DIR)/syscall/io/select.c \
    $(KERNEL_DIR)/syscall/io/epoll.c \
    $(KERNEL_DIR)/syscall/mm/mmap.c

# guest_loader.c 位于共享目录但仅 aarch64 编译（见 §4a 白名单），且引用 lwext4。
ifeq ($(ARCH),aarch64)
KERNEL_LWEXT4_SRCS += $(KERNEL_DIR)/vmm/guest_loader.c
endif

# kernel/net/** 全部引用 lwIP，按目录自动派生，将来新增文件不会漏。
KERNEL_LWIP_SRCS := $(filter $(KERNEL_DIR)/net/%,$(KERNEL_SRCS))

# 这两个同时引用 lwIP 与 lwext4：
# ksocket.c → syscall/fs/fd_pool.h → include/vfs.h:14 → #include <ext4.h>
KERNEL_LWIPX_SRCS := $(KERNEL_DIR)/syscall/net/ksocket.c \
                     $(KERNEL_DIR)/syscall/net/sock_syscall.c

KERNEL_PLAIN_SRCS := $(filter-out $(KERNEL_LWEXT4_SRCS) $(KERNEL_LWIP_SRCS) $(KERNEL_LWIPX_SRCS),$(KERNEL_SRCS))

# 分组完整性断言：漏一个或重一个都当场报错，而不是静默少编一个文件。
ifneq ($(words $(KERNEL_SRCS)),$(words $(KERNEL_PLAIN_SRCS) $(KERNEL_LWEXT4_SRCS) $(KERNEL_LWIP_SRCS) $(KERNEL_LWIPX_SRCS)))
$(error kernel source grouping mismatch: $(words $(KERNEL_SRCS)) sources vs $(words $(KERNEL_PLAIN_SRCS) $(KERNEL_LWEXT4_SRCS) $(KERNEL_LWIP_SRCS) $(KERNEL_LWIPX_SRCS)) grouped. Check KERNEL_LWEXT4_SRCS / KERNEL_LWIP*_SRCS)
endif
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
TESTS_SOURCES := $(filter-out $(TESTS_DIR)/pmm_test.c,$(wildcard $(TESTS_DIR)/*.c))
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

# ── §4b  平台 / 驱动基础源文件 ──────────────────────────────────────────────────
PLATFORM_SOURCES := $(PLATFORM_DIR)/$(PLATFORM)/platform.c
ifneq ($(filter qemu-virt-% rk3588-aarch64 sg2002-riscv64,$(PLATFORM)),)
    PLATFORM_SOURCES += $(PLATFORM_DIR)/qemu/qemu_platform.c
endif
PLATFORM_OBJECTS := $(PLATFORM_SOURCES:$(PLATFORM_DIR)/%.c=$(BUILD_DIR)/platform_%.o)

ifeq ($(wildcard $(PLATFORM_SOURCES)),)
$(error Missing platform source: $(PLATFORM_SOURCES))
endif

# 驱动基础源文件（来自 platform.mk，后续由 §6 选择块覆盖/追加）
DRIVER_UART_SRC  := $(DEV_UART_SRC)
DRIVER_IRQ_SRC   := $(DEV_IRQ_SRC)
DRIVER_TIMER_SRC := $(DEV_TIMER_SRC)
# 注意：DRIVER_OBJECTS 在 §6 驱动选择块之后统一组装

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

# ─── §5  工具链与编译标志（按架构）─────────────────────────────────────────────
X86_64_TOOL_PREFIX ?= $(if $(shell command -v x86_64-linux-musl-gcc 2>/dev/null),x86_64-linux-musl-,)

ifeq ($(ARCH),x86_64)
    CC      := $(X86_64_TOOL_PREFIX)gcc
    AR      := $(X86_64_TOOL_PREFIX)ar
    OBJCOPY := $(X86_64_TOOL_PREFIX)objcopy
    NM      := $(X86_64_TOOL_PREFIX)nm
    CFLAGS  := -Wall -Wextra -O2 -g
	CFLAGS  += -DARCH_X86_64=1
    CFLAGS  += -I$(INCLUDE_DIR)
    CFLAGS  += -I$(INCLUDE_DIR)/x86_64
    CFLAGS  += -I$(BOOT_DIR)/common
    CFLAGS  += $(LOG_DEFINE)
    CFLAGS  += -fno-pie -fno-stack-protector -fno-stack-clash-protection -U_FORTIFY_SOURCE
    CFLAGS  += -ffreestanding -fno-builtin
    CFLAGS  += -mcmodel=large -mno-red-zone
    CFLAGS  += -mno-mmx -mno-sse
    LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -no-pie
    LDFLAGS += -Wl,-z,max-page-size=0x1000
    KLOG_TARGET := $(BUILD_DIR)/libklog_x86_64.a
    KERNEL_TARGET := $(BUILD_DIR)/kernel_x86_64.elf
    KERNEL_BIN    := $(BUILD_DIR)/kernel_x86_64.bin
    KERNEL_IMAGE  := $(BUILD_DIR)/kernel_x86_64.img
    QEMU          := qemu-system-x86_64
    QEMU_FLAGS    := -machine q35 -enable-kvm -cpu host -smp $(SMP) -m 2G -nographic -kernel $(KERNEL_BIN)
    QEMU_NET_FLAGS ?= -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56
    QEMU_RUN_NET_FLAGS ?= -machine microvm -enable-kvm -cpu host -smp $(SMP) -m 2G -nographic -kernel $(KERNEL_BIN)
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
    CFLAGS  += -fno-pie
    # 允许 GCC 生成 FP/SIMD（NEON）指令（riscv64 靠 -march=rv64gc -mabi=lp64d 达
    # 到同样效果）。代价是内核随时可能占用 v0-v31，所以必须有两层保存，缺一不可：
    #   boot/aarch64/exception.S     陷阱帧保存 q0-q31 + FPCR/FPSR（保护 EL0 用户态）
    #   kernel/task/aarch64/switch.S 任务切换保存 d8-d15 + FPCR/FPSR（AAPCS64 被调用者保存）
    # 限制：内核里的浮点只能用 float/double —— 未链接 libgcc，long double（128 位）
    # 的运算会引出 __addtf3/__divtf3 等帮助函数，在链接期报未定义符号。
    # 详见 docs/arch/aarch64/FP_SIMD_CONTEXT.md
    CFLAGS  += -mno-outline-atomics  # freestanding：禁止 GCC outline atomics 调用 libgcc 帮助函数
    CFLAGS  += -ffreestanding -fno-builtin  # 禁用内置函数和标准库
    KLOG_TARGET := $(BUILD_DIR)/libklog_aarch64.a
    KERNEL_TARGET := $(BUILD_DIR)/kernel_aarch64.elf
    KERNEL_BIN    := $(BUILD_DIR)/kernel_aarch64.bin
    KERNEL_LINK_ADDR ?= 0xffff000040080000
    LDFLAGS += -Wl,--defsym=KERNEL_LINK_ADDR=$(KERNEL_LINK_ADDR)
    QEMU          := qemu-system-aarch64
    QEMU_FLAGS    := -cpu cortex-a76 -M virt,virtualization=on -smp $(SMP) -m 2G -nographic -kernel $(KERNEL_BIN)
    QEMU_NET_FLAGS ?= -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56
else ifeq ($(ARCH),riscv64)
    CC      := riscv64-linux-musl-gcc
    AR      := riscv64-linux-musl-ar
    OBJCOPY := riscv64-linux-musl-objcopy
    NM      := riscv64-linux-musl-nm
    CFLAGS  := -Wall -Wextra -O2 -g
    CFLAGS  += -march=rv64gc -mabi=lp64d
	CFLAGS  += -DARCH_RISCV64=1
    CFLAGS  += -I$(INCLUDE_DIR)
    CFLAGS  += -I$(INCLUDE_DIR)/riscv64
    CFLAGS  += -I$(BOOT_DIR)/common
    CFLAGS  += $(LOG_DEFINE)
    CFLAGS  += -mcmodel=medany
    CFLAGS  += -fno-pic -fno-pie
    CFLAGS  += -ffreestanding -fno-builtin
    # -no-pie：让链接器输出非 PIE 静态可执行文件，避免生成
    # R_RISCV_RELATIVE 重定位条目（裸机内核无动态链接器处理它们）
    LDFLAGS := -no-pie
    KLOG_TARGET := $(BUILD_DIR)/libklog_riscv64.a
    KERNEL_TARGET := $(BUILD_DIR)/kernel_riscv64.elf
    KERNEL_BIN    := $(BUILD_DIR)/kernel_riscv64.bin
    QEMU          := qemu-system-riscv64
    QEMU_FLAGS    := -M virt -smp $(SMP) -m 2G -nographic -bios default -kernel $(KERNEL_BIN)
    QEMU_NET_FLAGS ?= -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56
else
    $(error Unsupported architecture: $(ARCH). Use ARCH=x86_64, aarch64 or riscv64)
endif

# ── §5a  通用编译标志（所有架构共享，追加在架构特定 CFLAGS 之后）────────────────
CFLAGS  += -nostdinc
CFLAGS  += -I$(INCLUDE_DIR)/libc
CFLAGS  += -Idriver
CFLAGS  += -Ikernel
CFLAGS  += -Ikernel/mm
CFLAGS  += -Iinclude/net
CFLAGS  += -DAVATAR_HAS_FILESYSTEM
CFLAGS  += -DCONFIG_SMP_CPUS=$(SMP)
CFLAGS  += -DPLATFORM_$(MEM_PLATFORM_DEFINE)=1
CFLAGS  += -DPLATFORM_MEM_RAM_BASE=$(MEM_RAM_BASE)
CFLAGS  += -DPLATFORM_MEM_RAM_SIZE=$(MEM_RAM_SIZE)
CFLAGS  += -DPLATFORM_MEM_ROOTFS_BASE=$(MEM_ROOTFS_BASE)
CFLAGS  += -DPLATFORM_MEM_ROOTFS_SIZE=$(MEM_ROOTFS_SIZE)
CFLAGS  += -DDEVICE_MMIO_NEEDS_VMA=$(DEV_MMIO_NEEDS_VMA)
CFLAGS  += -DDEVICE_UART_BASE_RAW=$(DEV_UART_BASE_RAW)
CFLAGS  += -DDEVICE_UART_REG_SHIFT=$(DEV_UART_REG_SHIFT)
CFLAGS  += -DDEVICE_ETH_BASE_RAW=$(DEV_ETH_BASE)

# ─── §6  驱动选择 ────────────────────────────────────────────────────────────────
# 所有驱动默认值来自 platform.conf → gen_platform.py → platform.mk。
# 可在命令行覆盖，例如：make PLATFORM=sg2002-riscv64 ETH=none kernel

# ── §6a  基础设备驱动（UART / GIC / 定时器）──────────────────────────────────
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

# ── §6b  加速器驱动（NPU / TPU）───────────────────────────────────────────────
# NPU 驱动选择
# 用法：make PLATFORM=rk3588-aarch64 NPU=rknpu kernel
NPU ?= $(DEV_NPU_TYPE)
ifeq ($(NPU),rknpu)
    DRIVER_NPU_SRCS := driver/npu/rknpu.c driver/npu/rkpm.c
    CFLAGS          += -DDRIVER_NPU_RKNPU=1
else
    DRIVER_NPU_SRCS :=
endif

TPU ?= $(DEV_TPU_TYPE)
ifeq ($(TPU),cvitpu)
    DRIVER_TPU_SRCS := driver/tpu/cvi_tpu.c
    CFLAGS          += -DDRIVER_TPU_CVITPU=1
else
    DRIVER_TPU_SRCS :=
endif

# ── §6c  网络驱动（ETH）─────────────────────────────────────────────────────────
# 以太网驱动（由 platform.conf 的 eth.driver 自动推导；也可命令行覆盖：ETH=none / ETH=virtio）
ETH ?= $(DEV_ETH_TYPE)
ifeq ($(ETH),virtio)
    ifeq ($(filter $(ARCH),riscv64 aarch64 x86_64),)
        $(error ETH=virtio currently supports ARCH=riscv64, ARCH=aarch64 or ARCH=x86_64)
    endif
    CFLAGS          += -DDRIVER_ETH_VIRTIO=1
    DRIVER_ETH_SRCS := driver/eth/virtio_net.c
else ifeq ($(ETH),none)
    DRIVER_ETH_SRCS :=
else ifeq ($(ETH),)
    DRIVER_ETH_SRCS :=
else
    $(error Invalid ETH. Use: none or virtio)
endif

# ── §6e  DRIVER_OBJECTS 最终组装 ────────────────────────────────────────────────
# 在所有驱动选择块执行完毕后，按「源文件 → $(BUILD_DIR)/driver/<文件名>.o」统一
# 映射。原先三套命名混用（drv_uart/x.o、gicv2.o、drv_npu_x.o），现在只有一个
# 平铺的 driver/ 目录。driver/ 下各文件名互不重复，展平不会碰撞。
#
# 注意 pattern rule 做不到展平：GNU Make 的 '%' 会匹配 '/'，
# $(BUILD_DIR)/driver/%.o: driver/%.c 会把 driver/irq/gicv2.c 映射成
# driver/irq/gicv2.o（子目录被原样带过去）。规则由 §11c 用 $(eval) 逐文件生成。
_drv_obj = $(BUILD_DIR)/driver/$(notdir $(basename $(1))).o

# ── §6e-1  辅助驱动（ION / SDMMC）──────────────────────────────────────────────
# Ion 内存分配器（当 TPU=cvitpu 时自动启用；也可独立启用 ION=1）
ION ?= $(if $(filter cvitpu,$(TPU)),1,0)
DRIVER_ION_SRCS :=
ifeq ($(ION),1)
    CFLAGS           += -DDRIVER_ION=1
    DRIVER_ION_SRCS  := driver/ion/ion.c
endif

# ramblk 始终构建（rootfs 的 ramblk0 块设备）；SG2002 真机另加 SD 卡块设备。
DRIVER_BLK_SRCS := driver/blk/ramblk.c
ifeq ($(PLATFORM),sg2002-riscv64)
    CFLAGS              += -DDRIVER_SDBLK_SG2002=1
    DRIVER_BLK_SRCS     += driver/blk/sdblk.c
endif

# ── §6e-2  按编译标志分组（各组标志与重组前逐字一致）─────────────────────────
DRIVER_SRCS_CFLAGS := $(DRIVER_UART_SRC) $(DRIVER_IRQ_SRC) $(DRIVER_TIMER_SRC) \
                      $(DRIVER_NPU_SRCS) $(DRIVER_ETH_SRCS) \
                      $(if $(filter 1,$(DEV_NEED_LAPIC)),driver/irq/lapic.c)
DRIVER_SRCS_PLIC   := $(if $(filter riscv64,$(ARCH)),driver/irq/plic.c)
DRIVER_SRCS_TPU    := $(DRIVER_TPU_SRCS)
DRIVER_SRCS_ION    := $(DRIVER_ION_SRCS)
DRIVER_SRCS_BLK    := $(DRIVER_BLK_SRCS)

DRIVER_SRCS := $(strip $(DRIVER_SRCS_CFLAGS) $(DRIVER_SRCS_PLIC) $(DRIVER_SRCS_TPU) \
                       $(DRIVER_SRCS_ION) $(DRIVER_SRCS_BLK))
DRIVER_OBJECTS := $(foreach f,$(DRIVER_SRCS),$(call _drv_obj,$(f)))

CFLAGS  += -MMD -MP

# ─── §7  构建变体 ─────────────────────────────────────────────────────────────
# 三个变体互斥，优先级 GUEST_LINUX > VMM_TEST > NGINX_TEST > normal：
#   VMM_TEST=1    ：定义 RUN_VMM_TEST，跳过 busybox，运行三线程切换测试
#   GUEST_LINUX=1 ：定义 RUN_GUEST_LINUX，从 rootfs 加载并启动 Linux guest
#   NGINX_TEST=1  ：定义 RUN_NGINX_TEST，网络栈专供 nginx 压测（无对应 target）
# 新增测试时仿照此模式，同时在 _BUILD_VARIANT 里加一个唯一标识。
VMM_TEST ?= 0
NGINX_TEST ?= 0
# GUEST_LINUX=1：编译 RUN_GUEST_LINUX，从 rootfs 加载并启动 Linux guest
GUEST_LINUX ?= 0
ifeq ($(GUEST_LINUX),1)
    CFLAGS += -DRUN_GUEST_LINUX=1
    _BUILD_VARIANT := guest_linux
else ifeq ($(VMM_TEST),1)
    CFLAGS += -DRUN_VMM_TEST=1
    _BUILD_VARIANT := vmm_test
else ifeq ($(NGINX_TEST),1)
    CFLAGS += -DRUN_NGINX_TEST=1
    _BUILD_VARIANT := nginx_test
else
    _BUILD_VARIANT := normal
endif

# 当变体改变时自动清除 kernel/main.o，防止复用缓存了错误条件编译的对象文件。
_VARIANT_FILE := $(BUILD_DIR)/.build_variant
_VARIANT_CHECK := $(shell \
    mkdir -p $(BUILD_DIR) 2>/dev/null; \
    if [ "$$(cat $(_VARIANT_FILE) 2>/dev/null)" != "$(_BUILD_VARIANT)" ]; then \
        rm -f $(BUILD_DIR)/kernel_main.o; \
        printf '%s' '$(_BUILD_VARIANT)' > $(_VARIANT_FILE); \
    fi)

MKDIR   := mkdir -p

# ─── §8  第三方库：lwext4 文件系统 ──────────────────────────────────────────────
LWEXT4_DIR      := $(THIRD_PARTY_DIR)/lwext4
LWEXT4_PORT_DIR := $(FS_DIR)/lwext4_port

# ─── §8a  内核网络栈：netdev + lwIP ─────────────────────────────────────────────
LWIP_DIR      := $(THIRD_PARTY_DIR)/lwip
LWIP_PORT_DIR := $(KERNEL_DIR)/net/lwip_port


LWIP_CORE_SRCS := $(LWIP_DIR)/src/core/init.c \
                  $(LWIP_DIR)/src/core/def.c \
                  $(LWIP_DIR)/src/core/dns.c \
                  $(LWIP_DIR)/src/core/inet_chksum.c \
                  $(LWIP_DIR)/src/core/ip.c \
                  $(LWIP_DIR)/src/core/mem.c \
                  $(LWIP_DIR)/src/core/memp.c \
                  $(LWIP_DIR)/src/core/netif.c \
                  $(LWIP_DIR)/src/core/pbuf.c \
                  $(LWIP_DIR)/src/core/raw.c \
                  $(LWIP_DIR)/src/core/stats.c \
                  $(LWIP_DIR)/src/core/sys.c \
                  $(LWIP_DIR)/src/core/tcp.c \
                  $(LWIP_DIR)/src/core/tcp_in.c \
                  $(LWIP_DIR)/src/core/tcp_out.c \
                  $(LWIP_DIR)/src/core/timeouts.c \
                  $(LWIP_DIR)/src/core/udp.c \
                  $(LWIP_DIR)/src/core/ipv4/etharp.c \
                  $(LWIP_DIR)/src/core/ipv4/icmp.c \
                  $(LWIP_DIR)/src/core/ipv4/ip4.c \
                  $(LWIP_DIR)/src/core/ipv4/ip4_addr.c \
                  $(LWIP_DIR)/src/core/ipv4/ip4_frag.c \
                  $(LWIP_DIR)/src/netif/ethernet.c
LWIP_OBJS := $(patsubst $(LWIP_DIR)/src/core/%.c,$(THIRD_PARTY_BUILD_DIR)/lwip_core_%.o,$(filter $(LWIP_DIR)/src/core/%.c,$(filter-out $(LWIP_DIR)/src/core/ipv4/%,$(LWIP_CORE_SRCS))))
LWIP_OBJS += $(patsubst $(LWIP_DIR)/src/core/ipv4/%.c,$(THIRD_PARTY_BUILD_DIR)/lwip_ipv4_%.o,$(filter $(LWIP_DIR)/src/core/ipv4/%.c,$(LWIP_CORE_SRCS)))
LWIP_OBJS += $(patsubst $(LWIP_DIR)/src/netif/%.c,$(THIRD_PARTY_BUILD_DIR)/lwip_netif_%.o,$(filter $(LWIP_DIR)/src/netif/%.c,$(LWIP_CORE_SRCS)))

LWIP_CFLAGS := $(CFLAGS)
LWIP_CFLAGS += -I$(LWIP_DIR)/src/include
LWIP_CFLAGS += -I$(LWIP_PORT_DIR)
LWIP_CFLAGS += -I$(LWIP_PORT_DIR)/arch
LWIP_CFLAGS += -DLWIP_NO_CTYPE_H=1
LWIP_CFLAGS += -w

# lwext4 库源文件（第三方代码）
LWEXT4_SRCS     := $(wildcard $(LWEXT4_DIR)/src/*.c)
LWEXT4_OBJS     := $(patsubst $(LWEXT4_DIR)/src/%.c,$(THIRD_PARTY_BUILD_DIR)/lwext4_%.o,$(LWEXT4_SRCS))

# lwext4 移植胶水代码（属于本项目，使用 LWEXT4_CFLAGS）
# 注：driver/blk/ramblk.c 也是同一组标志，但它是驱动，归 §6e 的 DRIVER_OBJECTS。
LWEXT4_PORT_OBJS := $(BUILD_DIR)/lwext4_port_kmalloc.o \
                   $(BUILD_DIR)/lwext4_port_fs_init.o

# lwext4 专用编译标志（在通用 CFLAGS 基础上添加）
LWEXT4_CFLAGS  := $(CFLAGS)
LWEXT4_CFLAGS  += -I$(LWEXT4_DIR)/include   # lwext4 头文件
LWEXT4_CFLAGS  += -I$(LWEXT4_PORT_DIR)      # generated/ext4_config.h 所在目录
LWEXT4_CFLAGS  += -DCONFIG_USE_DEFAULT_CFG=0  # 使用自定义 ext4_config.h
# 以下定义与 generated/ext4_config.h 保持一致，防止默认值覆盖
LWEXT4_CFLAGS  += -DCONFIG_HAVE_OWN_ERRNO=1
LWEXT4_CFLAGS  += -DCONFIG_HAVE_OWN_OFLAGS=1
LWEXT4_CFLAGS  += -DCONFIG_DEBUG_PRINTF=0
LWEXT4_CFLAGS  += -DCONFIG_DEBUG_ASSERT=0
LWEXT4_CFLAGS  += -DCONFIG_HAVE_OWN_ASSERT=1
LWEXT4_CFLAGS  += -DCONFIG_USE_USER_MALLOC=1
LWEXT4_CFLAGS  += -w   # 屏蔽第三方代码警告

# ─── §9  Rootfs 配置 ────────────────────────────────────────────────────────────
# 每个平台独立一个 build/<platform>/ 目录，切换平台无需 make clean。
ROOTFS_IMG       := $(BUILD_DIR)/rootfs-$(ARCH).img
ROOTFS_STAGE     := $(BUILD_DIR)/rootfs-stage-$(ARCH)
LTP_BIN_DIR      := tests/ltp/bin/$(ARCH)
LTP_BINS         := $(wildcard $(LTP_BIN_DIR)/*)
ifneq ($(filter $(ARCH),aarch64 riscv64),)
EPOLL_PERF_CC    := $(ARCH)-linux-musl-gcc
EPOLL_PERF_BIN   := apps/epoll_perf-$(ARCH)
else
EPOLL_PERF_CC    :=
EPOLL_PERF_BIN   :=
endif
NGINX_BIN        := $(wildcard apps/nginx-$(ARCH))
ifeq ($(ARCH),aarch64)
GUEST_LINUX_FILES := imgs/aarch64/linux.bin imgs/aarch64/linux.dtb imgs/aarch64/initrd.gz
else
GUEST_LINUX_FILES :=
endif
# ROOTFS_SIZE_MB / ROOTFS_PHYS_ADDR 来自自动生成的 $(MEM_LAYOUT_MK)
QEMU_ROOTFS_FLAGS = -device loader,file=$(ROOTFS_IMG),addr=$(ROOTFS_PHYS_ADDR),force-raw=on

# ─── §10  顶层目标声明 ───────────────────────────────────────────────────────────
.PHONY: all clean clean-all help klog kernel run run-net rootfs run-fs test-pthread test-mutex test-vmm test-guest-linux test-ltp epoll-perf test-epoll-perf

all: klog

kernel: | kernel_clean
kernel: $(KERNEL_BIN)

# 平台隔离后不再需要跨架构自动清理；保留空目标兼容 kernel 的 order-only 依赖。
.PHONY: kernel_clean
kernel_clean:

klog: $(KLOG_TARGET)

$(BUILD_DIR):
	$(MKDIR) $(BUILD_DIR)


$(KLOG_TARGET): $(KLOG_OBJECT) | $(BUILD_DIR)
	$(AR) rcs $@ $^

# ─── §11  构建规则 ───────────────────────────────────────────────────────────────

# ── §11a  库与通用规则 ───────────────────────────────────────────────────────
$(BUILD_DIR)/klog.o: $(LIB_DIR)/klog.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vsnprintf.o: $(LIB_DIR)/vsnprintf.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/string.o: $(LIB_DIR)/string.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc.o: $(LIB_DIR)/libc.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ── §11b  内核 / 测试 / 平台 / 启动规则 ────────────────────────────────────────

# 内核源文件的编译规则由 §4a 发现出的列表批量生成，不再逐文件手写。
# 标志参数必须写成 $$(...)：LWEXT4_CFLAGS / LWIP_CFLAGS 定义在 §8，晚于 §4，
# 用单个 $ 会在本行展开时冻结成空字符串。
define KERNEL_RULE
$(call kobj,$(1)): $(1) | $(BUILD_DIR)
	$$(CC) $(2) -c $$< -o $$@
endef

$(foreach f,$(KERNEL_PLAIN_SRCS),$(eval $(call KERNEL_RULE,$(f),$$(CFLAGS))))
$(foreach f,$(KERNEL_LWEXT4_SRCS),$(eval $(call KERNEL_RULE,$(f),$$(LWEXT4_CFLAGS))))
$(foreach f,$(KERNEL_LWIP_SRCS),$(eval $(call KERNEL_RULE,$(f),$$(LWIP_CFLAGS))))
$(foreach f,$(KERNEL_LWIPX_SRCS),$(eval $(call KERNEL_RULE,$(f),$$(LWIP_CFLAGS) -I$$(LWEXT4_DIR)/include -I$$(LWEXT4_PORT_DIR))))

$(BUILD_DIR)/tests_%.o: $(TESTS_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/platform_%.o: $(PLATFORM_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/boot_%.o: $(BOOT_DIR)/$(ARCH)/%.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ── §11c  启动 / 异常 / 驱动规则 ────────────────────────────────────────────────

# 驱动源文件的编译规则由 §6e 分好的组批量生成，统一产出到 $(BUILD_DIR)/driver/。
# 标志参数必须写成 $$(...)：LWEXT4_CFLAGS 定义在 §8，晚于 §6e。
$(BUILD_DIR)/driver:
	$(MKDIR) $@

define DRIVER_RULE
$(call _drv_obj,$(1)): $(1) | $(BUILD_DIR)/driver
	$$(CC) $(2) -c $$< -o $$@
endef

$(foreach f,$(DRIVER_SRCS_CFLAGS),$(eval $(call DRIVER_RULE,$(f),$$(CFLAGS))))
$(foreach f,$(DRIVER_SRCS_PLIC),  $(eval $(call DRIVER_RULE,$(f),$$(CFLAGS) -Idriver)))
$(foreach f,$(DRIVER_SRCS_TPU),   $(eval $(call DRIVER_RULE,$(f),$$(CFLAGS) -Idriver/tpu)))
$(foreach f,$(DRIVER_SRCS_ION),   $(eval $(call DRIVER_RULE,$(f),$$(CFLAGS) -Idriver/ion -Ikernel/mm)))
$(foreach f,$(DRIVER_SRCS_BLK),   $(eval $(call DRIVER_RULE,$(f),$$(LWEXT4_CFLAGS) -Idriver)))

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

# ── §11c-1  平台配置依赖 ───────────────────────────────────────────────────────
# 内核对象多数用平台派生的 CFLAGS 编译（PLATFORM_* / DRIVER_* / DEVICE_* / 内存布局）。
# platform.mk 只在内容变化时才被 gen_platform.py 重写，所以把对象挂到它上面
# 可以精确捕捉「真的换了平台」，而不会每次 make 都全量重编。
PLATFORM_CONFIG_DEPS := $(PLATFORM_MK) $(_PLATFORM_CONF)
$(BOOT_OBJECTS) $(KERNEL_OBJECTS) \
$(PMM_TEST_OBJECT) \
$(TASK_USER_TEST_OBJ) $(TASK_USER_HELLO_OBJ) $(TASK_USER_TESTEXECVE_OBJ) \
$(GUEST_TEST_OBJ) $(TESTS_OBJECTS) $(PLATFORM_OBJECTS) $(DRIVER_OBJECTS) \
$(EXCEPTION_OBJECTS) $(KLOG_OBJECT) $(VSNPRINTF_OBJECT) $(STRING_OBJECT) \
$(LIBC_OBJECT) $(BITMAP_OBJECT) $(PLATFORM_CFG_OBJECT) $(PLATFORM_STATIC_OBJECT) $(LWEXT4_OBJS) $(LWEXT4_PORT_OBJS) \
$(LWIP_OBJS): $(PLATFORM_CONFIG_DEPS)


# 内嵌用户程序：直接链接进内核镜像（与下面「从文件系统加载」的 apps/ 不同）
$(BUILD_DIR)/user_test.o: $(TASK_USER_TEST_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hello.o: $(TASK_USER_HELLO_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# test_execve（仅 x86_64）
ifeq ($(ARCH),x86_64)
$(BUILD_DIR)/test_execve.o: apps/x86_64/test_execve.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

# 用户应用程序：汇编为 .bin / .bin.elf，由 rootfs 装载（非内嵌）
$(BUILD_DIR)/apps_%.o: $(APPS_DIR)/%.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DAPP_ELF=1 -c $< -o $@

# 这些 .o 只经由模式规则（上面的 apps_%.o 与下面的 %.bin）产生，从未在
# Makefile 里被显式指名，因此 make 会把它们判为 intermediate 并在构建结束时
# 自动删除（终端上会看到一行 "rm build/.../apps_hello.o ..."）。
# 标记为 secondary 即可保留它们，避免每次重建 rootfs 都重新编译。
# 注意：apps_guest_test.o / apps_el0_loop.o 有显式规则，本来就不会被删。
# 加 ifneq 是因为裸 ".SECONDARY:"（无前置条件）含义是"所有目标都不删"，
# 若某架构 apps/ 下没有 .S，会意外变成全局语义。
ifneq ($(APPS_OBJECTS),)
.SECONDARY: $(APPS_OBJECTS)
endif

# 生成应用程序二进制文件
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/apps_%.o $(APPS_LD) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -nostdlib -nostartfiles -nodefaultlibs -T $(APPS_LD) -o $@.elf $<
	$(OBJCOPY) -O binary $@.elf $@
	@echo "App binary created: $@"
	@echo "  Entry point: $(shell $(NM) $@.elf 2>/dev/null | grep ' _start')"

# C 用户程序编译规则（当前各架构目录暂无 .c 应用；保留直接静态链接形式）
$(BUILD_DIR)/%.elf: $(APPS_DIR)/%.c $(APPS_LD) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/apps_$*.o
	$(CC) $(CFLAGS) -static -nostdlib -nostartfiles -nodefaultlibs \
		-T $(APPS_LD) \
		$(BUILD_DIR)/apps_$*.o \
		-o $@
	@echo "C app ELF created: $@"

ifneq ($(EPOLL_PERF_BIN),)
epoll-perf: $(EPOLL_PERF_BIN)

$(EPOLL_PERF_BIN): apps/c/epoll_perf.c
	$(EPOLL_PERF_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "epoll perf app created: $@"
else
epoll-perf:
	@echo "epoll_perf is only wired for ARCH=aarch64 or ARCH=riscv64"
	@exit 1
endif

# ── §11d  测试 / 库对象规则 ────────────────────────────────────────────────────

# PMM 测试（唯一来自 tests/ 却参与内核链接的源文件，不在 §4a 的 find 范围内）
PMM_TEST_OBJECT := $(BUILD_DIR)/kernel_mm_pmm_test.o

$(BUILD_DIR)/kernel_mm_pmm_test.o: $(TESTS_DIR)/pmm_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/bitmap.o: $(LIB_DIR)/bitmap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/platform_cfg.o: $(LIB_DIR)/platform_cfg.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/platform_static.c: $(_PLATFORM_CONF) $(TOOLS_DIR)/gen_platform.py | $(BUILD_DIR)
	python3 $(TOOLS_DIR)/gen_platform.py $(_PLATFORM_CONF) $(PLATFORM_MK) $(INCLUDE_DIR)

$(BUILD_DIR)/platform_static.o: $(BUILD_DIR)/platform_static.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@


# ── §11e  第三方库编译规则（lwext4 / lwIP）──────────────────────────────────────
# lwext4 第三方源码：用带 compat 路径的专用 LWEXT4_CFLAGS

$(THIRD_PARTY_BUILD_DIR)/lwext4_%.o: $(LWEXT4_DIR)/src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(THIRD_PARTY_BUILD_DIR)/lwip_core_%.o: $(LWIP_DIR)/src/core/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

$(THIRD_PARTY_BUILD_DIR)/lwip_ipv4_%.o: $(LWIP_DIR)/src/core/ipv4/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

$(THIRD_PARTY_BUILD_DIR)/lwip_netif_%.o: $(LWIP_DIR)/src/netif/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# lwext4 移植胶水：属于本项目，用普通 CFLAGS
$(BUILD_DIR)/lwext4_port_kmalloc.o: $(LWEXT4_PORT_DIR)/kmalloc.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# FS 初始化需要 lwext4 头文件，用 LWEXT4_CFLAGS
$(BUILD_DIR)/lwext4_port_fs_init.o: $(LWEXT4_PORT_DIR)/fs_init.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -I$(LWEXT4_PORT_DIR) -c $< -o $@


# ── §11f  内嵌 guest 测试程序（链接进内核镜像）─────────────────────────────────
ifeq ($(ARCH),aarch64)


$(BUILD_DIR)/apps_guest_test.o: apps/aarch64/guest_test.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/apps_el0_loop.o: apps/aarch64/el0_loop.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

ifeq ($(ARCH),x86_64)


$(BUILD_DIR)/apps_x86_guest_test.o: apps/x86_64/guest_test.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

ifeq ($(ARCH),riscv64)


$(BUILD_DIR)/apps_riscv_guest_test.o: apps/riscv64/guest_test.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

# ── §11g  链接 ────────────────────────────────────────────────────────────────────
$(KERNEL_TARGET): $(BOOT_OBJECTS) $(KERNEL_OBJECTS) $(PMM_TEST_OBJECT) $(LWIP_OBJS) $(TASK_USER_TEST_OBJ) $(TASK_USER_HELLO_OBJ) $(TASK_USER_TESTEXECVE_OBJ) $(GUEST_TEST_OBJ) $(TESTS_OBJECTS) $(PLATFORM_OBJECTS) $(DRIVER_OBJECTS) $(EXCEPTION_OBJECTS) $(KLOG_OBJECT) $(VSNPRINTF_OBJECT) $(STRING_OBJECT) $(LIBC_OBJECT) $(BITMAP_OBJECT) $(PLATFORM_CFG_OBJECT) $(PLATFORM_STATIC_OBJECT) $(LWEXT4_OBJS) $(LWEXT4_PORT_OBJS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -nostartfiles -nodefaultlibs -T $(BOOT_DIR)/$(ARCH)/link.ld -o $@ -Wl,--start-group $^ -Wl,--end-group

# 转换为二进制文件
$(KERNEL_BIN): $(KERNEL_TARGET)
	$(OBJCOPY) -O binary $< $@

# 1.44MB 软盘镜像（KERNEL_IMAGE 仅在 x86_64 定义，见 §5；
# 其它架构下该变量为空，这条规则不会被注册）
$(KERNEL_IMAGE): $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=1024 count=1440
	dd if=$< of=$@ bs=512 conv=notrunc

# ─── §12  运行 / 测试目标 ────────────────────────────────────────────────────────
run: kernel
	@echo "Starting QEMU for $(ARCH)..."
	$(QEMU) $(QEMU_FLAGS)

run-net: kernel $(ROOTFS_IMG)
	@if [ "$(ARCH)" != "riscv64" ] && [ "$(ARCH)" != "aarch64" ] && [ "$(ARCH)" != "x86_64" ]; then \
		echo "ERROR: run-net currently supports ARCH=riscv64, ARCH=aarch64 or ARCH=x86_64 only."; \
		exit 1; \
	fi
	@echo "Starting QEMU for $(ARCH) with rootfs at $(ROOTFS_PHYS_ADDR) and virtio-net..."
	@echo "QEMU_NET_FLAGS=$(QEMU_NET_FLAGS)"
	$(QEMU) $(if $(QEMU_RUN_NET_FLAGS),$(QEMU_RUN_NET_FLAGS),$(QEMU_FLAGS)) $(QEMU_ROOTFS_FLAGS) $(QEMU_NET_FLAGS)

test-epoll-perf: epoll-perf kernel $(ROOTFS_IMG)
	@if [ "$(ARCH)" != "riscv64" ] && [ "$(ARCH)" != "aarch64" ]; then \
		echo "ERROR: test-epoll-perf currently supports ARCH=riscv64 or ARCH=aarch64 only."; \
		exit 1; \
	fi
	@echo "Starting QEMU for $(ARCH) with /bin/epoll_perf and virtio-net..."
	@echo "In QEMU shell: /bin/epoll_perf 10000 1"
	@echo "QEMU_NET_FLAGS=$(QEMU_NET_FLAGS)"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS) $(QEMU_NET_FLAGS)

# 创建 ext4 rootfs 镜像（无需 sudo）
# 依赖：Host 已安装 e2fsprogs（mkfs.ext4 >= 1.43 支持 -d 选项）
# 每次 apps 变动时自动重建；切换架构直接使用各自的镜像文件，无需 make clean
$(ROOTFS_IMG): Makefile $(APPS_BINS) $(APPS_C_ELFS) $(LTP_BINS) $(EPOLL_PERF_BIN) $(NGINX_BIN) $(GUEST_LINUX_FILES) | $(BUILD_DIR)
	@echo "=== Building rootfs for $(ARCH): $(ROOTFS_IMG) ==="
	@rm -rf $(ROOTFS_STAGE)
	@mkdir -p $(ROOTFS_STAGE)/bin
	@# 虚拟文件系统挂载点（pseudofs 在内核侧拦截，ext4 只需目录项存在）
	@mkdir -p $(ROOTFS_STAGE)/dev $(ROOTFS_STAGE)/proc $(ROOTFS_STAGE)/sys
	@mkdir -p $(ROOTFS_STAGE)/root $(ROOTFS_STAGE)/tmp $(ROOTFS_STAGE)/etc
	@# 安装 busybox
	@if [ -f apps/busybox-$(ARCH) ]; then \
		cp apps/busybox-$(ARCH) $(ROOTFS_STAGE)/busybox; \
		chmod +x $(ROOTFS_STAGE)/busybox; \
		for applet in sh ls cat echo pwd mkdir rm cp mv grep find ps kill dd time; do \
			cp apps/busybox-$(ARCH) $(ROOTFS_STAGE)/bin/$$applet; \
			chmod +x $(ROOTFS_STAGE)/bin/$$applet; \
		done; \
		echo "  [busybox + applets installed]"; \
	fi
	@# 安装所有 .bin.elf 优先于裸 .bin
	@installed=0; \
	for elf in $(BUILD_DIR)/*.bin.elf; do \
		[ -f "$$elf" ] || continue; \
		name=$$(basename "$$elf" .bin.elf); \
		cp "$$elf" "$(ROOTFS_STAGE)/$$name"; \
		chmod +x "$(ROOTFS_STAGE)/$$name"; \
		echo "  [$$name installed (bin.elf)]"; \
		installed=$$((installed+1)); \
	done; \
	for elf in $(BUILD_DIR)/*.elf; do \
		[ -f "$$elf" ] || continue; \
		case "$$elf" in *.bin.elf|*/kernel_*.elf) continue ;; esac; \
		name=$$(basename "$$elf" .elf); \
		cp "$$elf" "$(ROOTFS_STAGE)/$$name"; \
		chmod +x "$(ROOTFS_STAGE)/$$name"; \
		echo "  [$$name installed (elf)]"; \
		installed=$$((installed+1)); \
	done; \
	if [ "$$installed" -eq 0 ]; then \
		for bin in $(BUILD_DIR)/*.bin; do \
			[ -f "$$bin" ] || continue; \
			name=$$(basename "$$bin" .bin); \
			cp "$$bin" "$(ROOTFS_STAGE)/$$name"; \
			chmod +x "$(ROOTFS_STAGE)/$$name"; \
			echo "  [$$name installed (bin)]"; \
		done; \
	fi
	@# 安装 musl 用户态性能测试程序
	@if [ -f "$(EPOLL_PERF_BIN)" ]; then \
		mkdir -p $(ROOTFS_STAGE)/bin; \
		cp $(EPOLL_PERF_BIN) $(ROOTFS_STAGE)/bin/epoll_perf; \
		chmod +x $(ROOTFS_STAGE)/bin/epoll_perf; \
		echo "  [epoll_perf installed → /bin/epoll_perf]"; \
	fi
	@# 安装 nginx（如果存在对应架构的静态 musl 构建）
	@if [ -f "$(NGINX_BIN)" ]; then \
		mkdir -p $(ROOTFS_STAGE)/bin $(ROOTFS_STAGE)/etc/nginx $(ROOTFS_STAGE)/www; \
		cp $(NGINX_BIN) $(ROOTFS_STAGE)/bin/nginx; \
		chmod +x $(ROOTFS_STAGE)/bin/nginx; \
		printf '%s\n' \
		'worker_processes  1;' \
		'error_log /tmp/nginx-error.log info;' \
		'pid /tmp/nginx.pid;' \
		'events { worker_connections 64; }' \
		'http {' \
		'    access_log off;' \
		'    server {' \
		'        listen 80;' \
		'        root /www;' \
		'        location / { index index.html; }' \
		'    }' \
		'}' > $(ROOTFS_STAGE)/etc/nginx/nginx.conf; \
		printf '%s\n' '<html><body><h1>Avatar nginx</h1></body></html>' > $(ROOTFS_STAGE)/www/index.html; \
		echo "  [nginx installed → /bin/nginx]"; \
	fi
	@# 安装 AArch64 Linux guest 镜像（供 RUN_GUEST_LINUX 从 rootfs 加载）
	@if [ "$(ARCH)" = "aarch64" ]; then \
		missing=0; \
		for f in imgs/aarch64/linux.bin imgs/aarch64/linux.dtb imgs/aarch64/initrd.gz; do \
			if [ ! -f "$$f" ]; then echo "ERROR: missing guest image $$f"; missing=1; fi; \
		done; \
		if [ "$$missing" -ne 0 ]; then exit 1; fi; \
		mkdir -p $(ROOTFS_STAGE)/guests/linux; \
		cp imgs/aarch64/linux.bin $(ROOTFS_STAGE)/guests/linux/linux.bin; \
		cp imgs/aarch64/linux.dtb $(ROOTFS_STAGE)/guests/linux/linux.dtb; \
		cp imgs/aarch64/initrd.gz $(ROOTFS_STAGE)/guests/linux/initrd.gz; \
		echo "  [AArch64 Linux guest installed → /guests/linux]"; \
	fi
	@# 安装 Dropbear SSH 服务器
	@DROPBEAR_MULTI=third_party/dropbear-2024.86/dropbearmulti-$(ARCH); \
	DROPBEAR_STRIP=$(ARCH)-linux-musl-strip; \
	if [ "$(ARCH)" = "riscv64" ]; then \
		DROPBEAR_MULTI=third_party/dropbear-2024.86/dropbearmulti; \
		DROPBEAR_STRIP=riscv64-linux-musl-strip; \
	fi; \
	if [ -f "$$DROPBEAR_MULTI" ]; then \
		mkdir -p $(ROOTFS_STAGE)/usr/sbin $(ROOTFS_STAGE)/usr/bin $(ROOTFS_STAGE)/etc/dropbear; \
		$$DROPBEAR_STRIP -o $(ROOTFS_STAGE)/usr/sbin/dropbearmulti "$$DROPBEAR_MULTI"; \
		chmod +x $(ROOTFS_STAGE)/usr/sbin/dropbearmulti; \
		ln -sf dropbearmulti $(ROOTFS_STAGE)/usr/sbin/dropbear; \
		ln -sf ../sbin/dropbearmulti $(ROOTFS_STAGE)/usr/bin/dropbearkey; \
		echo "  [dropbear SSH installed]"; \
	fi
	@# 安装 LTP 测例（如果已编译）
	@LTP_BIN_DIR=$(LTP_BIN_DIR); \
	if [ -d "$$LTP_BIN_DIR" ] && [ "$$(ls -1 $$LTP_BIN_DIR/*.sh $$LTP_BIN_DIR/[a-z]* 2>/dev/null | wc -l)" -gt 1 ]; then \
		mkdir -p $(ROOTFS_STAGE)/ltp; \
		cp $$LTP_BIN_DIR/* $(ROOTFS_STAGE)/ltp/; \
		chmod +x $(ROOTFS_STAGE)/ltp/*; \
		echo "  [LTP testcases installed → /ltp/]"; \
	fi
	@# /etc/passwd: root 无密码；nobody 供 LTP getpwnam("nobody") 使用
	@printf 'root::0:0:root:/root:/bin/sh\nnobody:x:65534:65534:nobody:/tmp:/bin/sh\n' > $(ROOTFS_STAGE)/etc/passwd
	@printf 'root:x:0:\nnogroup:x:65534:\n' > $(ROOTFS_STAGE)/etc/group
	@echo "  [/etc/passwd + group created]"
	@# 用 staging 目录直接构建 ext4 镜像，无需挂载
	dd if=/dev/zero of=$@ bs=1M count=$(ROOTFS_SIZE_MB) status=none
	mkfs.ext4 -q -b 1024 -L "avatarfs" -d $(ROOTFS_STAGE) $@
	@rm -rf $(ROOTFS_STAGE)
	@echo "Rootfs ready: $@"

rootfs: $(ROOTFS_IMG)
	@echo "Rootfs up to date: $(ROOTFS_IMG)"

# 运行内核 + 加载 rootfs 镜像到 QEMU 客户机内存
run-fs: kernel $(ROOTFS_IMG)
	@echo "Starting QEMU for $(ARCH) with rootfs at $(ROOTFS_PHYS_ADDR)..."
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS)

# ── 便捷测试目标 ─────────────────────────────────────────────────────
#
# test-pthread: 一键跑 pthread_test（使用动态链接 rootfs，无需手动 cp）
#   用法: make ARCH=riscv64 test-pthread LOG=warn
#
test-pthread: kernel
	@if [ ! -f imgs/rootfs-$(ARCH).img ]; then \
		echo "ERROR: imgs/rootfs-$(ARCH).img not found."; \
		echo "Run: bash apps/c/build.sh"; \
		exit 1; \
	fi
	@echo "Copying imgs/rootfs-$(ARCH).img → $(ROOTFS_IMG)"
	@cp imgs/rootfs-$(ARCH).img $(ROOTFS_IMG)
	@echo "Starting QEMU for $(ARCH) with pthread_test rootfs..."
	@echo "In QEMU shell: /bin/pthread_test"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS)

# test-mutex: 一键跑 mutex_test（使用同一动态链接 rootfs）
#   用法: make ARCH=riscv64 test-mutex LOG=warn
#
test-mutex: kernel
	@if [ ! -f imgs/rootfs-$(ARCH).img ]; then \
		echo "ERROR: imgs/rootfs-$(ARCH).img not found."; \
		echo "Run: bash apps/c/build.sh"; \
		exit 1; \
	fi
	@echo "Copying imgs/rootfs-$(ARCH).img → $(ROOTFS_IMG)"
	@cp imgs/rootfs-$(ARCH).img $(ROOTFS_IMG)
	@echo "Starting QEMU for $(ARCH) with mutex_test rootfs..."
	@echo "In QEMU shell: /bin/mutex_test"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS)

# test-vmm: 编译 VMM_TEST=1 内核并运行三线程切换测试（不需要 rootfs）
#   用法: make ARCH=aarch64 test-vmm LOG=info
#
test-vmm:
	$(MAKE) ARCH=$(ARCH) LOG=$(LOG) VMM_TEST=1 kernel

# test-guest-linux: 编译 GUEST_LINUX=1 内核并把 Linux 作为 EL1 guest 启动
#                   rootfs 会自动安装 /guests/linux/{linux.bin,linux.dtb,initrd.gz}
test-guest-linux: $(ROOTFS_IMG)
	@if [ "$(ARCH)" != "aarch64" ]; then \
		echo "ERROR: test-guest-linux currently supports ARCH=aarch64 only."; \
		exit 1; \
	fi
	$(MAKE) PLATFORM=$(PLATFORM) LOG=$(LOG) GUEST_LINUX=1 kernel
	@echo "Starting QEMU (guest Linux)..."
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS)

# test-ltp: 编译 LTP 测例并启动带 rootfs 的 QEMU
#   前提: bash tests/ltp/build.sh [ARCH]  已编译测例到 tests/ltp/bin/<arch>/
#   用法: make ARCH=riscv64 test-ltp LOG=warn
#   在 QEMU shell 中: /ltp/run_ltp.sh
#
test-ltp: kernel $(ROOTFS_IMG)
	@if [ ! -d tests/ltp/bin/$(ARCH) ]; then \
		echo "ERROR: LTP binaries not found. Run: bash tests/ltp/build.sh $(ARCH)"; \
		exit 1; \
	fi
	@echo "Starting QEMU for $(ARCH) with LTP testcases in rootfs..."
	@echo "In QEMU shell: /ltp/run_ltp.sh"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS)

# ─── §13  清理 / 帮助 ────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)

clean-all:
	rm -rf $(BUILD_ROOT)/*

help:
	@echo "Avatar OS Makefile"
	@echo ""
	@echo "Usage: make PLATFORM=<platform> [LOG=<level>] [target]"
	@echo ""
	@echo "Platforms:"
	@echo "  PLATFORM=qemu-virt-x86_64     QEMU x86_64 platform"
	@echo "  PLATFORM=qemu-virt-aarch64    QEMU AArch64 platform (default)"
	@echo "  PLATFORM=qemu-virt-riscv64    QEMU RISC-V 64 platform"
	@echo "  PLATFORM=sg2002-riscv64       SG2002 real board"
	@echo "  PLATFORM=rk3588-aarch64       RK3588 real board"
	@echo ""
	@echo "Legacy compatibility:"
	@echo "  ARCH=<arch> without PLATFORM maps to PLATFORM=qemu-virt-<arch>"
	@echo ""
	@echo "Log Levels:"
	@echo "  LOG=none      Disable all logging (default: info)"
	@echo "  LOG=error     Show only errors"
	@echo "  LOG=warn      Show warnings and errors"
	@echo "  LOG=info      Show info, warnings and errors (default)"
	@echo "  LOG=debug     Show debug info and above"
	@echo "  LOG=trace     Show all logs including trace"
	@echo ""
	@echo "Assertions:"
	@echo "  Assertions are always enabled (assert/assert_always -> platform_panic)"
	@echo ""
	@echo "Cross-compiler:"
	@echo "  CC=<compiler>  Specify compiler (x86_64: gcc, aarch64: aarch64-linux-musl-gcc, riscv64: riscv64-linux-musl-gcc)"
	@echo ""
	@echo "Targets:"
	@echo "  all           Build static libraries (default)"
	@echo "  klog          Build klog library only"
	@echo "  kernel        Build kernel image"
	@echo "  run           Build and run kernel in QEMU (no rootfs)"
	@echo "  run-net       Build and run QEMU with rootfs and virtio-net"
	@echo "  run-fs        Build and run kernel with rootfs (busybox shell)"
	@echo "  test-pthread  Copy dynamic rootfs from imgs/ and run pthread_test"
	@echo "  test-mutex    Copy dynamic rootfs from imgs/ and run mutex_test (futex-based)"
	@echo "  test-vmm      Build with VMM_TEST=1 and run VMM 3-thread switch test"
	@echo "  clean         Remove build artifacts for current PLATFORM"
	@echo "  clean-all     Remove build artifacts for all platforms"
	@echo "  help          Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make PLATFORM=qemu-virt-aarch64 kernel"
	@echo "  make PLATFORM=qemu-virt-riscv64 run-fs LOG=trace"
	@echo "  make PLATFORM=qemu-virt-x86_64 kernel LOG=warn"
	@echo "  make PLATFORM=sg2002-riscv64 kernel LOG=info"
	@echo "  make PLATFORM=qemu-virt-riscv64 clean"
	@echo "  make PLATFORM=qemu-virt-riscv64 test-pthread LOG=warn    # pthread_test 一键测试"
	@echo "  make PLATFORM=qemu-virt-aarch64 test-vmm LOG=info        # VMM 三线程切换测试"
	@echo "  make ARCH=riscv64 kernel                                 # legacy alias for PLATFORM=qemu-virt-riscv64"
	@echo "  make PLATFORM=qemu-virt-riscv64 run-net"
	@echo "  make PLATFORM=qemu-virt-riscv64 run-net QEMU_NET_FLAGS='-netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56'"

# 头文件依赖：编译时已用 -MMD -MP 生成 build/**.d，但当前未纳入 -include，
# 因此修改头文件不会触发重新编译（改头文件后请手动 make clean）。
# 若要启用，需把所有目标的 .d 汇总成一个变量再 -include 之。

# ─── 目录索引 ────────────────────────────────────────────────────────────────────
# 节	内容
# §1	基本参数（PLATFORM / ARCH 兼容 / LOG / SMP / 目录）
# §2	平台配置生成（gen_platform.py）
# §3	日志标志（另含 ASSERT 开关已移除的断言）
# §4	源文件与目标文件变量（lib 对象）
# §4a	内核源文件自动发现（find 递归 + 架构目录排除 + vdev 白名单）
# §4a-2	内核编译标志分组（引用第三方头文件的短名单）
# §4b	平台 / 驱动基础源文件、启动与异常源文件
# §5	工具链与编译标志（按架构）
# §5a	通用编译标志（所有架构共享）
# §6	驱动选择（UART / GIC / NPU / TPU / ETH / ION / SDMMC）
# §6a	基础设备驱动选择
# §6b	加速器驱动选择（NPU / TPU）
# §6c	网络驱动选择（ETH）
# §6e	DRIVER_OBJECTS 组装：源文件 → $(BUILD_DIR)/driver/<文件名>.o
# §6e-1	辅助驱动（ION / SDMMC）
# §6e-2	驱动按编译标志分组
# §7	构建变体（VMM_TEST / GUEST_LINUX / NGINX_TEST）
# §8	第三方库：lwext4 文件系统
# §8a	内核网络栈：netdev + lwIP
# §9	Rootfs 配置
# §10	顶层目标声明
# §11	构建规则
# §11a	库与通用规则
# §11b	内核 / 测试 / 平台 / 启动规则（由 §4a 的列表 eval 生成）
# §11c	启动 / 异常 / 驱动规则（驱动由 §6e 的列表 eval 生成）
# §11c-1	平台配置依赖声明（换平台时精确触发重编）
# §11d	测试 / 库对象规则
# §11e	第三方库编译规则（lwext4 / lwIP）
# §11f	内嵌 guest 测试程序
# §11g	链接（elf → bin → img）
# §12	运行 / 测试目标
# §13	清理 / 帮助
#
# 新增内核源文件：放进 kernel/ 下即可，无需改本文件（见 §4a）。
# 新增驱动：在 §6 对应选择块里把它加进 DRIVER_SRCS_* 组（见 §6e-2）。
