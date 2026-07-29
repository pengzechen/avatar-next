# Avatar OS — 顶层 Makefile
# 用法: make PLATFORM=<platform> [LOG=none|error|warn|info|debug|trace] [ASSERT=panic|off] [target]
# 快速参考: make help

# ─── §1  基本参数 ─────────────────────────────────────────────────────────────
# 架构配置（旧式兼容保留；新式用 PLATFORM=qemu-virt-<arch> 自动推导）
ARCH ?= aarch64

# 平台配置
# 新式 (推荐): PLATFORM=qemu-virt-aarch64  — 平台决定架构，无需指定 ARCH
# 旧式 (兼容): ARCH=aarch64 PLATFORM=qemu  — 沿用旧的三表配置系统
PLATFORM ?= qemu-virt-$(ARCH)

# 日志级别配置
LOG ?= info

# 断言配置
ASSERT ?= panic

# QEMU vCPU 数量（用于 run / run-fs / test-*）
# Phase 0：仅传给 QEMU，内核当前仍按单核运行（cpu_bring_up_all 是 stub）。
SMP ?= 1
ifeq ($(filter $(SMP),1 2 3 4 5 6 7 8),)
$(error Invalid SMP value '$(SMP)'. Use SMP=1..8)
endif

# 目录设置
SRC_DIR         := examples
LIB_DIR         := lib
BUILD_DIR       := build
INCLUDE_DIR     := include
BOOT_DIR        := boot
KERNEL_DIR      := kernel
PLATFORM_DIR    := platforms
TESTS_DIR       := tests
TOOLS_DIR       := tools
FS_DIR          := fs
THIRD_PARTY_DIR := third_party

# ─── §2  平台配置生成 ────────────────────────────────────────────────────────────
# 平台配置全部在 platforms/$(PLATFORM)/platform.lua 的 BUILD_CONFIG 表中。
# gen_platform.py 解析该文件，生成 platform.mk 和内嵌 platform.lua blob。

_PLATFORM_LUA     := $(PLATFORM_DIR)/$(PLATFORM)/platform.lua
_HAVE_PLATFORM_LUA := $(wildcard $(_PLATFORM_LUA))

ifeq ($(_HAVE_PLATFORM_LUA),)
$(error No platform.lua found for PLATFORM=$(PLATFORM). Expected: $(_PLATFORM_LUA))
endif

override ARCH := $(shell sed -n 's/^[[:space:]]*arch[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' $(_PLATFORM_LUA) | head -1)

# 架构切换必须在 Makefile 解析阶段完成；否则并行构建可能在 kernel_clean
# 执行前就开始复用旧架构对象，最终链接出 "file in wrong format"。
$(shell if [ -f .arch ] && [ "$$(cat .arch)" != "$(ARCH)" ]; then \
            echo "Switching architecture from $$(cat .arch) to $(ARCH), cleaning..." >&2; \
            rm -rf $(BUILD_DIR); \
        fi; \
        echo "$(ARCH)" > .arch)

PLATFORM_MK  := $(BUILD_DIR)/platform.mk
$(shell python3 $(TOOLS_DIR)/gen_platform.py $(_PLATFORM_LUA) $(PLATFORM_MK) $(INCLUDE_DIR))
-include $(PLATFORM_MK)

ifeq ($(strip $(MEM_RAM_BASE)),)
$(error Failed to generate platform config from $(_PLATFORM_LUA))
endif

# ─── §3  日志与断言标志 ──────────────────────────────────────────────────────────
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

# ─── §4  源文件与目标文件变量 ────────────────────────────────────────────────────
# 遗留示例源文件（lib/examples/*.c）
SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS    := $(OBJECTS:.o=.d)

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

# 内核源文件
KERNEL_SOURCES := $(KERNEL_DIR)/main.c
KERNEL_OBJECTS := $(KERNEL_SOURCES:$(KERNEL_DIR)/%.c=$(BUILD_DIR)/kernel_%.o)

# ── §4a  架构特定模块（VMM / 异常 / 上下文切换 / 用户程序）─────────────────────
VM_C_SOURCES := $(KERNEL_DIR)/mm/pmm.c $(TESTS_DIR)/pmm_test.c $(KERNEL_DIR)/mm/vm_user.c $(KERNEL_DIR)/mm/kmalloc.c $(KERNEL_DIR)/mm/shared_page.c
VM_C_OBJECTS := $(BUILD_DIR)/kernel_mm_pmm.o $(BUILD_DIR)/kernel_mm_pmm_test.o $(BUILD_DIR)/kernel_mm_vm_user.o $(BUILD_DIR)/kernel_mm_kmalloc.o $(BUILD_DIR)/kernel_mm_shared_page.o

# 架构特定的 VM 模块
ifeq ($(ARCH),aarch64)
    VM_C_SOURCES += $(KERNEL_DIR)/mm/aarch64/vm_early.c
    VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_vm_early.o
    VM_C_SOURCES += $(KERNEL_DIR)/mm/aarch64/vmm.c
    VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_vmm.o
    # Stage-2 MMU
    VM_C_SOURCES += $(KERNEL_DIR)/mm/aarch64/stage2.c
    VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_stage2.o
    # VMM subsystem
    VMM_C_SOURCES := $(KERNEL_DIR)/vmm/vmm.c \
                     $(KERNEL_DIR)/vmm/aarch64/el2_run.c
    VMM_C_OBJECTS := $(BUILD_DIR)/kernel_vmm_vmm.o \
                     $(BUILD_DIR)/kernel_vmm_el2_run.o
    VMM_S_SOURCES := $(KERNEL_DIR)/vmm/aarch64/el2_vmcs.S \
                     $(KERNEL_DIR)/vmm/aarch64/vcpu_ctx.S \
                     $(KERNEL_DIR)/vmm/aarch64/guest_vec.S
    VMM_S_OBJECTS := $(BUILD_DIR)/kernel_vmm_el2_vmcs.o \
                     $(BUILD_DIR)/kernel_vmm_vcpu_ctx.o \
                     $(BUILD_DIR)/kernel_vmm_guest_vec.o
    # guest_test.S: embedded guest program (linked into kernel binary)
    GUEST_TEST_OBJ := $(BUILD_DIR)/apps_guest_test.o \
                      $(BUILD_DIR)/apps_el0_loop.o
