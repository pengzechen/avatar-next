# Avatar OS — 顶层 Makefile
# 用法: make PLATFORM=<platform> [LOG=none|error|warn|info|debug|trace] [target]
# 快速参考: make help
#
# 构建逻辑分片（按变更频率拆开，顶层只留参数、源文件发现与 include）：
#   driver/drivers.mk    驱动选择（UART / GIC / ETH / NPU / TPU / ION / DMA / BLK）
#   mk/toolchain.mk      三架构工具链、CFLAGS/LDFLAGS、QEMU 基础参数
#   mk/apps.mk           用户态程序（apps/）变量与编译规则
#   mk/rootfs.mk         rootfs 配置、guest 镜像布局、镜像/运行目标
#   mk/qemu.mk           QEMU 运行目标与便捷测试目标
#   mk/format.mk         clang-format 与 C API 文档
#   tools/make_rootfs.sh rootfs 镜像 recipe（由 mk/rootfs.mk 调用）

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

# ─── §4  源文件与目标文件变量 ────────────────────────────────────────────────────
# klog 库源文件
KLOG_SOURCES := $(LIB_DIR)/klog.c
VSNPRINTF_SOURCES := $(LIB_DIR)/vsnprintf.c
STRING_SOURCES := $(LIB_DIR)/string.c
BITMAP_SOURCES := $(LIB_DIR)/bitmap.c
KLOG_OBJECT         := $(BUILD_DIR)/klog.o
VSNPRINTF_OBJECT    := $(BUILD_DIR)/vsnprintf.o
STRING_OBJECT       := $(BUILD_DIR)/string.o
BITMAP_OBJECT       := $(BUILD_DIR)/bitmap.o
LIBC_OBJECT         := $(BUILD_DIR)/libc.o
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

# GIC 版本解析。§6 里那句 `GIC ?= $(DEV_DEFAULT_GIC)` 要等到 400 行之后才执行，
# 但 QEMU_FLAGS（mk/toolchain.mk）和下面的 vdev 源文件列表都是**立即展开**的，等不到那时候。
# 命令行赋值总是先于 makefile 生效，而 DEV_DEFAULT_GIC 由 platform.mk（第 59 行
# include）提供，所以这里算出来的就是最终值。
GIC_VER := $(if $(GIC),$(GIC),$(DEV_DEFAULT_GIC))

