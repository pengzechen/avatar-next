# mk/qemu.mk — QEMU 运行目标与便捷测试目标
# 由顶层 Makefile 在 mk/rootfs.mk 之后 include；rootfs/run-fs/test-fat 在 rootfs.mk。

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

# test-string: 字符串/内存函数自检（tests/unit-test/string_test.c）
#              长度 × 源对齐 × 目的对齐 全组合，跨过 NEON 阈值两侧，
#              跑完打印 "=== STRING TEST: PASS (N cases) ===" 后继续正常启动。
#   用法: make PLATFORM=qemu-virt-aarch64 test-string
test-string:
	$(MAKE) PLATFORM=$(PLATFORM) LOG=$(LOG) STRING_TEST=1 kernel
	@echo "Starting QEMU (string self-test)..."
	$(QEMU) $(QEMU_FLAGS)

# test-panic: 在 ELF 解析路径上**故意 panic 一次**，用来看调用栈回溯的效果。
#
#   用法: make PLATFORM=qemu-virt-x86_64 test-panic
#
# 触发点在 kernel/mm/elf_image.c 的 ELF 头部校验之后（PANIC_TEST=1 时才
# 编译进来），打出来的是一条真实的加载器调用链：
#   platform_panic ← elf_image_load_impl ← elf_image_load ← exec → …
# 设计说明与已知局限见 docs/basic/BACKTRACE.md。
#
# 需要 rootfs：触发点是"解析 /busybox 的 ELF 头"，没有文件系统就走不到那里
# （会先在 open 那一步失败退出）。所以这里和 test-pthread 一样先摆好镜像。
test-panic:
	@if [ ! -f imgs/rootfs-$(ARCH).img ]; then \
		echo "ERROR: imgs/rootfs-$(ARCH).img not found."; \
		echo "Run: bash apps/c/build.sh"; \
		exit 1; \
	fi
	$(MAKE) PLATFORM=$(PLATFORM) LOG=$(LOG) PANIC_TEST=1 kernel
	@cp imgs/rootfs-$(ARCH).img $(ROOTFS_IMG)
	@echo "Starting QEMU (PANIC_TEST: panic on purpose in the ELF parser path to show a backtrace)..."
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS)

# test-guest-linux: 编译 GUEST_LINUX=1 内核并把 Linux 作为 EL1 guest 启动
#                   rootfs 会自动安装 /guests/linux/{linux.bin,linux.dtb,initrd.gz}
# ⚠️ `$(ROOTFS_IMG)` 这个依赖不能少：三个架构的 guest 镜像
# （/guests/rv64/*、/guests/linux/*、/guests/x86_64/*）都是**打进 rootfs 镜像**、
# 再由 guest_loader 从镜像里读出来的。少了它，镜像不存在，QEMU 直接报
#     -device loader,file=.../rootfs-<arch>.img: Cannot load specified image
# （注意这条报错是 QEMU 打给 stderr 的，看起来很像"guest 起不来"，
#  实际是文件压根没生成）。曾经在给 x86_64 加分支时误删过一次。
test-guest-linux: $(ROOTFS_IMG)
	@if [ "$(ARCH)" != "aarch64" ] && [ "$(ARCH)" != "riscv64" ] && [ "$(ARCH)" != "x86_64" ]; then \
		echo "ERROR: test-guest-linux supports ARCH=aarch64, riscv64 or x86_64."; \
		exit 1; \
	fi
ifeq ($(ARCH),x86_64)
	@# x86_64：**rootfs 必须和内核用同一个变体标志构建** ——
	@# make rootfs 在 GUEST_LINUX=1 时才会把 /guests/x86_64/{bzImage,initrd}
	@# 装进镜像；而它同时会用当前命令行的变体重编内核（见 CLAUDE.md 的警告）。
	@# 所以顺序是：同变体先 rootfs，再 kernel（kernel 放最后），最后校验产物。
	$(MAKE) PLATFORM=$(PLATFORM) LOG=$(LOG) GUEST_LINUX=1 rootfs
	$(MAKE) PLATFORM=$(PLATFORM) LOG=$(LOG) GUEST_LINUX=1 kernel
	@# 判据是 kernel/main.c 里那个 __attribute__((used)) 的构建指纹，
	@# **不是**任何 KLOG 字符串 —— LOG=none 会把 KLOG 全编译掉，
	@# 拿它当判据会在正确的产物上误报。见 main.c 里的注释。
	@if [ "$$(strings $(KERNEL_BIN) | grep -c 'GUEST_LINUX_BUILD_TAG')" != "1" ]; then \
		echo "ERROR: the artifact is not a GUEST_LINUX variant (it will be overwritten by make rootfs)."; \
		echo "       Follow CLAUDE.md: build rootfs first, then kernel, with the same variant."; \
		exit 1; \
	fi
else
	$(MAKE) PLATFORM=$(PLATFORM) LOG=$(LOG) GUEST_LINUX=1 kernel
endif
	@echo "Starting QEMU (guest Linux, $(ARCH))..."
	@echo "  Note: the guest serial console is this terminal; press Ctrl-A X to exit QEMU."
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS)

# env: 拉取测试依赖（LTP 源码 → tests/ltp/src）。
#
# 全新 clone 里没有 LTP 源码（434MB，不入库），必须先跑这个，之后
# `make -C tests ltp-build` 才能交叉编译 LTP 测例；`make -C tests ltp-build`
# 也会自动先跑 env。
#
# 用法: make env
#       make -C tests env LTP_VERSION=20260529
env:
	$(MAKE) -C tests env

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