else ifeq ($(ARCH),x86_64)
    # x86_64 MM subsystem
    VM_C_SOURCES += $(KERNEL_DIR)/mm/x86_64/vmm.c
    VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_x86_vmm.o
    # x86_64 VMM subsystem
    VMM_C_SOURCES := $(KERNEL_DIR)/vmm/vmm.c \
                     $(KERNEL_DIR)/vmm/x86_64/vmx.c
    VMM_C_OBJECTS := $(BUILD_DIR)/kernel_vmm_vmm.o \
                     $(BUILD_DIR)/kernel_vmm_x86_vmx.o
    VMM_S_SOURCES := $(KERNEL_DIR)/vmm/x86_64/vmx_run.S
    VMM_S_OBJECTS := $(BUILD_DIR)/kernel_vmm_x86_vmx_run.o
    # x86_64 guest test program (linked into kernel binary)
    GUEST_TEST_OBJ := $(BUILD_DIR)/apps_x86_guest_test.o
else ifeq ($(ARCH),riscv64)
    # RISC-V MM subsystem
    VM_C_SOURCES += $(KERNEL_DIR)/mm/riscv64/vmm.c
    VM_C_OBJECTS += $(BUILD_DIR)/kernel_mm_rv_vmm.o
    # RISC-V H-extension VMM subsystem
    VMM_C_SOURCES := $(KERNEL_DIR)/vmm/vmm.c \
                     $(KERNEL_DIR)/vmm/riscv64/hext_run.c
    VMM_C_OBJECTS := $(BUILD_DIR)/kernel_vmm_vmm.o \
                     $(BUILD_DIR)/kernel_vmm_riscv_hext_run.o
    VMM_S_SOURCES := $(KERNEL_DIR)/vmm/riscv64/hext_vcpu.S
    VMM_S_OBJECTS := $(BUILD_DIR)/kernel_vmm_riscv_hext_vcpu.o
    # RISC-V VS-mode guest test program (linked into kernel binary)
    GUEST_TEST_OBJ := $(BUILD_DIR)/apps_riscv_guest_test.o
else
    VMM_C_SOURCES :=
    VMM_C_OBJECTS :=
    VMM_S_SOURCES :=
    VMM_S_OBJECTS :=
    GUEST_TEST_OBJ :=
endif

ifeq ($(ARCH),riscv64)
    # RISC-V VM 模块（如果有的话）
    # VM_C_SOURCES += $(KERNEL_DIR)/mm/riscv64/vm_early.c
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
TASK_C_SOURCES := $(KERNEL_DIR)/task/task.c $(KERNEL_DIR)/task/sched.c $(KERNEL_DIR)/task/mutex.c $(KERNEL_DIR)/task/exec.c $(KERNEL_DIR)/task/cpu.c $(KERNEL_DIR)/task/preempt.c
TASK_C_OBJECTS := $(BUILD_DIR)/kernel_task_task.o $(BUILD_DIR)/kernel_task_sched.o $(BUILD_DIR)/kernel_task_mutex.o $(BUILD_DIR)/kernel_task_exec.o $(BUILD_DIR)/kernel_task_cpu.o $(BUILD_DIR)/kernel_task_preempt.o

# loader 模块源文件
LOADER_C_SOURCES := $(KERNEL_DIR)/loader/bin_loader.c $(KERNEL_DIR)/loader/elf_loader.c $(KERNEL_DIR)/loader/elf_image.c
LOADER_C_OBJECTS := $(BUILD_DIR)/kernel_loader_bin_loader.o $(BUILD_DIR)/kernel_loader_elf_loader.o $(BUILD_DIR)/kernel_loader_elf_image.o

# syscall 模块源文件
SYSCALL_C_SOURCES := $(KERNEL_DIR)/syscall/syscall.c \
                     $(KERNEL_DIR)/syscall/core/futex.c \
                     $(KERNEL_DIR)/syscall/core/proc_lifecycle.c \
                     $(KERNEL_DIR)/syscall/core/proc_ids.c \
                     $(KERNEL_DIR)/syscall/core/sched.c \
                     $(KERNEL_DIR)/syscall/core/signal.c \
                     $(KERNEL_DIR)/syscall/fs/fd_pool.c \
                     $(KERNEL_DIR)/syscall/fs/path.c \
                     $(KERNEL_DIR)/syscall/fs/tty.c \
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
                     $(KERNEL_DIR)/syscall/mm/brk.c \
                     $(KERNEL_DIR)/syscall/mm/mmap.c \
                     $(KERNEL_DIR)/syscall/mm/pmap_compat.c \
                     $(KERNEL_DIR)/syscall/net/ksocket.c \
                     $(KERNEL_DIR)/syscall/net/sock_syscall.c
