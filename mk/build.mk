# mk/build.mk — 顶层目标、编译/链接规则、kallsyms、clean

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

clean:
	rm -rf $(BUILD_DIR)

clean-all:
	rm -rf $(BUILD_ROOT)/*
