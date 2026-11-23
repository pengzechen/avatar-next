# mk/toolchain.mk — 三架构工具链、CFLAGS/LDFLAGS 与 QEMU 基础参数
# 由顶层 Makefile 在源文件发现之后 include。所有变量仍是全局的，后续规则
# （§6 驱动 / §11 编译链接）直接在 recipe 里引用。
#
# 只把「三架构相同的部分」提到公共段，各架构块只保留真正的差异；
# 改架构相关标志时先确认 CFLAGS 的追加顺序（§7 的 _CFG_SIG 依赖最终值）。

X86_64_TOOL_PREFIX ?= $(if $(shell command -v x86_64-linux-musl-gcc 2>/dev/null),x86_64-linux-musl-,)

ifeq ($(ARCH),x86_64)
    TOOL_PREFIX := $(X86_64_TOOL_PREFIX)
    ARCH_DEFINE := X86_64
else ifeq ($(ARCH),aarch64)
    TOOL_PREFIX := aarch64-linux-musl-
    ARCH_DEFINE := AARCH64
else ifeq ($(ARCH),riscv64)
    TOOL_PREFIX := riscv64-linux-musl-
    ARCH_DEFINE := RISCV64
else
    $(error Unsupported architecture: $(ARCH). Use ARCH=x86_64, aarch64 or riscv64)
endif

CC      := $(TOOL_PREFIX)gcc
AR      := $(TOOL_PREFIX)ar
OBJCOPY := $(TOOL_PREFIX)objcopy
NM      := $(TOOL_PREFIX)nm

# ── 公共标志（架构块只追加差异）────────────────────────────────────────────
CFLAGS  := -Wall -Wextra -O2 -g
CFLAGS  += -DARCH_$(ARCH_DEFINE)=1
CFLAGS  += -I$(INCLUDE_DIR)
CFLAGS  += $(LOG_DEFINE)
CFLAGS  += -ffreestanding -fno-builtin

KLOG_TARGET   := $(BUILD_DIR)/libklog_$(ARCH).a
KERNEL_TARGET := $(BUILD_DIR)/kernel_$(ARCH).elf
KERNEL_BIN    := $(BUILD_DIR)/kernel_$(ARCH).bin

# LDFLAGS 先清空，各架构块只追加自己的那部分。
LDFLAGS :=

ifeq ($(ARCH),x86_64)
    CFLAGS  += -fno-pie -fno-stack-protector -fno-stack-clash-protection -U_FORTIFY_SOURCE
    CFLAGS  += -mcmodel=large -mno-red-zone
    CFLAGS  += -mno-mmx -mno-sse
    # 内核高半区偏移与物理加载基址只在这里定义一次：C/汇编经 -D 拿到，
    # link.ld 经 --defsym 拿到。物理基址 = RAM 基址 + lowmem 保留区
    # （platform.conf 的 lowmem_1m_2m）。
    X86_KERNEL_VMA_OFF      ?= 0xffff800000000000
    X86_LOWMEM_RESERVE_SIZE ?= 0x100000
    X86_KERNEL_LOAD_PHYS := $(shell printf '0x%x' \
        $$(( $(MEM_RAM_BASE) + $(X86_LOWMEM_RESERVE_SIZE) )))
    CFLAGS  += -DX86_KERNEL_VMA_OFF=$(X86_KERNEL_VMA_OFF)
    CFLAGS  += -DX86_KERNEL_LOAD_PHYS=$(X86_KERNEL_LOAD_PHYS)
    LDFLAGS += -nostdlib -nostartfiles -nodefaultlibs -no-pie
    LDFLAGS += -Wl,-z,max-page-size=0x1000
    LDFLAGS += -Wl,--defsym=KERNEL_VMA_OFF=$(X86_KERNEL_VMA_OFF)
    LDFLAGS += -Wl,--defsym=KERNEL_LOAD_PHYS=$(X86_KERNEL_LOAD_PHYS)
    KERNEL_IMAGE := $(BUILD_DIR)/kernel_x86_64.img
    QEMU          := qemu-system-x86_64
    QEMU_FLAGS    := -machine q35 -enable-kvm -cpu host -smp $(SMP) -m 2G -nographic -kernel $(KERNEL_BIN)
    QEMU_RUN_NET_FLAGS ?= -machine microvm -enable-kvm -cpu host -smp $(SMP) -m 2G -nographic -kernel $(KERNEL_BIN)