SYSCALL_C_OBJECTS := $(BUILD_DIR)/kernel_syscall_syscall.o \
                     $(BUILD_DIR)/kernel_syscall_core_futex.o \
                     $(BUILD_DIR)/kernel_syscall_core_proc_lifecycle.o \
                     $(BUILD_DIR)/kernel_syscall_core_proc_ids.o \
                     $(BUILD_DIR)/kernel_syscall_core_sched.o \
                     $(BUILD_DIR)/kernel_syscall_core_signal.o \
                     $(BUILD_DIR)/kernel_syscall_fs_fd_pool.o \
                     $(BUILD_DIR)/kernel_syscall_fs_path.o \
                     $(BUILD_DIR)/kernel_syscall_fs_tty.o \
                     $(BUILD_DIR)/kernel_syscall_fs_file_io.o \
                     $(BUILD_DIR)/kernel_syscall_fs_file_ops.o \
                     $(BUILD_DIR)/kernel_syscall_fs_file_stat.o \
                     $(BUILD_DIR)/kernel_syscall_fs_dir.o \
                     $(BUILD_DIR)/kernel_syscall_fs_ioctl.o \
                     $(BUILD_DIR)/kernel_syscall_fs_pipe.o \
                     $(BUILD_DIR)/kernel_syscall_fs_pty.o \
                     $(BUILD_DIR)/kernel_syscall_io_poll.o \
                     $(BUILD_DIR)/kernel_syscall_io_select.o \
                     $(BUILD_DIR)/kernel_syscall_io_epoll.o \
                     $(BUILD_DIR)/kernel_syscall_mm_brk.o \
                     $(BUILD_DIR)/kernel_syscall_mm_mmap.o \
                     $(BUILD_DIR)/kernel_syscall_mm_pmap_compat.o \
                     $(BUILD_DIR)/kernel_syscall_net_ksocket.o \
                     $(BUILD_DIR)/kernel_syscall_net_sock_syscall.o
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
    QEMU_FLAGS    := -machine q35 -enable-kvm -cpu host -smp $(SMP) -m 2G -nographic -kernel $(KERNEL_BIN)
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
    CFLAGS  += -mno-outline-atomics  # freestanding：禁止 GCC outline atomics 调用 libgcc 帮助函数
    CFLAGS  += -ffreestanding -fno-builtin  # 禁用内置函数和标准库
    TARGET  := $(BUILD_DIR)/spinlock_aarch64.a
    KLOG_TARGET := $(BUILD_DIR)/libklog_aarch64.a
    KERNEL_TARGET := $(BUILD_DIR)/kernel_aarch64.elf
    KERNEL_BIN    := $(BUILD_DIR)/kernel_aarch64.bin
    KERNEL_LINK_ADDR ?= 0xffff000040080000
    LDFLAGS += -Wl,--defsym=KERNEL_LINK_ADDR=$(KERNEL_LINK_ADDR)
    QEMU          := qemu-system-aarch64
    QEMU_FLAGS    := -cpu cortex-a76 -M virt,virtualization=on -smp $(SMP) -m 2G -nographic -kernel $(KERNEL_BIN)
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
CFLAGS  += -DDEVICE_USB_BASE_RAW=$(DEV_USB_BASE)
CFLAGS  += -DDEVICE_USB_PHY_BASE_RAW=$(DEV_USB_PHY_BASE)

# ─── §6  驱动选择 ────────────────────────────────────────────────────────────────
# 所有驱动默认值来自 platform.lua → gen_platform.py → platform.mk。
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
    DRIVER_NPU_OBJS := $(BUILD_DIR)/drv_npu_rknpu.o $(BUILD_DIR)/drv_npu_rkpm.o
else
    DRIVER_NPU_SRCS :=
    DRIVER_NPU_OBJS :=
endif

TPU ?= $(DEV_TPU_TYPE)
ifeq ($(TPU),cvitpu)
    DRIVER_TPU_SRCS := driver/tpu/cvi_tpu.c
    CFLAGS          += -DDRIVER_TPU_CVITPU=1
    DRIVER_TPU_OBJS := $(BUILD_DIR)/drv_tpu_cvi_tpu.o
else
    DRIVER_TPU_SRCS :=
    DRIVER_TPU_OBJS :=
endif

# ── §6b+ Rust 工具链参数（ETH/USB 共用）───────────────────────────────────────
_RUST_TARGET    := riscv64gc-unknown-none-elf
_RUST_DIR       := rust

# ── §6c  网络驱动（ETH）─────────────────────────────────────────────────────────
# 以太网驱动（由 platform.lua 的 eth.driver 自动推导；也可命令行覆盖：ETH=none / ETH=cvitek / ETH=virtio）
ETH ?= $(DEV_ETH_TYPE)
ifeq ($(ETH),cvitek)
    ifneq ($(ARCH),riscv64)
        $(error ETH=cvitek 目前仅支持 ARCH=riscv64)
    endif
    CFLAGS          += -DDRIVER_ETH_CVITEK=1
    DRIVER_ETH_OBJS := $(BUILD_DIR)/drv_eth/cvitek_eth_bridge.o $(BUILD_DIR)/rust_glue.o $(BUILD_DIR)/libavatar_eth.a
    _RUST_TARGET    := riscv64gc-unknown-none-elf
    _RUST_DIR       := rust
else ifeq ($(ETH),virtio)
    ifneq ($(ARCH),riscv64)
        $(error ETH=virtio 目前仅支持 ARCH=riscv64)
    endif
    CFLAGS          += -DDRIVER_ETH_VIRTIO=1
    DRIVER_ETH_OBJS := $(BUILD_DIR)/drv_eth/virtio_net.o
else
    DRIVER_ETH_OBJS :=
endif

# ── §6d  USB 驱动（DWC2 主机控制器）───────────────────────────────────
# 由 platform.lua 的 usb.driver 自动推导；也可命令行覆盖：USB=none / USB=dwc2
USB ?= $(DEV_USB_TYPE)
ifeq ($(USB),dwc2)
    CFLAGS           += -DDRIVER_USB_DWC2=1
    DRIVER_USB_OBJS  := $(BUILD_DIR)/rust_glue.o $(BUILD_DIR)/libavatar_usb.a $(BUILD_DIR)/drv_usb_uvc_video_glue.o
else
    DRIVER_USB_OBJS  :=
endif

# ── §6e  DRIVER_OBJECTS 最终组装 ────────────────────────────────────────────────
# 在所有驱动选择块执行完毕后，统一从各驱动变量中收集目标文件。
DRIVER_OBJECTS := $(patsubst driver/%.c,$(BUILD_DIR)/drv_%.o,$(DRIVER_UART_SRC))
ifneq ($(strip $(DRIVER_IRQ_SRC)),)
ifeq ($(DRIVER_IRQ_SRC),driver/irq/gicv2.c)
	DRIVER_OBJECTS += $(BUILD_DIR)/gicv2.o
else ifeq ($(DRIVER_IRQ_SRC),driver/irq/gicv3.c)
	DRIVER_OBJECTS += $(BUILD_DIR)/gicv3.o
endif
endif
ifeq ($(ARCH),riscv64)
	DRIVER_OBJECTS += $(BUILD_DIR)/plic.o
endif
ifneq ($(strip $(DRIVER_TIMER_SRC)),)
	DRIVER_OBJECTS += $(BUILD_DIR)/timer.o
