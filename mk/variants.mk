# mk/variants.mk — 构建变体、日志模块白名单、变体/配置清理

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
# （wait/complete/reset，见 kernel/linux/syscall.c）。只影响 syscall.o，
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
# 变体 → 条件编译落点。切换方向两个方向都要正确，所以这里取所有变体的
# **并集**（不按当前开关筛选）。新增变体时在表里加一行，不要直接写对象名。
# 注意 rt_irq_trace 只改 CFLAGS、不落任何源文件的 #if，已由 _CFG_SIG 全量清理。
_VARIANT_SRC_TABLE := \
    guest_linux=$(KERNEL_DIR)/main.c \
    vmm_test=$(KERNEL_DIR)/main.c \
    nginx_test=$(KERNEL_DIR)/net/net.c \
    string_test=$(KERNEL_DIR)/main.c \
    task_test=$(KERNEL_DIR)/linux/syscall.c \
    rt_test=$(KERNEL_DIR)/main.c \
    rt_demo=$(KERNEL_DIR)/main.c \
    pmm_test=$(UNIT_TEST_DIR)/unit_test.c \
    kmem_test=$(UNIT_TEST_DIR)/unit_test.c \
    panic_test=$(KERNEL_DIR)/mm/elf_image.c

_VARIANT_SRCS := $(sort $(foreach e,$(_VARIANT_SRC_TABLE),$(word 2,$(subst =, ,$(e)))))
# kernel/ 的源文件走 kobj（扁平成 kernel_*），tests/unit-test/ 走 tests_*。
_variant_obj = $(if $(filter $(UNIT_TEST_DIR)/%,$(1)),\
                  $(BUILD_DIR)/tests_$(notdir $(basename $(1))).o,\
                  $(call kobj,$(1)))
_VARIANT_FILE := $(BUILD_DIR)/.build_variant
_VARIANT_OBJS := $(strip $(foreach f,$(_VARIANT_SRCS),$(call _variant_obj,$(f))) \
                 $(TEST_BOOT_OBJ))
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