else ifeq ($(ARCH),aarch64)
    CFLAGS  += -fno-pie
    # 允许 GCC 生成 FP/SIMD（NEON）指令（riscv64 靠 -march=rv64gc -mabi=lp64d 达
    # 到同样效果）。代价是内核随时可能占用 v0-v31，所以必须有两层保存，缺一不可：
    #   boot/aarch64/exception.S     陷阱帧保存 q0-q31 + FPCR/FPSR（保护 EL0 用户态）
    #   kernel/task/aarch64/switch.S 任务切换保存 d8-d15 + FPCR/FPSR（AAPCS64 被调用者保存）
    # 限制：内核里的浮点只能用 float/double —— 未链接 libgcc，long double（128 位）
    # 的运算会引出 __addtf3/__divtf3 等帮助函数，在链接期报未定义符号。
    # 详见 docs/arch/aarch64/FP_SIMD_CONTEXT.md
    CFLAGS  += -mno-outline-atomics  # freestanding：禁止 GCC outline atomics 调用 libgcc 帮助函数
    # 高半区偏移只在这里定义一次；link.ld 与 mem_layout.h 都从这里取。
    AARCH64_KERNEL_VMA_OFF ?= 0xffff000000000000
    CFLAGS  += -DAARCH64_KERNEL_VMA_OFF=$(AARCH64_KERNEL_VMA_OFF)
    KERNEL_LINK_ADDR ?= 0xffff000040080000
    LDFLAGS += -Wl,--defsym=KERNEL_LINK_ADDR=$(KERNEL_LINK_ADDR)
    LDFLAGS += -Wl,--defsym=KERNEL_VMA_OFF=$(AARCH64_KERNEL_VMA_OFF)
    QEMU          := qemu-system-aarch64
    # QEMU virt 默认 gic-version=2（`-machine virt,dumpdtb=…` 实测确认）。
    # 内核按 GIC=v3 编译时物理 GIC 也必须是 v3：guest DTB 声明的是
    # arm,gic-v3（GICD + GICR，CPU interface 走系统寄存器），而 QEMU 给的是
    # GICv2（GICD + GICC/GICH/GICV MMIO）——两边对不上，guest 的 GIC
    # 初始化会直接卡死。
    _QEMU_MACHINE_V3 := virt,virtualization=on,gic-version=3
    _QEMU_MACHINE_V2 := virt,virtualization=on
    QEMU_MACHINE  := $(if $(filter v3,$(GIC_VER)),$(_QEMU_MACHINE_V3),$(_QEMU_MACHINE_V2))
    QEMU_FLAGS    := -cpu cortex-a76 -M $(QEMU_MACHINE) -smp $(SMP) -m 2G -nographic -kernel $(KERNEL_BIN)
else ifeq ($(ARCH),riscv64)
    CFLAGS  += -march=rv64gc -mabi=lp64d
    CFLAGS  += -mcmodel=medany
    CFLAGS  += -fno-pic -fno-pie
    # 内核高半区基址只在这里定义一次：C/汇编经 -D 拿到，link.ld 经
    # --defsym 拿到。mem_layout.h 里的 fallback 仅为单独包含该头的场合。
    RV_KERNEL_VMA_OFF       ?= 0xffffffc000000000
    RV_OPENSBI_RESERVE_SIZE ?= 0x200000
    RV_KERNEL_LOAD_BASE := $(shell printf '0x%x' \
        $$(( $(MEM_RAM_BASE) + $(RV_OPENSBI_RESERVE_SIZE) )))
    CFLAGS  += -DRV_KERNEL_VMA_OFF=$(RV_KERNEL_VMA_OFF)
    # -no-pie：让链接器输出非 PIE 静态可执行文件，避免生成
    # R_RISCV_RELATIVE 重定位条目（裸机内核无动态链接器处理它们）
    LDFLAGS += -no-pie
    LDFLAGS += -Wl,--defsym=KERNEL_LOAD_BASE=$(RV_KERNEL_LOAD_BASE)
    LDFLAGS += -Wl,--defsym=KERNEL_VMA_OFF=$(RV_KERNEL_VMA_OFF)
    QEMU          := qemu-system-riscv64
    QEMU_FLAGS    := -M virt -smp $(SMP) -m 2G -nographic -bios default -kernel $(KERNEL_BIN)
