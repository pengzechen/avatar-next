# mk/sources.mk — 源文件发现、对象映射、第三方头文件分组

# ─── §4  源文件与目标文件变量 ────────────────────────────────────────────────────
# klog 库源文件
KLOG_SOURCES := $(LIB_DIR)/klog.c
VSNPRINTF_SOURCES := $(LIBC_DIR)/vsnprintf.c
STRING_SOURCES := $(LIBC_DIR)/string.c
BITMAP_SOURCES := $(LIB_DIR)/bitmap.c
KLOG_OBJECT         := $(BUILD_DIR)/klog.o
VSNPRINTF_OBJECT    := $(BUILD_DIR)/vsnprintf.o
STRING_OBJECT       := $(BUILD_DIR)/string.o
BITMAP_OBJECT       := $(BUILD_DIR)/bitmap.o
LIBC_SOURCES        := $(LIBC_DIR)/stdlib.c
LIBC_OBJECT         := $(BUILD_DIR)/stdlib.o
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
                        $(KERNEL_DIR)/vmm/vdev/ \
                        $(KERNEL_DIR)/net/lwip/
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
    $(KERNEL_DIR)/linux/exec/elf_loader.c \
    $(KERNEL_DIR)/linux/syscall.c \
    $(KERNEL_DIR)/linux/core/proc_lifecycle.c \
    $(KERNEL_DIR)/linux/fs/fd/fd_pool.c \
    $(KERNEL_DIR)/linux/fs/path/path.c \
    $(KERNEL_DIR)/linux/fs/file/file_io.c \
    $(KERNEL_DIR)/linux/fs/file/file_ops.c \
    $(KERNEL_DIR)/linux/fs/file/file_stat.c \
    $(KERNEL_DIR)/linux/fs/file/dir.c \
    $(KERNEL_DIR)/linux/fs/ioctl.c \
    $(KERNEL_DIR)/linux/fs/char/pipe.c \
    $(KERNEL_DIR)/linux/fs/char/pty.c \
    $(KERNEL_DIR)/linux/io/poll.c \
    $(KERNEL_DIR)/linux/io/select.c \
    $(KERNEL_DIR)/linux/io/epoll.c \
    $(KERNEL_DIR)/linux/mm/mmap.c

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
# ksocket.c → linux/fs/fd/fd_pool.h → include/fs/vfs.h:14 → #include <ext4.h>
KERNEL_LWIPX_SRCS := $(KERNEL_DIR)/linux/net/ksocket.c \
                     $(KERNEL_DIR)/linux/net/sock_syscall.c

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