endif
ifeq ($(DEV_NEED_LAPIC),1)
	DRIVER_OBJECTS += $(BUILD_DIR)/lapic.o
endif
ifneq ($(strip $(DRIVER_NPU_OBJS)),)
	DRIVER_OBJECTS += $(DRIVER_NPU_OBJS)
endif
ifneq ($(strip $(DRIVER_TPU_OBJS)),)
	DRIVER_OBJECTS += $(DRIVER_TPU_OBJS)
endif
ifneq ($(strip $(DRIVER_ETH_OBJS)),)
	DRIVER_OBJECTS += $(DRIVER_ETH_OBJS)
endif
ifneq ($(strip $(DRIVER_USB_OBJS)),)
		DRIVER_OBJECTS += $(DRIVER_USB_OBJS)
endif

# ── §6e  辅助驱动（ION / SDMMC）────────────────────────────────────────────────
# Ion 内存分配器（当 TPU=cvitpu 时自动启用；也可独立启用 ION=1）
ION ?= $(if $(filter cvitpu,$(TPU)),1,0)
ifeq ($(ION),1)
    CFLAGS           += -DDRIVER_ION=1
    DRIVER_ION_OBJS  := $(BUILD_DIR)/drv_ion_ion.o
    DRIVER_OBJECTS   += $(DRIVER_ION_OBJS)
endif

# SDMMC 块设备驱动（由 platform.lua 的 sdmmc.driver 自动推导；也可命令行覆盖）
SDMMC ?= $(DEV_SDMMC_TYPE)
ifeq ($(SDMMC),sg2002)
    CFLAGS              += -DDRIVER_SDBLK_SG2002=1
    DRIVER_OBJECTS      += $(BUILD_DIR)/drv_blk_sdblk.o
endif

CFLAGS  += -MMD -MP

# ─── §7  构建变体 ─────────────────────────────────────────────────────────────
# VMM_TEST=1：编译 RUN_VMM_TEST，跳过 busybox，运行三线程切换测试
# 新增测试时仿照此模式，同时在 _BUILD_VARIANT 里加一个唯一标识。
VMM_TEST ?= 0
ifeq ($(VMM_TEST),1)
    CFLAGS += -DRUN_VMM_TEST=1
    _BUILD_VARIANT := vmm_test
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

NET_OBJS := $(BUILD_DIR)/kernel_net_netdev.o \
            $(BUILD_DIR)/kernel_net_net.o \
            $(BUILD_DIR)/kernel_net_tcp_echo.o \
            $(BUILD_DIR)/kernel_net_webcam_httpd.o \
            $(BUILD_DIR)/kernel_net_http_server.o \
            $(BUILD_DIR)/kernel_net_lwip_port_netif_avatar.o \
            $(BUILD_DIR)/kernel_net_lwip_port_sys_arch.o

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
LWIP_OBJS := $(patsubst $(LWIP_DIR)/src/core/%.c,$(BUILD_DIR)/lwip_core_%.o,$(filter $(LWIP_DIR)/src/core/%.c,$(filter-out $(LWIP_DIR)/src/core/ipv4/%,$(LWIP_CORE_SRCS))))
LWIP_OBJS += $(patsubst $(LWIP_DIR)/src/core/ipv4/%.c,$(BUILD_DIR)/lwip_ipv4_%.o,$(filter $(LWIP_DIR)/src/core/ipv4/%.c,$(LWIP_CORE_SRCS)))
LWIP_OBJS += $(patsubst $(LWIP_DIR)/src/netif/%.c,$(BUILD_DIR)/lwip_netif_%.o,$(filter $(LWIP_DIR)/src/netif/%.c,$(LWIP_CORE_SRCS)))

LWIP_CFLAGS := $(CFLAGS)
LWIP_CFLAGS += -I$(LWIP_DIR)/src/include
LWIP_CFLAGS += -I$(LWIP_PORT_DIR)
LWIP_CFLAGS += -I$(LWIP_PORT_DIR)/arch
LWIP_CFLAGS += -DLWIP_NO_CTYPE_H=1
LWIP_CFLAGS += -w