# vGIC 后端按 GIC 版本二选一：vdev/vgic/ = GICv2，vdev/vgicv3/ = GICv3。
# 两套的寄存器模型（GICH MMIO vs ICH_* 系统寄存器）不兼容，只能编译一套。
#
# ⚠️ GIC= 会改变 CFLAGS（-DDRIVER_GIC_V2/V3）和源文件列表，而 -MMD 的 .d
# 没有被 -include（见本文件 §11 附近说明），make 察觉不到这种变化。
# 从 GIC=v2 切到 v3（或反向）**不再需要手动 clean**：§7 的 _CFG_CHECK 会在
# 检测到配置变化时自动清掉已编译的目标文件（历史上这里要求手动 clean，
# 忘了就会混用两套 flag 编出来的 .o，报一堆莫名其妙的 undefined reference）。
# 该检查同样覆盖 SMP= / LOG= / LOG_MODULES=。
_KERNEL_VGIC_SRCS   := $(if $(filter v3,$(GIC_VER)),\
                          $(wildcard $(KERNEL_DIR)/vmm/vdev/vgicv3/*.c),\
                          $(wildcard $(KERNEL_DIR)/vmm/vdev/vgic/*.c))

# vdev 下按文件选架构；loader/ 下的 guest_loader.c 按架构显式加入（见下方列表）。
ifeq ($(ARCH),aarch64)
    _KERNEL_VDEV_SRCS     := $(KERNEL_DIR)/vmm/vdev/console/vpl011.c \
                             $(KERNEL_DIR)/vmm/vdev/npu/vnpu.c \
                             $(_KERNEL_VGIC_SRCS)
    _KERNEL_ARCHONLY_SRCS := $(KERNEL_DIR)/vmm/loader/guest_loader.c
    # guest_test.S: embedded guest program (linked into kernel binary)
    GUEST_TEST_OBJ        := $(BUILD_DIR)/apps_guest_test.o \
                             $(BUILD_DIR)/apps_el0_loop.o
else ifeq ($(ARCH),riscv64)
    _KERNEL_VDEV_SRCS     := $(KERNEL_DIR)/vmm/vdev/console/vuart16550.c \
                             $(KERNEL_DIR)/vmm/vdev/vplic.c
    _KERNEL_ARCHONLY_SRCS := $(KERNEL_DIR)/vmm/loader/guest_loader.c
    # RISC-V VS-mode guest test program (linked into kernel binary)
    GUEST_TEST_OBJ        := $(BUILD_DIR)/apps_riscv_guest_test.o
else ifeq ($(ARCH),x86_64)
    _KERNEL_VDEV_SRCS     := $(KERNEL_DIR)/vmm/vdev/console/vuart16550.c \
                             $(KERNEL_DIR)/vmm/vdev/vlapic.c
    # guest_loader.c 被 KERNEL_SRCS 的 filter-out 排除了，必须在这里加回来；
    # 它的编译分组另见 KERNEL_LWEXT4_SRCS。两处都要有 —— 少一处，
    # 下面的「分组完整性断言」会当场报 67 vs 68。
    _KERNEL_ARCHONLY_SRCS := $(KERNEL_DIR)/vmm/loader/guest_loader.c
    # x86_64 guest test program (linked into kernel binary)
    GUEST_TEST_OBJ        := $(BUILD_DIR)/apps_x86_guest_test.o
else
    _KERNEL_VDEV_SRCS     :=
    _KERNEL_ARCHONLY_SRCS :=
    GUEST_TEST_OBJ        :=
endif

KERNEL_SRCS := $(filter-out $(_KERNEL_EXCLUDE_PAT) $(KERNEL_DIR)/vmm/loader/guest_loader.c,$(_KERNEL_FOUND))
KERNEL_SRCS += $(_KERNEL_VDEV_SRCS) $(_KERNEL_ARCHONLY_SRCS)

# fs/ 下的 VFS / pseudofs / 总入口。后端的 core 与 port 由 §8/§8b 单独编译，
# 不在这个列表里；这里只收"内核文件系统层"。
FS_CORE_SRCS := $(FS_DIR)/init.c $(FS_DIR)/vfs/vfs.c $(FS_DIR)/vfs/fat_vfs.c \
                $(wildcard $(FS_DIR)/pseudofs/*.c)
KERNEL_SRCS += $(FS_CORE_SRCS)

# 源路径 → 扁平对象名：kernel/mm/pmm.c → $(BUILD_DIR)/kernel_mm_pmm.o
kobj = $(BUILD_DIR)/$(subst /,_,$(basename $(1))).o
KERNEL_OBJECTS := $(foreach f,$(KERNEL_SRCS),$(call kobj,$(f)))

# ── §4a-2  内核编译标志分组 ────────────────────────────────────────────────────
# kernel/ 的编译标志无法按目录推导（同一目录里有些文件引用第三方头文件、
# 有些没有），所以引用第三方头文件的文件在这里显式列出，其余一律用 CFLAGS。
# 列入 = 拿到 lwext4 的头文件路径；漏列 = 编译失败（include 找不到），
# 所以名单不准会立刻炸，不会静默走错分支。
#
# 注意这一组是**内核自己的代码**，警告必须可见（LWEXT4_CFLAGS 里不再带 -w，
# `-w` 只加在编译第三方源码的那条规则上）—— 见 §8 里 LWEXT4_THIRD_PARTY_CFLAGS
# 的注释：fs_lock.c 的指针截断事故就是被一刀切的 -w 藏掉的。
KERNEL_LWEXT4_SRCS := \
    $(FS_DIR)/vfs/vfs.c \
    $(KERNEL_DIR)/task/elf_loader.c \
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

# loader/guest_loader.c 只给「已实现 Linux guest 启动」的架构编译
# （见 §4a 白名单与 include/vmm/vmm.h 的 VMM_GUEST_LINUX_SUPPORTED），且引用 lwext4。
#
# ⚠️ image_load.c 是 2026-09 从 guest_loader.c 拆出来的装载通路（VFS 读 + 逐块
# 写 guest 内存），它同样 #include "fs/vfs.h"（→ <ext4.h>），所以**必须进同一组** ——
# 少了这一条，编译会以 "include/fs/vfs.h:14:10: fatal error: ext4.h: No such file" 失败，
# 而且因为 §236 的 filter-out，它同时还会触发下面的「分组完整性断言」。
ifneq ($(filter $(ARCH),aarch64 riscv64 x86_64),)
KERNEL_LWEXT4_SRCS += $(KERNEL_DIR)/vmm/loader/guest_loader.c \
                      $(KERNEL_DIR)/vmm/loader/image_load.c
endif

# 文件系统总入口和 FAT 的 VFS 胶水都需要 FatFs 头（ff.h）和 lwext4 头
# （vfs.h → ext4.h），单独一组，避免把 FatFs include 路径灌给所有 lwext4 组文件。
# pseudofs 不需要额外头，落在 KERNEL_PLAIN_SRCS。
KERNEL_FS_SRCS := $(FS_DIR)/init.c \
                  $(FS_DIR)/vfs/fat_vfs.c

# kernel/net/** 全部引用 lwIP，按目录自动派生，将来新增文件不会漏。
KERNEL_LWIP_SRCS := $(filter $(KERNEL_DIR)/net/%,$(KERNEL_SRCS))

# 这两个同时引用 lwIP 与 lwext4：
# ksocket.c → syscall/fs/fd_pool.h → include/fs/vfs.h:14 → #include <ext4.h>
KERNEL_LWIPX_SRCS := $(KERNEL_DIR)/syscall/net/ksocket.c \
                     $(KERNEL_DIR)/syscall/net/sock_syscall.c

KERNEL_PLAIN_SRCS := $(filter-out $(KERNEL_FS_SRCS) $(KERNEL_LWEXT4_SRCS) $(KERNEL_LWIP_SRCS) $(KERNEL_LWIPX_SRCS),$(KERNEL_SRCS))

# 分组完整性断言：漏一个或重一个都当场报错，而不是静默少编一个文件。
ifneq ($(words $(KERNEL_SRCS)),$(words $(KERNEL_PLAIN_SRCS) $(KERNEL_FS_SRCS) $(KERNEL_LWEXT4_SRCS) $(KERNEL_LWIP_SRCS) $(KERNEL_LWIPX_SRCS)))
$(error kernel source grouping mismatch: $(words $(KERNEL_SRCS)) sources vs $(words $(KERNEL_PLAIN_SRCS) $(KERNEL_FS_SRCS) $(KERNEL_LWEXT4_SRCS) $(KERNEL_LWIP_SRCS) $(KERNEL_LWIPX_SRCS)) grouped. Check KERNEL_FS_SRCS / KERNEL_LWEXT4_SRCS / KERNEL_LWIP*_SRCS)
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

# 启动变体钩子：这些文件提供 test_boot_* 强定义，由 TEST_BOOT_SRC 按
# GUEST_LINUX / VMM_TEST 选一个链接。它们也在 unit-test/ 下，必须从自动
# 扫描里排除，否则会同时编成普通的 tests_*.o，链接期重复定义（且每个变体
# 都会链进不属于自己的钩子）。新增变体时把钩子文件加到这里。
TEST_BOOT_HOOKS := $(UNIT_TEST_DIR)/guest_test.c $(UNIT_TEST_DIR)/vcpu_thread_test.c

# 测试源文件（tests/unit-test/）。pmm 单独编译，其余自动链接进内核。
TESTS_SOURCES := $(filter-out $(UNIT_TEST_DIR)/pmm_test.c $(TEST_BOOT_HOOKS), \
                 $(wildcard $(UNIT_TEST_DIR)/*.c))
TESTS_OBJECTS := $(TESTS_SOURCES:$(UNIT_TEST_DIR)/%.c=$(BUILD_DIR)/tests_%.o)

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
PLATFORM_SOURCES := $(PLATFORM_DIR)/$(PLATFORM)/platform.c \
                    $(PLATFORM_DIR)/common/platform_cfg.c
ifneq ($(filter qemu-virt-% rk3588-aarch64 sg2002-riscv64,$(PLATFORM)),)
    PLATFORM_SOURCES += $(PLATFORM_DIR)/common/platform_ops.c
endif
PLATFORM_OBJECTS := $(PLATFORM_SOURCES:$(PLATFORM_DIR)/%.c=$(BUILD_DIR)/platform_%.o)

ifeq ($(wildcard $(PLATFORM_SOURCES)),)
$(error Missing platform source: $(PLATFORM_SOURCES))
endif

# 驱动源文件由 driver/drivers.mk 选择，DRIVER_OBJECTS 在 §6 统一组装。

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

include mk/toolchain.mk

# ─── §6  驱动选择 ────────────────────────────────────────────────────────────────
# 选择逻辑下沉到 driver/drivers.mk 与各 class 的片段。顶层只负责把源文件
# 映射成 build/<platform>/driver/<basename>.o（driver/ 下文件名互不重复）。
#
# 默认值来自 platform.conf → gen_platform.py → platform.mk；仍可用命令行覆盖：
# UART=、GIC=/IRQ=、ETH=、NPU=、TPU=、DMA=、ION=、BLOCK=。

DRIVER_SRCS_CORE   := driver/core/driver.c driver/core/device.c driver/blk/block.c \
                      driver/serial/serial.c driver/irq/irq.c \
                      driver/ion/ion_core.c driver/dma/dma_core.c \
                      driver/tpu/tpu_core.c driver/npu/npu_core.c
DRIVER_SRCS_CFLAGS :=
DRIVER_SRCS_PLIC   :=
DRIVER_SRCS_TPU    :=
DRIVER_SRCS_ION    :=
DRIVER_SRCS_DMA    :=
DRIVER_SRCS_BLK    :=

include driver/drivers.mk

DRIVER_SRCS := $(strip $(DRIVER_SRCS_CORE) $(DRIVER_SRCS_CFLAGS) $(DRIVER_SRCS_PLIC) \
                       $(DRIVER_SRCS_TPU) $(DRIVER_SRCS_ION) $(DRIVER_SRCS_DMA) \
                       $(DRIVER_SRCS_BLK))

# 源文件 → $(BUILD_DIR)/driver/<basename>.o。
# GNU Make 的 '%' 会匹配 '/'，pattern rule 做不到展平，所以由 §11c 逐文件 eval。
_drv_obj = $(BUILD_DIR)/driver/$(notdir $(basename $(1))).o
DRIVER_OBJECTS := $(foreach f,$(DRIVER_SRCS),$(call _drv_obj,$(f)))

# 用户态程序：变量与编译规则（需要 driver/drivers.mk 的 DMA/NPU 选择）
include mk/apps.mk

CFLAGS  += -MMD -MP

# ─── §7  构建变体 ─────────────────────────────────────────────────────────────
# 三个变体互斥，优先级 GUEST_LINUX > VMM_TEST > NGINX_TEST > normal：
#   VMM_TEST=1    ：链接 tests/unit-test/vcpu_thread_test.c，运行三线程切换测试
#   GUEST_LINUX=1 ：链接 tests/unit-test/guest_test.c，从 rootfs 直启 Linux guest
#   NGINX_TEST=1  ：定义 RUN_NGINX_TEST，网络栈专供 nginx 压测（无对应 target）
# 新增测试时仿照此模式，同时在 _BUILD_VARIANT 里加一个唯一标识。
#
# guest/VMM 不再定义 RUN_* 宏：变体只决定链接哪个 boot 对象，main.c 调用
# tests/boot_mode.h 的通用钩子（active / skip_net / run）。正常模式没有
# 额外对象：main.c 里的 weak 默认实现就是 busybox 路径。
VMM_TEST ?= 0
NGINX_TEST ?= 0
GUEST_LINUX ?= 0
ifeq ($(GUEST_LINUX),1)
    TEST_BOOT_SRC := $(UNIT_TEST_DIR)/guest_test.c
    _BUILD_VARIANT := guest_linux
else ifeq ($(VMM_TEST),1)
    TEST_BOOT_SRC := $(UNIT_TEST_DIR)/vcpu_thread_test.c
    _BUILD_VARIANT := vmm_test
else ifeq ($(NGINX_TEST),1)
    CFLAGS += -DRUN_NGINX_TEST=1
    TEST_BOOT_SRC :=
    _BUILD_VARIANT := nginx_test
else
    TEST_BOOT_SRC :=
    _BUILD_VARIANT := normal
endif
TEST_BOOT_OBJ := $(if $(TEST_BOOT_SRC),$(BUILD_DIR)/test_boot.o)

# STRING_TEST=1：定义 RUN_STRING_TEST，kernel_main 早期跑 tests/unit-test/string_test.c 自检
# （与上面三个变体正交：自检跑完继续正常启动）。标志只影响 kernel_main.o，
# 所以并入 _BUILD_VARIANT 让变体机制在开关变化时清掉那一个对象即可。
STRING_TEST ?= 0
ifeq ($(STRING_TEST),1)
    CFLAGS += -DRUN_STRING_TEST=1
    _BUILD_VARIANT := $(_BUILD_VARIANT)+string_test
endif

# TASK_TEST=1：暴露 completion 的用户态测试 syscall
# （wait/complete/reset，见 kernel/syscall/syscall.c）。只影响 syscall.o，
# 正常内核不编这三个入口；用户态程序 /bin/task-completion 始终可用，
# 但在非 TASK_TEST 内核上调用会得到 ENOSYS。
TASK_TEST ?= 0
ifeq ($(TASK_TEST),1)
    CFLAGS += -DTASK_INFRA_TEST=1
    _BUILD_VARIANT := $(_BUILD_VARIANT)+task_test
endif

# RT_TEST=1：启动时跑 RT 调度类自测（tests/unit-test/rt_sched_test.c）
RT_TEST ?= 0
ifeq ($(RT_TEST),1)
    CFLAGS += -DRUN_RT_SCHED_TEST=1
    _BUILD_VARIANT := $(_BUILD_VARIANT)+rt_test
endif

# RT_DEMO=1：启动后运行 RT 性能 demo（ping-pong 唤醒延迟 / 抢占延迟）
RT_DEMO ?= 0
ifeq ($(RT_DEMO),1)
    CFLAGS += -DRT_DEMO=1
    _BUILD_VARIANT := $(_BUILD_VARIANT)+rt_demo
endif

# RT_IRQ_TRACE=1：在 arch_irq_save/restore/disable/enable 上记录 IRQ-off 区间，
# 并在 preempt_disable/enable 上记录外层 preempt-off 区间
# （诊断变体；默认关闭，避免给关中断/自旋锁热路径增加函数调用开销）
RT_IRQ_TRACE ?= 0
ifeq ($(RT_IRQ_TRACE),1)
    CFLAGS += -DRT_IRQ_TRACE=1
    _BUILD_VARIANT := $(_BUILD_VARIANT)+rt_irq_trace
endif

# PMM_TEST=1：定义 RUN_PMM_TESTS，在 fs_init 之后跑 tests/unit-test/pmm_test.c
# 并关机。调用点在 tests/unit-test/unit_test.c，main.c 不含测试细节。
#
# 以前这个宏只能"手动加 CFLAGS"，测试框架（tests/Makefile）没法稳定地
# 编出 PMM 测试变体。现在它是一个正式变体。
PMM_TEST ?= 0
ifeq ($(PMM_TEST),1)
    CFLAGS += -DRUN_PMM_TESTS=1
    _BUILD_VARIANT := $(_BUILD_VARIANT)+pmm_test
endif

# KMEM_TEST=1：定义 RUN_KMEM_TEST，在 fs_init 之后跑 tests/unit-test/kmem_test.c
# （仅 AArch64 有实现），跑完继续正常启动。普通启动不会跑这个测试。
KMEM_TEST ?= 0
ifeq ($(KMEM_TEST),1)
    CFLAGS += -DRUN_KMEM_TEST=1
    _BUILD_VARIANT := $(_BUILD_VARIANT)+kmem_test
endif

# PANIC_TEST=1：定义 RUN_PANIC_TEST，在 ELF 解析路径上**故意 panic 一次**，
# 用来看调用栈回溯长什么样（kernel/mm/elf_image.c、docs/basic/BACKTRACE.md）。
# 和 STRING_TEST 一样与上面三个变体正交，只影响 elf_image.o 一个对象。
#
# 触发点选在 ELF 头部校验之后：那里离 boot 足够远，能打出一条真实的
# 加载器调用链（exec → elf_image_load → elf_image_load_impl），
# 而不是只有 kernel_main 两层。
#
# ⚠️ 这个变体编出来的内核**跑不完启动** —— 它不是用来跑功能的。
PANIC_TEST ?= 0
ifeq ($(PANIC_TEST),1)
    CFLAGS += -DRUN_PANIC_TEST=1
    _BUILD_VARIANT := $(_BUILD_VARIANT)+panic_test
endif

# ─── §7b  日志模块白名单 ───────────────────────────────────────────────────────
# LOG_MODULES=uart,gic,timer ：编译期默认模块掩码（配合 LOG=debug|trace 使用）。
#   不传      = 全部模块（保持旧行为）
#   all/none  = 全开 / 全关
#   一经传入即为**白名单**：未打模块标签的 KLOG_DEBUG/KLOG_TRACE 归入
#   LOG_MODULE_GENERIC，不在名单里就不输出（见 include/klog.h）。
#   例: make PLATFORM=qemu-virt-aarch64 run LOG=debug LOG_MODULES=gic
LOG_MODULES ?=
ifneq ($(LOG_MODULES),)
    CFLAGS += -DLOG_MODULES_DEFAULT='"$(LOG_MODULES)"'
endif

# 变体改变时自动清除**受变体影响的**对象文件，防止复用缓存了错误条件编译的 .o。
#
# ⚠️ 新增变体时必须把它的条件编译打到哪个文件加进 _VARIANT_OBJS。
# 漏了的话症状是"新开关没生效"——**而且没有任何报错**：CFLAGS 变了但
# _CFG_SIG（§7 下面那个）里没有变体这一项，make 也就不知道要重编。
# 历史上这里只清 kernel_main.o（当时变体确实只影响它），加 STRING_TEST/
# PANIC_TEST 之后就靠不住了 —— 后者的条件编译在 kernel/mm/elf_image.c。
#
# 两个方向都要正确：从 A 切到 B 和从 B 切回 A 都得清掉。所以这里**无条件**
# 列出所有可能受影响的文件，而不是只在对应开关打开时列 —— 否则切回去那一趟
# 会漏。（多编一个 .o 的代价可以忽略。）
_VARIANT_FILE := $(BUILD_DIR)/.build_variant
_VARIANT_OBJS := $(BUILD_DIR)/kernel_main.o \
                 $(BUILD_DIR)/kernel_mm_elf_image.o \
                 $(BUILD_DIR)/kernel_syscall_syscall.o \
                 $(BUILD_DIR)/tests_unit_test.o \
                 $(TEST_BOOT_OBJ)
_VARIANT_CHECK := $(shell \
    mkdir -p $(BUILD_DIR) 2>/dev/null; \
    if [ "$$(cat $(_VARIANT_FILE) 2>/dev/null)" != "$(_BUILD_VARIANT)" ]; then \
        rm -f $(_VARIANT_OBJS); \
        printf '%s' '$(_BUILD_VARIANT)' > $(_VARIANT_FILE); \
    fi)

# 凡是**会改 CFLAGS 的配置项**（GIC= / SMP= / LOG= / LOG_MODULES=）切换时同理，
# 而且影响面大得多：它们改变每一个 TU 的编译结果；GIC= 还会改**源文件列表**
# （vdev/vgic/ 与 vdev/vgicv3/ 二选一）。.d 没有被 -include，make 察觉不到这种
# 变化，于是新旧两套 .o 混在一起链接，报出来的是
# 「undefined reference to gicv2_gicd_base」「vmm_vgic_set_pending」这类
# **完全指不到真正原因**的符号缺失 —— 实测踩过两次（GIC 一次、SMP 一次），
# 都很容易误判成驱动写错了。
#
# 所以配置一变就把已编译的目标文件全清掉，不指望用户记得那句 clean
# （§4a 的注释里虽然写了，但说明都在 400 行开外，报错时没人会去翻）。
#
# 只删 .o/.elf —— 都是可再生的；rootfs 镜像不动，guest DTB 的切换由
# $(GUEST_GIC_STAMP) 单独负责触发重建。
#
# ⚠️ kallsyms_*.S 必须一并删掉。它是 tools/gen_kallsyms.py 从**上一趟链接**
# 出来的 ELF 里抽的符号表（见 §11g）。只删 .elf 而留下这份 .S，重建时它的
# mtime 会比新生成的 stage-1 ELF 还新，make 判定"已是最新"→ 拿上一次构建
# 的符号表去链接。症状是 backtrace 打出函数名整体错位，但二进制本身完全
# 正常 —— 正是 §11 注释里警告的那类「看起来完全合理」的幽灵 bug。
#
# 戳文件不存在时（刚 clean 过、或本机制刚引入）也走一次清理：此时无从得知
# 现有目标文件是用哪套 flag 编的，重建比赌一把便宜。
#
# 注：_BUILD_VARIANT（GUEST_LINUX= / STRING_TEST= / PANIC_TEST= 等）不在这个
# 签名里 —— 它影响面小得多（每个变体只条件编译一两个文件），走上面
# _VARIANT_CHECK 的细粒度处理：只删 _VARIANT_OBJS 里列的那几个 .o，
# 不为此整包重编。新增变体时记得把受影响的对象加进那个列表。
_CFG_STAMP_FILE := $(BUILD_DIR)/.build-cfg
# RT_IRQ_TRACE 会影响所有包含 exception_impl.h 的 TU（arch_irq_save/restore
# 内联点遍布全内核），因此必须进 _CFG_SIG 触发全量重编，不能只靠
# _VARIANT_OBJS 的细粒度清理。
_CFG_SIG        := $(GIC_VER)/smp$(SMP)/vcpus$(GUEST_VCPUS)/log$(LOG)/mods$(LOG_MODULES)/fp$(FP)/fatmp$(FAT_MOUNT_POINT)/rtirq$(RT_IRQ_TRACE)
_CFG_CHECK := $(shell \
    mkdir -p $(BUILD_DIR) 2>/dev/null; \
    rm -f $(BUILD_DIR)/.gic-last; \
    if [ "$$(cat $(_CFG_STAMP_FILE) 2>/dev/null)" != "$(_CFG_SIG)" ]; then \
        find $(BUILD_DIR) -name '*.o' -delete 2>/dev/null; \
        find $(BUILD_DIR) -name '*.elf' -delete 2>/dev/null; \
        find $(BUILD_DIR) -name 'kallsyms_*.S' -delete 2>/dev/null; \
        printf '%s' '$(_CFG_SIG)' > $(_CFG_STAMP_FILE); \
    fi)

MKDIR   := mkdir -p

# 顶层默认目标：早期 include 的 mk/*.mk 会定义目标，显式钉住 all（klog）。
.DEFAULT_GOAL := all

# ─── §8  ext4 后端：lwext4（first-party）───────────────────────────────────────
LWEXT4_DIR      := $(FS_DIR)/ext4/lwext4
LWEXT4_PORT_DIR := $(FS_DIR)/ext4/port

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
                   $(BUILD_DIR)/lwext4_port_fs_lock.o

# lwext4 的全局串行化（详见 fs/ext4/port/fs_lock.c）。
#
# lwext4 是单线程用户态库，全局缓冲缓存无内部同步 —— SMP>1 时并发 exec
# 会把它的红黑树写坏。这里用**链接期符号包装**把所有公开 API 的引用改指向
# 我们的包装函数（进去先拿全局锁），真实实现仍可经 __real_* 调用。
#
# 为什么不直接在调用点加锁：内核里有 90+ 个调用点、22 个直接调用者中 15 个
# 是多出口的，手工配对 lock/unlock 必漏；而且粒度必须落在"每次 API 调用"
# 上（read_file_fully 要循环读 5.6MB，整函数持锁会关中断几十~几百毫秒）。
#
# ⚠️ 新增 lwext4 调用时，如果用到列表外的符号，**要把它加进来**，
#    否则那条路径不受保护。列表与 fs_lock.c 里的 FS_WRAP(...) 一一对应，
#    两处必须同时改。漏了包装不会报错（只是没锁），所以改 lwext4 调用时
#    顺手用 `grep -rhoE '\bext4_[a-z0-9_]+' kernel/ fs/ driver/` 对一遍。
LWEXT4_WRAP_SYMS := \
    ext4_block_fini ext4_block_init ext4_device_register ext4_device_unregister \
    ext4_dir_close ext4_dir_entry_next ext4_dir_mk ext4_dir_open ext4_dir_rm \
    ext4_fclose ext4_fopen ext4_fopen2 ext4_fread ext4_fremove ext4_frename \
    ext4_fseek ext4_fsize ext4_ftell ext4_ftruncate ext4_fwrite \
    ext4_inode_exist ext4_mode_get ext4_mount \
    ext4_raw_inode_fill ext4_readlink ext4_sb_read

LDFLAGS += $(foreach s,$(LWEXT4_WRAP_SYMS),-Wl,--wrap=$(s))

# lwext4 专用编译标志（在通用 CFLAGS 基础上添加）
LWEXT4_CFLAGS  := $(CFLAGS)
LWEXT4_CFLAGS  += -I$(FS_DIR)               # "vfs/vfs_internal.h"（跨文件引用）
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

# `-w` 只给 lwext4 自己的源码。**本项目的胶水代码（fs/ext4/port/*、
# driver/blk/ramblk.c）必须保持警告可见** —— 曾经这里是一刀切的 `-w`，
# 于是 fs_lock.c 里漏声明 `__real_ext4_*` （编译器按隐式声明当成返回 int）
# 这条致命警告被吞掉，最终表现成 aarch64 上指针被 `sxtw` 截断的
# "确定性崩溃"（详见 fs/ext4/port/fs_lock.c 顶部注释）。
LWEXT4_THIRD_PARTY_CFLAGS := $(LWEXT4_CFLAGS) -w

# ─── §8b  FAT 后端：FatFs（FAT12/16/32，first-party）───────────────────────────
# 源码在 fs/fat/fatfs/source（唯一 vendor patch 是 ffconf.h wrapper），
# 真正的配置在 fs/fat/port/ffconf.h，diskio 也在 fs/fat/port/。
FATFS_DIR      := $(FS_DIR)/fat/fatfs
FATFS_PORT_DIR := $(FS_DIR)/fat/port
FATFS_SRCS     := $(FATFS_DIR)/source/ff.c $(FATFS_DIR)/source/ffunicode.c
FATFS_OBJS     := $(patsubst $(FATFS_DIR)/source/%.c,$(THIRD_PARTY_BUILD_DIR)/fatfs_%.o,$(FATFS_SRCS))
FATFS_PORT_OBJS := $(BUILD_DIR)/fatfs_port_diskio.o
# FatFs 胶水需要 lwext4 的 ext4_blockdev 类型（共享 bdif），所以基于 LWEXT4_CFLAGS。
FATFS_CFLAGS   := $(LWEXT4_CFLAGS) -I$(FATFS_DIR)/source -I$(FATFS_PORT_DIR) -I$(FS_DIR)
FATFS_THIRD_PARTY_CFLAGS := $(FATFS_CFLAGS) -w

include mk/rootfs.mk
# ─── §9a  第三方源码前置检查 ─────────────────────────────────────────────────────
#
# third_party/ 下的 lwext4 与 lwIP 都**不再是 submodule**，而是内嵌在本仓库里
# 的源码副本。原因见各自的 README.md：lwext4 是因为我们 fork 并修改了它，而
# .gitmodules 指向上游、父仓库钉的却是 fork 的 commit —— 全新 clone 必失败；
# lwIP 是未修改的上游代码，一并内嵌以彻底去掉 "submodule 状态可能撒谎" 这个
# 失败面。
#
# 这个检查仍然保留：它现在挡的是**工作树不完整**（checkout 被中断、有人手工
# 删过 third_party 里的东西）。那种情况下构建会死在一行极具误导性的错误上：
#
#     include/fs/vfs.h:14:10: fatal error: ext4.h: No such file or directory
#
# 这行指向的是**我们自己的**头文件，和真正的原因看不出关系 —— 照着它去查
# vfs.h 会白查很久。所以在任何编译发生之前先拦一道。
#
# 判据只能是文件本身，不能信版本控制的状态字段。历史教训：曾出现过
# "指针文件在、status 不带 '-'、update --init 还 exit 0，但工作树是空的"
# 这种状态，排查代价很高。
#
# 挂载点选在 KERNEL_RULE（见 §11b）而不是顶层的 kernel 目标：那样只能保证
# **链接前**检查，各 .o 的编译（也就是真正报 ext4.h 的地方）早就先炸了。
THIRD_PARTY_HEADERS := $(LWEXT4_DIR)/include/ext4.h \
                       $(LWIP_DIR)/src/include/lwip/init.h \
                       $(FATFS_DIR)/source/ff.h

# 兼容旧命令：本仓库不使用 submodule（lwIP 内嵌于 third_party/，
# lwext4/FatFs 已收编到 fs/）。保留空目标只为别让旧脚本/旧文档报错。
submodules:
	@echo "No submodule fetch is needed: lwIP is embedded, lwext4/FatFs live under fs/."

check-submodules:
	@missing=""; \
	for h in $(THIRD_PARTY_HEADERS); do \
	    [ -e "$$h" ] || missing="$$missing $$h"; \
	done; \
	if [ -n "$$missing" ]; then \
	    echo ""; \
	    echo "════════════════════════════════════════════════════════════════"; \
	    echo " Required sources are missing (lwIP is embedded; lwext4/FatFs now live under fs/)."; \
	    echo " The working tree is incomplete; please re-clone. Missing:"; \
	    for h in $$missing; do echo "     $$h"; done; \
	    echo ""; \
	    echo " Fix (run from the repository root):"; \
	    echo "     re-clone the repository (no submodules are used)"; \
	    echo "════════════════════════════════════════════════════════════════"; \
	    echo ""; \
	    exit 1; \
	fi

# ─── §10  顶层目标声明 ───────────────────────────────────────────────────────────
.PHONY: all clean clean-all help klog kernel run run-net rootfs run-fs run-fs-mbr test-fat test-pthread test-mutex test-vmm test-guest-linux test-ltp env epoll-perf test-epoll-perf racetest task-ipc task-completion dmatest drmtest test-string test-panic rtps rtctl format format-check apidoc apidoc-check submodules check-submodules

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
# check-submodules 是 order-only 依赖：它必须在这个 .o 的编译**之前**跑完，
# 但不该影响 .o 的新旧判断。挂了它才能保证"缺 submodule"时先看到 §9a 的
# 提示，而不是各 .o 那行指向 include/fs/vfs.h 的误导性 ext4.h 报错。
define KERNEL_RULE
$(call kobj,$(1)): $(1) | $(BUILD_DIR) check-submodules
	$$(CC) $(2) -c $$< -o $$@
endef

$(foreach f,$(KERNEL_PLAIN_SRCS),$(eval $(call KERNEL_RULE,$(f),$$(CFLAGS))))
$(foreach f,$(KERNEL_FS_SRCS),$(eval $(call KERNEL_RULE,$(f),$$(FATFS_CFLAGS))))
$(foreach f,$(KERNEL_LWEXT4_SRCS),$(eval $(call KERNEL_RULE,$(f),$$(LWEXT4_CFLAGS))))
$(foreach f,$(KERNEL_LWIP_SRCS),$(eval $(call KERNEL_RULE,$(f),$$(LWIP_CFLAGS))))
$(foreach f,$(KERNEL_LWIPX_SRCS),$(eval $(call KERNEL_RULE,$(f),$$(LWIP_CFLAGS) -I$$(LWEXT4_DIR)/include -I$$(LWEXT4_PORT_DIR))))

$(BUILD_DIR)/tests_%.o: $(UNIT_TEST_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 启动变体对象：GUEST_LINUX / VMM_TEST 选择不同类型；main.c 只认通用钩子。
# 正常模式没有这个对象，main.c 的 weak 默认实现生效。
ifneq ($(TEST_BOOT_OBJ),)
$(TEST_BOOT_OBJ): $(TEST_BOOT_SRC) | $(BUILD_DIR) check-submodules
	$(CC) $(CFLAGS) -c $< -o $@
endif

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

$(foreach f,$(DRIVER_SRCS_CORE),  $(eval $(call DRIVER_RULE,$(f),$$(CFLAGS))))
$(foreach f,$(DRIVER_SRCS_CFLAGS),$(eval $(call DRIVER_RULE,$(f),$$(CFLAGS))))
$(foreach f,$(DRIVER_SRCS_PLIC),  $(eval $(call DRIVER_RULE,$(f),$$(CFLAGS) -Idriver)))
$(foreach f,$(DRIVER_SRCS_TPU),   $(eval $(call DRIVER_RULE,$(f),$$(CFLAGS) -Idriver/tpu)))
$(foreach f,$(DRIVER_SRCS_ION),   $(eval $(call DRIVER_RULE,$(f),$$(CFLAGS) -Idriver/ion -Ikernel/mm)))
$(foreach f,$(DRIVER_SRCS_DMA),   $(eval $(call DRIVER_RULE,$(f),$$(CFLAGS) -Idriver/dma -Ikernel/mm)))
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
$(BOOT_OBJECTS) $(KERNEL_OBJECTS) \
$(PMM_TEST_OBJECT) \
$(TASK_USER_TEST_OBJ) $(TASK_USER_HELLO_OBJ) $(TASK_USER_TESTEXECVE_OBJ) \
$(GUEST_TEST_OBJ) $(TEST_BOOT_OBJ) $(TESTS_OBJECTS) $(PLATFORM_OBJECTS) $(DRIVER_OBJECTS) \
$(EXCEPTION_OBJECTS) $(KLOG_OBJECT) $(VSNPRINTF_OBJECT) $(STRING_OBJECT) \
$(LIBC_OBJECT) $(BITMAP_OBJECT) $(PLATFORM_STATIC_OBJECT) $(LWEXT4_OBJS) $(LWEXT4_PORT_OBJS) \
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


# ── §11d  测试 / 库对象规则 ────────────────────────────────────────────────────

# PMM 测试（唯一来自 tests/ 却参与内核链接的源文件，不在 §4a 的 find 范围内）
PMM_TEST_OBJECT := $(BUILD_DIR)/kernel_mm_pmm_test.o

$(BUILD_DIR)/kernel_mm_pmm_test.o: $(UNIT_TEST_DIR)/pmm_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/bitmap.o: $(LIB_DIR)/bitmap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/platform_static.c: $(_PLATFORM_CONF) $(TOOLS_DIR)/gen_platform.py | $(BUILD_DIR)
	python3 $(TOOLS_DIR)/gen_platform.py $(_PLATFORM_CONF) $(PLATFORM_MK) $(INCLUDE_DIR)

$(BUILD_DIR)/platform_static.o: $(BUILD_DIR)/platform_static.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@


# ── §11e  第三方库编译规则（lwext4 / lwIP）──────────────────────────────────────
# lwext4 第三方源码：用带 compat 路径的专用 LWEXT4_CFLAGS

$(THIRD_PARTY_BUILD_DIR)/lwext4_%.o: $(LWEXT4_DIR)/src/%.c | $(BUILD_DIR) check-submodules
	@mkdir -p $(dir $@)
	$(CC) $(LWEXT4_THIRD_PARTY_CFLAGS) -c $< -o $@

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
$(BUILD_DIR)/lwext4_port_kmalloc.o: $(LWEXT4_PORT_DIR)/kmalloc.c | $(BUILD_DIR) check-submodules
	$(CC) $(CFLAGS) -c $< -o $@

# lwext4 全局串行化的包装层（要 lwext4 的头文件才能写对签名）
$(BUILD_DIR)/lwext4_port_fs_lock.o: $(LWEXT4_PORT_DIR)/fs_lock.c | $(BUILD_DIR) check-submodules
	$(CC) $(LWEXT4_CFLAGS) -I$(LWEXT4_PORT_DIR) -c $< -o $@

# ── §11e-2  FatFs 第三方源码 / 移植胶水 ────────────────────────────────────────
$(THIRD_PARTY_BUILD_DIR)/fatfs_%.o: $(FATFS_DIR)/source/%.c | $(BUILD_DIR) check-submodules
	@mkdir -p $(dir $@)
	$(CC) $(FATFS_THIRD_PARTY_CFLAGS) -c $< -o $@

# diskio.c 是本项目的胶水，警告必须可见（不加 -w）
$(BUILD_DIR)/fatfs_port_diskio.o: $(FATFS_PORT_DIR)/diskio.c | $(BUILD_DIR) check-submodules
	$(CC) $(FATFS_CFLAGS) -c $< -o $@


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
#
# 内核链接是**两趟**的，为的是把函数符号表（kallsyms）嵌进镜像：
#
#   stage-1.elf  ──gen_kallsyms.py──>  kallsyms_<arch>.S  ──>  .o
#        │                                                      │
#        └────────────────────┬─────────────────────────────────┘
#                             ▼
#                      kernel_<arch>.elf （带 .kallsyms 段）
#                             │
#                             └─ gen_kallsyms.py --verify 比对两趟的
#                                (地址, 函数名) 集合是否完全一致
#
# 为什么需要两趟：内核是 `objcopy -O binary` 出 .bin 再交给 QEMU `-kernel`
# 的，镜像里没有符号表；要让 panic backtrace 打出 `func+0x12`，符号表只能
# 编进内核 —— 而表的内容（地址）又依赖链接结果。鸡生蛋。
#
# 为什么两趟的地址对得上：link.ld 把 .kallsyms 放在 .text **之后**（见
# boot/x86_64/link.ld 里的长注释），新增这一段不移动任何函数；表里也只收
# STT_FUNC。--verify 是给这条不变量兜底的 —— 一旦有人挪动段序，构建当场
# 失败，而不是产出二进制正常、函数名整体错位的镜像。
KERNEL_LINK_OBJS := $(BOOT_OBJECTS) $(KERNEL_OBJECTS) $(PMM_TEST_OBJECT) $(LWIP_OBJS) $(TASK_USER_TEST_OBJ) $(TASK_USER_HELLO_OBJ) $(TASK_USER_TESTEXECVE_OBJ) $(GUEST_TEST_OBJ) $(TEST_BOOT_OBJ) $(TESTS_OBJECTS) $(PLATFORM_OBJECTS) $(DRIVER_OBJECTS) $(EXCEPTION_OBJECTS) $(KLOG_OBJECT) $(VSNPRINTF_OBJECT) $(STRING_OBJECT) $(LIBC_OBJECT) $(BITMAP_OBJECT) $(PLATFORM_STATIC_OBJECT) $(LWEXT4_OBJS) $(LWEXT4_PORT_OBJS) $(FATFS_OBJS) $(FATFS_PORT_OBJS)

# 第一趟：链一个**空**符号表桩，只为拿到各函数的最终地址。
# 桩不是可有可无的：.text 里的代码要引用 __kallsyms_*，两趟必须解析到
# "同样形态"的符号，生成出来的指令长度才会一样（见 mk/toolchain.mk 的变量说明）。
$(KERNEL_STAGE1): $(KERNEL_LINK_OBJS) $(KALLSYMS_STUB_OBJ) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -nostartfiles -nodefaultlibs -T $(BOOT_DIR)/$(ARCH)/link.ld -o $@ -Wl,--start-group $^ -Wl,--end-group

$(KALLSYMS_STUB_S): $(TOOLS_DIR)/gen_kallsyms.py | $(BUILD_DIR)
	$(PYTHON) $(TOOLS_DIR)/gen_kallsyms.py --stub $@

$(KALLSYMS_STUB_OBJ): $(KALLSYMS_STUB_S) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 从第一趟的 ELF 抽函数符号表。
# 依赖 gen_kallsyms.py 本身：改脚本要能触发重生成。
$(KALLSYMS_S): $(KERNEL_STAGE1) $(TOOLS_DIR)/gen_kallsyms.py
	$(PYTHON) $(TOOLS_DIR)/gen_kallsyms.py $< $@

$(KALLSYMS_OBJ): $(KALLSYMS_S) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 第二趟：带上符号表。链完立刻校验两趟地址一致 —— 这一步失败说明 .kallsyms
# 的段序被挪了，此时镜像**不能**用来调试，宁可让构建红掉。
$(KERNEL_TARGET): $(KERNEL_LINK_OBJS) $(KALLSYMS_OBJ) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -nostartfiles -nodefaultlibs -T $(BOOT_DIR)/$(ARCH)/link.ld -o $@ -Wl,--start-group $^ -Wl,--end-group
	$(PYTHON) $(TOOLS_DIR)/gen_kallsyms.py --verify $(KERNEL_STAGE1) $@

# 转换为二进制文件
$(KERNEL_BIN): $(KERNEL_TARGET)
	$(OBJCOPY) -O binary $< $@

# 1.44MB 软盘镜像（KERNEL_IMAGE 仅在 x86_64 定义，见 mk/toolchain.mk；
# 其它架构下该变量为空，这条规则不会被注册）
$(KERNEL_IMAGE): $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=1024 count=1440
	dd if=$< of=$@ bs=512 conv=notrunc

include mk/qemu.mk

# ─── §13  清理 / 格式化 / 帮助 ───────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)

clean-all:
	rm -rf $(BUILD_ROOT)/*

include mk/format.mk
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
	@echo "  LOG_MODULES=uart,gic   Whitelist which modules' DEBUG/TRACE logs print (needs LOG=debug|trace)"
	@echo "  LOG=trace     Show all logs including trace"
	@echo ""
	@echo "Assertions:"
	@echo "  Assertions are always enabled (assert/assert_always -> platform_panic)"
	@echo ""
	@echo "Backtrace:"
	@echo "  FP=1          Add -fno-omit-frame-pointer so panic/exception dumps a"
	@echo "                symbolicated call stack (default). Costs a GPR and ~1-3%."
	@echo "  FP=0          Omit frame pointers; backtrace falls back to a heuristic"
	@echo "                stack scan (works, but may miss or invent frames)."
	@echo "  Function names come from an embedded kallsyms table generated out of"
	@echo "  the stage-1 link; see kernel/debug/backtrace.h and docs/basic/BACKTRACE.md"
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
	@echo "  run-fs-mbr    Like run-fs, but with an MBR p1=FAT + p2=ext4 test image"
	@echo "                (requires ROOTFS_MBR=1)"
	@echo "  test-fat      Automated FAT read/write/mkdir/rm regression on the MBR image"
	@echo "                e.g. make PLATFORM=qemu-virt-x86_64 ROOTFS_MBR=1 LOG=info test-fat"
	@echo "  test-pthread  Copy dynamic rootfs from imgs/ and run pthread_test"
	@echo "  test-mutex    Copy dynamic rootfs from imgs/ and run mutex_test (futex-based)"
	@echo "  rtps          Build the userspace RT task viewer (installed as /bin/rtps)"
	@echo "  rtctl         Build the /dev/rtctl RT worker control tool (installed as /bin/rtctl)"
	@echo "  test-vmm      Build with VMM_TEST=1 and run VMM 3-thread switch test"
	@echo "  env           Fetch LTP source → tests/ltp/src (needed by tests/ltp/build.sh)"
	@echo "  test-panic    Build with PANIC_TEST=1: panic on purpose in the ELF"
	@echo "                parser, to see what a backtrace looks like"
	@echo "  test-guest-linux  Boot a Linux guest (aarch64/riscv64/x86_64)"
	@echo "  test-epoll-perf   Boot with /bin/epoll_perf (aarch64/riscv64)"
	@echo "  racetest      Build the concurrency regression tool (installed as /bin/racetest)"
	@echo "  task-ipc      Build the cross-core wakeup test (installed as /bin/task-ipc)"
	@echo "  task-completion  Build the completion A/B test (needs TASK_TEST=1 kernel)"
	@echo "  dmatest       Build the dma-buf/dma-heap test (needs DMA=1)"
	@echo "  drmtest       Build the DRM/GEM test (needs NPU=rknpu)"
	@echo "  submodules    Print a notice: no submodules are used (kept for old scripts)"
	@echo "  clean         Remove build artifacts for current PLATFORM"
	@echo "  clean-all     Remove build artifacts for all platforms"
	@echo "  format        Reformat all project .c/.h with clang-format (.clang-format)"
	@echo "  format-check  Dry-run: exit non-zero if anything is not formatted (CI)"
	@echo "  apidoc        Generate C API reference (docs/api/, rustdoc-style HTML)"
	@echo "  apidoc-check  Check extraction warnings without writing files"
	@echo "  help          Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make PLATFORM=qemu-virt-aarch64 kernel"
	@echo "  make PLATFORM=qemu-virt-riscv64 run-fs LOG=trace"
	@echo "  make PLATFORM=qemu-virt-x86_64 kernel LOG=warn"
	@echo "  make PLATFORM=sg2002-riscv64 kernel LOG=info"
	@echo "  make PLATFORM=qemu-virt-riscv64 clean"
	@echo "  make PLATFORM=qemu-virt-riscv64 test-pthread LOG=warn    # one-shot pthread_test"
	@echo "  make PLATFORM=qemu-virt-aarch64 test-vmm LOG=info        # VMM 3-thread switch test"
	@echo "  make PLATFORM=qemu-virt-aarch64 test-string              # string/memory function self-test"
	@echo "  make ARCH=riscv64 kernel                                 # legacy alias for PLATFORM=qemu-virt-riscv64"
	@echo "  make PLATFORM=qemu-virt-riscv64 run-net"
	@echo "  make PLATFORM=qemu-virt-riscv64 run-net QEMU_NET_FLAGS='-netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56'"

# 头文件依赖：编译时已用 -MMD -MP 生成 build/**.d，但当前未纳入 -include，
# 因此修改头文件不会触发重新编译（改头文件后请手动 make clean）。
# 若要启用，需把所有目标的 .d 汇总成一个变量再 -include 之。
#
# ⚠️ 最常踩这个坑的头文件是 include/klog.h —— 它定义所有 KLOG_* 宏，
# 改它等于改每一个 TU。而 _CFG_SIG（见 §7b 附近）只跟踪 GIC=/SMP=/LOG=/
# LOG_MODULES=，**不跟踪头文件 mtime**，所以不会替你清 .o。
# 后果很隐蔽：新旧宏混在同一个镜像里，输出**看起来完全合理**，
# 只是行数和采样都不对 —— 而日志改动的验收恰恰是按行数比对的。
# 改 klog.h 后务必 make clean 再全量重编。

# ─── 目录索引 ────────────────────────────────────────────────────────────────────
# 节	内容
# §1	基本参数（PLATFORM / ARCH 兼容 / LOG / SMP / 目录）
# §2	平台配置生成（gen_platform.py）
# §3	日志标志（另含 ASSERT 开关已移除的断言）
# §4	源文件与目标文件变量（lib 对象）
# §4a	内核源文件自动发现（find 递归 + 架构目录排除 + vdev 白名单）
# §4a-2	内核编译标志分组（引用第三方头文件的短名单）
# §4b	平台 / 驱动基础源文件、启动与异常源文件
# §5	工具链与编译标志（按架构，见 mk/toolchain.mk）
# §5a	通用编译标志（所有架构共享，见 mk/toolchain.mk）
# §6	驱动选择（选择逻辑已下沉到 driver/drivers.mk；这里只组装对象）
# §7	构建变体（VMM_TEST / GUEST_LINUX / NGINX_TEST）
# §8	第三方库：lwext4 文件系统
# §8a	内核网络栈：netdev + lwIP
# §8b	FAT 后端：FatFs
# §9	Rootfs 配置
# §9a	第三方源码前置检查（工作树完整性）
# §10	顶层目标声明
# §11	构建规则
# §11a	库与通用规则
# §11b	内核 / 测试 / 平台 / 启动规则（由 §4a 的列表 eval 生成）
# §11c	启动 / 异常 / 驱动规则（驱动由 driver/drivers.mk 的列表 eval 生成）
# §11c-1	平台配置依赖声明（换平台时精确触发重编）
# §11d	测试 / 库对象规则
# §11e	第三方库编译规则（lwext4 / lwIP）
# §11e-2	FatFs 第三方源码 / 移植胶水
# §11f	内嵌 guest 测试程序
# §11g	链接（elf → bin → img）
# §12	运行 / 测试目标（见 mk/qemu.mk；rootfs 相关在 mk/rootfs.mk）
# §13	清理 / 格式化 / API 文档 / 帮助
#
# ─── 构建逻辑分片（不在本文件里）────────────────────────────────────────────────
# driver/drivers.mk    驱动选择；新增驱动在这里加进 DRIVER_SRCS_* 组。
# mk/toolchain.mk      三架构 CC/CFLAGS/LDFLAGS/QEMU；改架构标志先看这里。
# mk/apps.mk           用户态程序变量与编译规则；新增 apps/c/*.c 通常无需改这里。
# mk/rootfs.mk         rootfs 配置 / guest 镜像布局 / rootfs、run-fs、test-fat。
# mk/qemu.mk           run/run-net/test-* 等 QEMU 目标。
# mk/format.mk         format、format-check、apidoc、apidoc-check。
# tools/make_rootfs.sh rootfs 镜像的具体安装步骤（由 mk/rootfs.mk 调用）。
#
# 新增内核源文件：放进 kernel/ 下即可，无需改本文件（见 §4a）。