endif

# 三架构相同的 QEMU 网络参数。
QEMU_NET_FLAGS ?= -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56

# ⚠️ 必须加在架构块之后、§8 (`LWEXT4_CFLAGS := $(CFLAGS)`) 之前：
#   - 架构块里是 `CFLAGS := ...`，会覆盖掉更早 append 的内容；
#   - guest_loader.c 走 LWEXT4_CFLAGS，而那个变量是对 CFLAGS 的快照，
#     晚于它 append 的宏不会进 guest_loader.c 的编译命令（实测 nr_vcpus
#     仍是 1）。
CFLAGS += -DGUEST_LINUX_NR_VCPUS=$(GUEST_VCPUS)

# 内嵌函数符号表（kallsyms）的产物路径。三架构同名不同目录，所以规则只在
# §11g 写一份。用途和两趟链接的理由见 §11g 顶部注释。
KERNEL_STAGE1 := $(KERNEL_TARGET:.elf=.stage1.elf)
KALLSYMS_S    := $(BUILD_DIR)/kallsyms_$(ARCH).S
KALLSYMS_OBJ  := $(KALLSYMS_S:.S=.o)
# stage-1 用的空表桩（count=0）。**必须有**，不能靠"不链接"或 C 侧弱符号：
# 那样两趟的符号解析不同，C 侧生成出来的代码长度会差几个字节，.text 一漂
# 符号表就全错位。详见 tools/gen_kallsyms.py 里 emit_stub()。
KALLSYMS_STUB_S   := $(BUILD_DIR)/kallsyms_stub.S
KALLSYMS_STUB_OBJ := $(KALLSYMS_STUB_S:.S=.o)

# ── §5a  通用编译标志（所有架构共享，追加在架构特定 CFLAGS 之后）────────────────

# 帧指针：三架构通用，放在这里而不是各架构块里各写一遍。
# -O2 下 GCC 默认省掉帧指针，那样 backtrace 就只能靠栈扫描；
# 见 §1 的 FP= 说明和 kernel/debug/backtrace.c。
ifeq ($(FP),1)
    CFLAGS += -fno-omit-frame-pointer
endif

CFLAGS  += -nostdinc
CFLAGS  += -I$(INCLUDE_DIR)/libc
CFLAGS  += -Idriver
CFLAGS  += -Ikernel
CFLAGS  += -Ikernel/mm
CFLAGS  += -DAVATAR_HAS_FILESYSTEM

# ── FAT 分区挂载点（MBR 盘上的 FAT 分区挂到这里）───────────────────────────
# 必须在本行之后、§7 的 _CFG_SIG 之前定义，并且 CFLAGS 要早于 §8 的
# LWEXT4_CFLAGS 快照；改这个值会触发全量重编。
FAT_MOUNT_POINT ?= /boot
$(if $(filter /%,$(FAT_MOUNT_POINT)),,$(error FAT_MOUNT_POINT must be an absolute path))
$(if $(filter /,$(FAT_MOUNT_POINT)),$(error FAT_MOUNT_POINT must not be /))
$(if $(filter %/,$(FAT_MOUNT_POINT)),$(error FAT_MOUNT_POINT must not end with /))
$(if $(filter /dev /dev/% /proc /proc/% /sys /sys/%,$(FAT_MOUNT_POINT)),\
    $(error FAT_MOUNT_POINT must not be under /dev, /proc or /sys))
CFLAGS  += -DFAT_MOUNT_POINT=\"$(FAT_MOUNT_POINT)\"

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