# lwext4 库源文件（第三方代码）
LWEXT4_SRCS     := $(wildcard $(LWEXT4_DIR)/src/*.c)
LWEXT4_OBJS     := $(patsubst $(LWEXT4_DIR)/src/%.c,$(BUILD_DIR)/lwext4_%.o,$(LWEXT4_SRCS))

# lwext4 移植胶水代码（属于本项目，使用 LWEXT4_CFLAGS）
LWEXT4_PORT_OBJS := $(BUILD_DIR)/lwext4_port_kmalloc.o \
                   $(BUILD_DIR)/drv_blk_ramblk.o \
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

# ─── §9  第三方库：Lua 5.4 ──────────────────────────────────────────────────────
LUA_DIR      := $(THIRD_PARTY_DIR)/lua54
LUA_SRC_DIR  := $(LUA_DIR)/src
LUA_PORT_DIR := $(LIB_DIR)/lua54_port
LUA_LIBC_SHIM := $(LUA_PORT_DIR)/libc_shim

# Lua VM 核心模块（不含 linit.c / lmathlib — 后者依赖 libc 数学函数）
LUA_CORE_SRCS := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem \
                 lobject lopcodes lparser lstate lstring ltable ltm \
                 lundump lvm lzio lauxlib lbaselib ltablib lstrlib

LUA_CORE_OBJS := $(patsubst %,$(BUILD_DIR)/lua54_%.o,$(LUA_CORE_SRCS))

# LUA_CFLAGS: libc shim 头文件先于 include/，FP 限制解除
LUA_CFLAGS := -I$(LUA_LIBC_SHIM) -I$(LUA_SRC_DIR) \
              $(filter-out -mgeneral-regs-only,$(CFLAGS))
ifeq ($(ARCH),x86_64)
LUA_CFLAGS := $(filter-out -mno-mmx -mno-sse,$(LUA_CFLAGS))
LUA_CFLAGS += -msse2
endif
LUA_CFLAGS += -Os -w -DLUA_C89_NUMBERS=1

# setjmp 汇编（使用标准 CFLAGS）
SETJMP_OBJ := $(BUILD_DIR)/setjmp_$(ARCH).o

# Lua 胶水代码（使用 LUA_CFLAGS）
LUA_GLUE_OBJS := $(BUILD_DIR)/lua_platform.o \
                 $(BUILD_DIR)/lua_drivers.o \
                 $(BUILD_DIR)/lua_math_impl.o \
                 $(BUILD_DIR)/lua_kernel_init.o

# 嵌入式 platform.lua 字节数组（由 gen_platform.py 生成，使用标准 CFLAGS）
LUA_BLOB_OBJ := $(BUILD_DIR)/platform_lua_blob.o

LUA_OBJECTS := $(LUA_CORE_OBJS) $(LUA_GLUE_OBJS) $(LUA_BLOB_OBJ) $(SETJMP_OBJ)

# ─── §10  Rootfs 配置 ────────────────────────────────────────────────────────────
# 每个架构独立一个镜像，切换架构无需 make clean
ROOTFS_IMG       := $(BUILD_DIR)/rootfs-$(ARCH).img
ROOTFS_STAGE     := $(BUILD_DIR)/rootfs-stage-$(ARCH)
LTP_BIN_DIR      := tests/ltp/bin/$(ARCH)
LTP_BINS         := $(wildcard $(LTP_BIN_DIR)/*)
# ROOTFS_SIZE_MB / ROOTFS_PHYS_ADDR 来自自动生成的 $(MEM_LAYOUT_MK)
QEMU_ROOTFS_FLAGS = -device loader,file=$(ROOTFS_IMG),addr=$(ROOTFS_PHYS_ADDR),force-raw=on

# ─── §11  顶层目标声明 ───────────────────────────────────────────────────────────
.PHONY: all clean help klog kernel run run-net rootfs run-fs test-pthread test-mutex test-vmm test-ltp

all: $(TARGET) klog

kernel: | kernel_clean
kernel: $(KERNEL_BIN)

# 架构切换清理在解析阶段完成；保留空目标兼容 kernel 的 order-only 依赖。
.PHONY: kernel_clean
kernel_clean:

klog: $(KLOG_TARGET)

$(BUILD_DIR):
	$(MKDIR) $(BUILD_DIR)

$(TARGET): $(OBJECTS) | $(BUILD_DIR)
	$(AR) rcs $@ $^

$(KLOG_TARGET): $(KLOG_OBJECT) | $(BUILD_DIR)
	$(AR) rcs $@ $^

# ─── §12  构建规则 ───────────────────────────────────────────────────────────────

# ── §12a  库与通用规则 ───────────────────────────────────────────────────────
$(BUILD_DIR)/klog.o: $(LIB_DIR)/klog.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vsnprintf.o: $(LIB_DIR)/vsnprintf.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/string.o: $(LIB_DIR)/string.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc.o: $(LIB_DIR)/libc.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ── §12b  内核 / 任务 / 加载器 / 系统调用规则 ──────────────────────────────────
$(BUILD_DIR)/kernel_%.o: $(KERNEL_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests_%.o: $(TESTS_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/platform_%.o: $(PLATFORM_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/boot_%.o: $(BOOT_DIR)/$(ARCH)/%.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ── §12c  启动 / 异常 / 驱动规则 ────────────────────────────────────────────────
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

$(BUILD_DIR)/plic.o: driver/irq/plic.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Idriver -c $< -o $@

$(BUILD_DIR)/timer.o: driver/timer/timer.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# NPU 驱动编译规则
$(BUILD_DIR)/drv_npu_%.o: driver/npu/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drv_tpu_%.o: driver/tpu/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Idriver/tpu -c $< -o $@

$(BUILD_DIR)/drv_ion_%.o: driver/ion/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Idriver/ion -Ikernel/mm -c $< -o $@

# SD 块设备（包含 lwext4 接口，使用 LWEXT4_CFLAGS）
$(BUILD_DIR)/drv_blk_sdblk.o: driver/blk/sdblk.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -Idriver -c $< -o $@

# PseudoFS（虚拟文件系统 /dev /proc /sys）— 始终构建
PSEUDOFS_OBJS := $(BUILD_DIR)/pseudofs_pseudofs.o

# Most kernel objects are compiled with platform-derived CFLAGS
# (PLATFORM_*, DRIVER_*, DEVICE_*, memory layout).  The generator writes these
# files only when contents change, so this catches real platform switches
# without forcing recompilation on every make invocation.
PLATFORM_CONFIG_DEPS := $(PLATFORM_MK) $(_PLATFORM_LUA)
$(BOOT_OBJECTS) $(KERNEL_OBJECTS) $(NET_OBJS) $(TASK_C_OBJECTS) $(TASK_S_OBJ) \
$(TASK_USER_TEST_OBJ) $(TASK_USER_HELLO_OBJ) $(TASK_USER_TESTEXECVE_OBJ) \
$(LOADER_C_OBJECTS) $(SYSCALL_C_OBJECTS) \
$(VM_C_OBJECTS) $(VM_S_OBJ) $(VMM_C_OBJECTS) $(VMM_S_OBJECTS) \
$(GUEST_TEST_OBJ) $(TESTS_OBJECTS) $(PLATFORM_OBJECTS) $(DRIVER_OBJECTS) \
$(EXCEPTION_OBJECTS) $(KLOG_OBJECT) $(VSNPRINTF_OBJECT) $(STRING_OBJECT) \
$(LIBC_OBJECT) $(BITMAP_OBJECT) $(PLATFORM_CFG_OBJECT) $(LWEXT4_OBJS) $(LWEXT4_PORT_OBJS) \
$(LUA_CORE_OBJS) $(LUA_GLUE_OBJS) $(LUA_BLOB_OBJ) $(SETJMP_OBJ) \
$(PSEUDOFS_OBJS) $(LWIP_OBJS): $(PLATFORM_CONFIG_DEPS)

$(BUILD_DIR)/pseudofs_pseudofs.o: fs/pseudofs/pseudofs.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Idriver -Ikernel -Ikernel/mm -c $< -o $@

$(BUILD_DIR)/kernel_net_lwip_port_%.o: $(KERNEL_DIR)/net/lwip_port/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_net_%.o: $(KERNEL_DIR)/net/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# 驱动编译规则
$(BUILD_DIR)/drv_%.o: driver/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# USB 驱动编译规则（Rust staticlib + C glue）
$(BUILD_DIR)/libavatar_usb.a: | $(BUILD_DIR)
	cd $(_RUST_DIR) && cargo build --release --target $(_RUST_TARGET)
	cp $(_RUST_DIR)/target/$(_RUST_TARGET)/release/libavatar_usb.a $@

$(BUILD_DIR)/drv_usb_uvc_video_glue.o: driver/usb/uvc_video_glue.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Idriver/usb -c $< -o $@

# task 模块编译规则
$(BUILD_DIR)/kernel_task_task.o: $(KERNEL_DIR)/task/task.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_task_sched.o: $(KERNEL_DIR)/task/sched.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_task_mutex.o: $(KERNEL_DIR)/task/mutex.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_task_cpu.o: $(KERNEL_DIR)/task/cpu.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_task_preempt.o: $(KERNEL_DIR)/task/preempt.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_task_exec.o: $(KERNEL_DIR)/task/exec.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/task_switch.o: $(TASK_S_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# loader 模块编译规则
$(BUILD_DIR)/kernel_loader_bin_loader.o: $(KERNEL_DIR)/loader/bin_loader.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_loader_elf_loader.o: $(KERNEL_DIR)/loader/elf_loader.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_loader_elf_image.o: $(KERNEL_DIR)/loader/elf_image.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# syscall 模块编译规则
$(BUILD_DIR)/kernel_syscall_syscall.o: $(KERNEL_DIR)/syscall/syscall.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_core_futex.o: $(KERNEL_DIR)/syscall/core/futex.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_core_proc_lifecycle.o: $(KERNEL_DIR)/syscall/core/proc_lifecycle.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_core_proc_ids.o: $(KERNEL_DIR)/syscall/core/proc_ids.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_core_sched.o: $(KERNEL_DIR)/syscall/core/sched.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_core_signal.o: $(KERNEL_DIR)/syscall/core/signal.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_fd_pool.o: $(KERNEL_DIR)/syscall/fs/fd_pool.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_path.o: $(KERNEL_DIR)/syscall/fs/path.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_tty.o: $(KERNEL_DIR)/syscall/fs/tty.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_file_io.o: $(KERNEL_DIR)/syscall/fs/file_io.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_file_ops.o: $(KERNEL_DIR)/syscall/fs/file_ops.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_file_stat.o: $(KERNEL_DIR)/syscall/fs/file_stat.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_dir.o: $(KERNEL_DIR)/syscall/fs/dir.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_ioctl.o: $(KERNEL_DIR)/syscall/fs/ioctl.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_pipe.o: $(KERNEL_DIR)/syscall/fs/pipe.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_fs_pty.o: $(KERNEL_DIR)/syscall/fs/pty.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_io_poll.o: $(KERNEL_DIR)/syscall/io/poll.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_io_select.o: $(KERNEL_DIR)/syscall/io/select.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_io_epoll.o: $(KERNEL_DIR)/syscall/io/epoll.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_mm_brk.o: $(KERNEL_DIR)/syscall/mm/brk.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_mm_mmap.o: $(KERNEL_DIR)/syscall/mm/mmap.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_mm_pmap_compat.o: $(KERNEL_DIR)/syscall/mm/pmap_compat.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_net_ksocket.o: $(KERNEL_DIR)/syscall/net/ksocket.c | $(BUILD_DIR)
	$(CC) $(LWIP_CFLAGS) -I$(LWEXT4_DIR)/include -I$(LWEXT4_PORT_DIR) -c $< -o $@

$(BUILD_DIR)/kernel_syscall_net_sock_syscall.o: $(KERNEL_DIR)/syscall/net/sock_syscall.c | $(BUILD_DIR)
	$(CC) $(LWIP_CFLAGS) -I$(LWEXT4_DIR)/include -I$(LWEXT4_PORT_DIR) -c $< -o $@

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

# C 用户程序编译规则（当前各架构目录暂无 .c 应用；保留直接静态链接形式）
$(BUILD_DIR)/%.elf: $(APPS_DIR)/%.c $(APPS_LD) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/apps_$*.o
	$(CC) $(CFLAGS) -static -nostdlib -nostartfiles -nodefaultlibs \
		-T $(APPS_LD) \
		$(BUILD_DIR)/apps_$*.o \
		-o $@
	@echo "C app ELF created: $@"

$(TASK_USER_BIN): $(BUILD_DIR)/user_test.o $(TASK_USER_LD) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -nostdlib -nostartfiles -nodefaultlibs -T $(TASK_USER_LD) -o $@.elf $<
	$(OBJCOPY) -O binary $@.elf $@
	@echo "User program linked at: $(shell aarch64-linux-musl-nm $@.elf | grep user_test_program)"
	@echo "User data at: $(shell aarch64-linux-musl-nm $@.elf | grep msg_hello)"

# ── §12d  VM / VMM / 架构特定规则 ───────────────────────────────────────────────
$(BUILD_DIR)/kernel_mm_vm_early.o: $(VM_EARLY_C_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_mm_pmm.o: $(KERNEL_DIR)/mm/pmm.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_mm_pmm_test.o: $(TESTS_DIR)/pmm_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 架构特定的 VMM 模块（仅 AArch64）
ifeq ($(ARCH),aarch64)
$(BUILD_DIR)/kernel_mm_vmm.o: $(KERNEL_DIR)/mm/aarch64/vmm.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

ifeq ($(ARCH),riscv64)
$(BUILD_DIR)/kernel_mm_rv_vmm.o: $(KERNEL_DIR)/mm/riscv64/vmm.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

ifeq ($(ARCH),x86_64)
$(BUILD_DIR)/kernel_mm_x86_vmm.o: $(KERNEL_DIR)/mm/x86_64/vmm.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

$(BUILD_DIR)/kernel_mm_vm_user.o: $(KERNEL_DIR)/mm/vm_user.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_mm_kmalloc.o: $(KERNEL_DIR)/mm/kmalloc.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_mm_shared_page.o: $(KERNEL_DIR)/mm/shared_page.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/bitmap.o: $(LIB_DIR)/bitmap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/platform_cfg.o: $(LIB_DIR)/platform_cfg.c | $(BUILD_DIR)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

# Rust FFI 胶水层（kernel_alloc/kernel_free 包装器，仅 ETH=cvitek 时编译）
$(BUILD_DIR)/rust_glue.o: $(LIB_DIR)/rust_glue.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Rust 静态库（仅 ETH=cvitek 时构建）
$(BUILD_DIR)/libavatar_eth.a: | $(BUILD_DIR)
	cd $(_RUST_DIR) && MAKEFLAGS= cargo build --release --target $(_RUST_TARGET)
	cp $(_RUST_DIR)/target/$(_RUST_TARGET)/release/libavatar_eth.a $@

$(BUILD_DIR)/kernel_mm_mmu.o: $(VM_S_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ── §12e  第三方库编译规则（lwext4 / Lua）────────────────────────────────────────
# 第三方 lwext4 源文件：使用包含 compat 路径的专用 LWEXT4_CFLAGS
$(BUILD_DIR)/lwext4_%.o: $(LWEXT4_DIR)/src/%.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

# lwext4 移植胶水代码：属于本项目，使用普通 CFLAGS
$(BUILD_DIR)/lwext4_port_kmalloc.o: $(LWEXT4_PORT_DIR)/kmalloc.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# RAM 块设备和 FS 初始化（需要 lwext4 头文件，使用 LWEXT4_CFLAGS）
$(BUILD_DIR)/drv_blk_ramblk.o: driver/blk/ramblk.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWEXT4_CFLAGS) -Idriver -c $< -o $@

$(BUILD_DIR)/lwext4_port_fs_init.o: $(LWEXT4_PORT_DIR)/fs_init.c | $(BUILD_DIR)
	$(CC) $(LWEXT4_CFLAGS) -I$(LWEXT4_PORT_DIR) -c $< -o $@

$(BUILD_DIR)/lwip_core_%.o: $(LWIP_DIR)/src/core/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lwip_ipv4_%.o: $(LWIP_DIR)/src/core/ipv4/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lwip_netif_%.o: $(LWIP_DIR)/src/netif/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# ── §12f  Lua 5.4 编译规则 ──────────────────────────────────────────────────────
# Lua VM 核心源文件：使用 LUA_CFLAGS（FP 开启，compat 头文件路径前置）
$(BUILD_DIR)/lua54_%.o: $(LUA_SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

# setjmp 汇编（每架构一个）
$(SETJMP_OBJ): $(LIB_DIR)/setjmp/setjmp_$(ARCH).S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Lua 平台胶水代码（LUA_CFLAGS + lua.h 路径）
$(BUILD_DIR)/lua_platform.o: $(LUA_PORT_DIR)/platform.c | $(BUILD_DIR)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lua_drivers.o: $(LUA_PORT_DIR)/drivers.c | $(BUILD_DIR)
	$(CC) $(LUA_CFLAGS) -Idriver -c $< -o $@

$(BUILD_DIR)/lua_math_impl.o: $(LUA_LIBC_SHIM)/lua_math_impl.c | $(BUILD_DIR)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lua_kernel_init.o: $(LUA_PORT_DIR)/lua_kernel_init.c | $(BUILD_DIR)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

# 嵌入式 platform.lua —— 由 gen_platform.py 生成到 build/ 目录
# This rule re-runs gen_platform.py after any arch-switch clean wipes build/
$(BUILD_DIR)/platform_lua_blob.c: $(_PLATFORM_LUA) $(TOOLS_DIR)/gen_platform.py | $(BUILD_DIR)
	python3 $(TOOLS_DIR)/gen_platform.py $(_PLATFORM_LUA) $(PLATFORM_MK) $(INCLUDE_DIR)

$(BUILD_DIR)/platform_lua_blob.o: $(BUILD_DIR)/platform_lua_blob.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# VMM 模块编译规则（仅 AArch64）
ifeq ($(ARCH),aarch64)
$(BUILD_DIR)/kernel_mm_stage2.o: $(KERNEL_DIR)/mm/aarch64/stage2.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_vmm_vmm.o: $(KERNEL_DIR)/vmm/vmm.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ikernel -Ikernel/vmm -c $< -o $@

$(BUILD_DIR)/kernel_vmm_el2_run.o: $(KERNEL_DIR)/vmm/aarch64/el2_run.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ikernel -Ikernel/vmm -c $< -o $@

$(BUILD_DIR)/kernel_vmm_el2_vmcs.o: $(KERNEL_DIR)/vmm/aarch64/el2_vmcs.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_vmm_vcpu_ctx.o: $(KERNEL_DIR)/vmm/aarch64/vcpu_ctx.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel_vmm_guest_vec.o: $(KERNEL_DIR)/vmm/aarch64/guest_vec.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/apps_guest_test.o: apps/aarch64/guest_test.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/apps_el0_loop.o: apps/aarch64/el0_loop.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

ifeq ($(ARCH),x86_64)
$(BUILD_DIR)/kernel_vmm_vmm.o: $(KERNEL_DIR)/vmm/vmm.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ikernel -Ikernel/vmm -c $< -o $@

$(BUILD_DIR)/kernel_vmm_x86_vmx.o: $(KERNEL_DIR)/vmm/x86_64/vmx.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ikernel -Ikernel/vmm -c $< -o $@

$(BUILD_DIR)/kernel_vmm_x86_vmx_run.o: $(KERNEL_DIR)/vmm/x86_64/vmx_run.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/apps_x86_guest_test.o: apps/x86_64/guest_test.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

ifeq ($(ARCH),riscv64)
$(BUILD_DIR)/kernel_vmm_vmm.o: $(KERNEL_DIR)/vmm/vmm.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ikernel -Ikernel/vmm -c $< -o $@

$(BUILD_DIR)/kernel_vmm_riscv_hext_run.o: $(KERNEL_DIR)/vmm/riscv64/hext_run.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ikernel -Ikernel/vmm -c $< -o $@

$(BUILD_DIR)/kernel_vmm_riscv_hext_vcpu.o: $(KERNEL_DIR)/vmm/riscv64/hext_vcpu.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/apps_riscv_guest_test.o: apps/riscv64/guest_test.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
endif

# ── §12g  链接 ────────────────────────────────────────────────────────────────────
$(KERNEL_TARGET): $(BOOT_OBJECTS) $(KERNEL_OBJECTS) $(NET_OBJS) $(LWIP_OBJS) $(TASK_C_OBJECTS) $(TASK_S_OBJ) $(TASK_USER_TEST_OBJ) $(TASK_USER_HELLO_OBJ) $(TASK_USER_TESTEXECVE_OBJ) $(LOADER_C_OBJECTS) $(SYSCALL_C_OBJECTS) $(VM_C_OBJECTS) $(VM_S_OBJ) $(VMM_C_OBJECTS) $(VMM_S_OBJECTS) $(GUEST_TEST_OBJ) $(TESTS_OBJECTS) $(PLATFORM_OBJECTS) $(DRIVER_OBJECTS) $(EXCEPTION_OBJECTS) $(KLOG_OBJECT) $(VSNPRINTF_OBJECT) $(STRING_OBJECT) $(LIBC_OBJECT) $(BITMAP_OBJECT) $(PLATFORM_CFG_OBJECT) $(LWEXT4_OBJS) $(LWEXT4_PORT_OBJS) $(LUA_OBJECTS) $(PSEUDOFS_OBJS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -nostartfiles -nodefaultlibs -T $(BOOT_DIR)/$(ARCH)/link.ld -o $@ -Wl,--start-group $^ -Wl,--end-group

# 转换为二进制文件
$(KERNEL_BIN): $(KERNEL_TARGET)
	$(OBJCOPY) -O binary $< $@

# 创建软盘镜像（1.44MB）
$(KERNEL_IMAGE): $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=1024 count=1440
	dd if=$< of=$@ bs=512 conv=notrunc

# ─── §13  运行 / 测试目标 ────────────────────────────────────────────────────────
run: kernel
	@echo "Starting QEMU for $(ARCH)..."
	$(QEMU) $(QEMU_FLAGS)

run-net: kernel $(ROOTFS_IMG)
	@if [ "$(ARCH)" != "riscv64" ]; then \
		echo "ERROR: run-net currently supports ARCH=riscv64 only."; \
		exit 1; \
	fi
	@echo "Starting QEMU for $(ARCH) with rootfs at $(ROOTFS_PHYS_ADDR) and virtio-net..."
	@echo "QEMU_NET_FLAGS=$(QEMU_NET_FLAGS)"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS) $(QEMU_NET_FLAGS)

# 创建 ext4 rootfs 镜像（无需 sudo）
# 依赖：Host 已安装 e2fsprogs（mkfs.ext4 >= 1.43 支持 -d 选项）
# 每次 apps 变动时自动重建；切换架构直接使用各自的镜像文件，无需 make clean
$(ROOTFS_IMG): Makefile $(APPS_BINS) $(APPS_C_ELFS) $(LTP_BINS) | $(BUILD_DIR)
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
	@# 安装 Dropbear SSH 服务器
	@DROPBEAR_MULTI=third_party/dropbear-2024.86/dropbearmulti; \
	if [ -f "$$DROPBEAR_MULTI" ]; then \
		mkdir -p $(ROOTFS_STAGE)/usr/sbin $(ROOTFS_STAGE)/usr/bin $(ROOTFS_STAGE)/etc/dropbear; \
		riscv64-linux-musl-strip -o $(ROOTFS_STAGE)/usr/sbin/dropbearmulti "$$DROPBEAR_MULTI"; \
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
	$(MAKE) ARCH=$(ARCH) LOG=$(LOG) ASSERT=$(ASSERT) VMM_TEST=1 kernel
	@echo "Starting QEMU for $(ARCH) — VMM 3-thread context switch test..."
	$(QEMU) $(QEMU_FLAGS)

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

# ─── §14  清理 / 帮助 ────────────────────────────────────────────────────────────
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
	@echo "  (Future real boards: add platforms/<name>/platform.lua)"
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
	@echo "  run           Build and run kernel in QEMU (no rootfs)"
	@echo "  run-net       Build and run RISC-V QEMU with rootfs and virtio-net"
	@echo "  run-fs        Build and run kernel with rootfs (busybox shell)"
	@echo "  test-pthread  Copy dynamic rootfs from imgs/ and run pthread_test"
	@echo "  test-mutex    Copy dynamic rootfs from imgs/ and run mutex_test (futex-based)"
	@echo "  test-vmm      Build with VMM_TEST=1 and run VMM 3-thread switch test"
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
	@echo "  make ARCH=riscv64 test-pthread LOG=warn    # pthread_test 一键测试"
	@echo "  make ARCH=aarch64 test-vmm   LOG=info     # VMM 三线程切换测试"
	@echo "  make PLATFORM=qemu-virt-riscv64 run-net"
	@echo "  make PLATFORM=qemu-virt-riscv64 run-net QEMU_NET_FLAGS='-netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56'"

# 包含依赖文件
-include $(DEPS)



# 节	内容
# §1	基本参数（ARCH / PLATFORM / LOG / ASSERT / 目录）
# §2	平台配置生成（gen_platform.py）
# §3	日志与断言标志
# §4	源文件与目标文件变量
# §4a	架构特定模块（VMM / 异常 / 切换 / 用户程序）
# §4b	平台 / 驱动基础源文件
# §5	工具链与编译标志
# §5a	通用编译标志
# §6	驱动选择
# §6a-e	UART/GIC → NPU/TPU → ETH → 组装 → ION/SDMMC
# §7	构建变体（VMM_TEST）
# §8	第三方库：lwext4
# §9	第三方库：Lua 5.4
# §10	Rootfs 配置
# §11	顶层目标声明
# §12	构建规则（a~g 子节）
# §13	运行 / 测试目标
# §14	清理 / 帮助
