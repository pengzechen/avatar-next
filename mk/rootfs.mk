# mk/rootfs.mk — rootfs 配置、guest 镜像布局与镜像/运行目标
# 由顶层 Makefile 在 mk/apps.mk 之后 include。

# ─── §9  Rootfs 配置 ────────────────────────────────────────────────────────────
# 每个平台独立一个 build/<platform>/ 目录，切换平台无需 make clean。
# ROOTFS_MBR=1 生成 p1 FAT + p2 ext4 的 MBR 镜像（默认仍是裸 ext4）。
# 用独立文件名，避免和默认镜像互相覆盖（变体标志的坑见 CLAUDE.md）。
ROOTFS_MBR       ?= 0
ifeq ($(filter $(ROOTFS_MBR),0 1),)
$(error Invalid ROOTFS_MBR '$(ROOTFS_MBR)'. Use 0 or 1)
endif
# 16 MiB 用 FAT16；FAT32 的最小卷约 33 MiB。aarch64 的 rootfs 内容较大，
# 测试镜像给 FAT 留 16 MiB、其余全给 ext4。
# ⚠️ 改 FAT_PART_MB 不会让已存在的 rootfs-mbr-*.img 过期（make 不跟踪变量值）：
#    改完先 `rm -f $(BUILD_DIR)/rootfs-mbr-*.img` 或 `make clean`。
FAT_PART_MB      ?= 16
ROOTFS_IMG       := $(BUILD_DIR)/rootfs$(if $(filter 1,$(ROOTFS_MBR)),-mbr,)-$(ARCH).img
ROOTFS_STAGE     := $(BUILD_DIR)/rootfs-stage-$(ARCH)

LTP_BIN_DIR      := tests/ltp/bin/$(ARCH)
LTP_BINS         := $(wildcard $(LTP_BIN_DIR)/*)

# guest 的 DTB 必须与内核用的 GIC 版本匹配：GICv2 的 DTB 里是
# arm,cortex-a15-gic（GICD + GICC 两段 reg），GICv3 的是 arm,gic-v3
# （GICD + GICR，且 CPU interface 走系统寄存器）。装错版本 guest 会在
# GIC 初始化阶段直接卡死。
#
# GUEST_NPU=1 换成带 NPU 节点的那一版（多 npu@fdab0000 + 48 MiB
# reserved-memory）。**默认关**，理由有两条：
#   ① reserved-memory 会从 guest RAM 顶部挖走 0x79000000..0x7C000000 给
#      NPU DMA 用 —— 在没有 NPU 的平台上（QEMU）这块地址纯属白挖，
#      而 guest 的 npu 驱动还会 probe 一个不存在的设备、刷一屏报错。
#   ② 那条 window 要求宿主 stage-2 做**恒等映射**，而恒等映射只在
#      PLATFORM_RK3588 下打开（见 include/vmm/guest_loader.h 的
#      GUEST_LINUX_NPU_DMA_GPA）。不匹配时 guest 拿到的"物理地址"
#      在宿主侧指向别的内存 —— 结果是 NPU 读到垃圾，不报错。
# 完整的链路与三处地址的一致性见 docs/vmm/AARCH64_GUEST_NPU.md。
ifeq ($(GUEST_NPU),1)
  # 判据是**平台**（MEM_PLATFORM_DEFINE 由 build/$(PLATFORM)/platform.mk 生成，
  # 正是喂给 -DPLATFORM_RK3588 的那个），不是 GIC 版本。
  # ⚠️ 别把这条写成"GIC 必须是 v3"：QEMU 的 aarch64 也是 v3，那样写 QEMU
  #    会一路放行 —— 而 QEMU 上宿主 PA 0x79000000 根本不是 RAM，
  #    guest 的每次 NPU DMA 都会撞到不存在的物理地址。
  ifneq ($(MEM_PLATFORM_DEFINE),RK3588)
    $(error GUEST_NPU=1 is only valid for RK3588: that DTB's reserved-memory needs an\
identity mapping in host stage-2, and the identity window is only enabled with PLATFORM_RK3588)
  endif
  ifneq ($(filter v3,$(GIC_VER)),v3)
    $(error GUEST_NPU=1 requires GIC=v3: the NPU guest DTB is GICv3-only)
  endif
endif
# aarch64 的 guest DTB 选择。GICv3/NPU 这两条校验是 aarch64 专属的：
# riscv64 没有 GIC，多核 DTB 是 linux-smpN.dtb（见下面的 RISCV_GUEST_DTBS），
# 不能被这里的 `GUEST_VCPUS>1 requires GIC=v3` 误伤。
ifeq ($(ARCH),aarch64)
# 单核基础 DTB（NPU 版 / GICv3 版 / GICv2 版），与 GUEST_VCPUS 无关。
# 运行期 `vmm-run -c 1` 在“构建默认是多核”的机器上就靠它。
GUEST_LINUX_BASE_DTB_SRC := $(if $(GUEST_NPU),imgs/guests/aarch64/linux-gicv3-npu.dtb,$(if $(filter v3,$(GIC_VER)),imgs/guests/aarch64/linux-gicv3.dtb,imgs/guests/aarch64/linux.dtb))

ifneq ($(GUEST_VCPUS),1)
  ifneq ($(filter v3,$(GIC_VER)),v3)
    $(error GUEST_VCPUS=$(GUEST_VCPUS) requires GIC=v3: multi-CPU guest DTBs are GICv3-only)
  endif
  ifneq ($(GUEST_NPU),)
    $(error GUEST_VCPUS>1 is incompatible with GUEST_NPU=1: the NPU guest DTB is single-vCPU only)
  endif
  GUEST_LINUX_DTB_SRC := imgs/guests/aarch64/linux-gicv3-smp$(GUEST_VCPUS).dtb
else
  GUEST_LINUX_DTB_SRC := $(GUEST_LINUX_BASE_DTB_SRC)
endif

# 运行期按 vCPU 数选 DTB（vmm-run -c N）时用的两份额外资源：
#
#   linux-1cpu.dtb   真正的单核 DTB。GUEST_VCPUS=8 的构建里 linux.dtb 是
#                    8 核版本，`-c 1` 拿它会让 guest 去 CPU_ON 不存在的核。
#   smp2/4/8/12      N>1 的候选；只有 GICv3 版（GIC=v2 构建不装，内核的
#                    guest_loader_check_vcpus() 也会拒绝 N>1）。
#
# 注意 GUEST_LINUX_BASE_DTB_SRC 在 GUEST_VCPUS=1 时与 DTB_SRC 相同，rootfs
# 里会装两份同名内容的单核 DTB —— 4 KB 级别，换来运行期选核的确定性。
ifneq ($(filter v3,$(GIC_VER)),v3)
  GUEST_LINUX_MULTI_DTBS :=
else
  GUEST_LINUX_MULTI_DTBS := imgs/guests/aarch64/linux-gicv3-smp2.dtb \
                            imgs/guests/aarch64/linux-gicv3-smp4.dtb \
                            imgs/guests/aarch64/linux-gicv3-smp8.dtb \
                            imgs/guests/aarch64/linux-gicv3-smp12.dtb
endif
else
GUEST_LINUX_BASE_DTB_SRC :=
GUEST_LINUX_DTB_SRC :=
GUEST_LINUX_MULTI_DTBS :=
endif

# RISC-V 多核 DTB。由 tools/gen_rv64_smp_dtb.py 从 linux.dts 的同一份模板
# 生成（N 个 cpu@i + PLIC 的 N 个 S-mode context），已签入仓库。
# 运行期选核规则见 guest_loader.c 的 guest_loader_dtb_path()。
RISCV_GUEST_SMP_DTBS := imgs/guests/rv64/linux-smp2.dtb \
                        imgs/guests/rv64/linux-smp4.dtb \
                        imgs/guests/rv64/linux-smp8.dtb \
                        imgs/guests/rv64/linux-smp12.dtb

ifneq ($(filter $(ARCH),aarch64 riscv64 x86_64),)
ifeq ($(ARCH),x86_64)
# x86 guest 镜像：bzImage（自编 linux-6.2.15，带 HdrS 64 位入口）+ initramfs
# 源文件叫 initrd.gz 但内容是**裸 cpio**（和 rv64 一样，别再纠结后缀）；
# rootfs 里仍落名 initrd —— 必须和 guest_loader.h 的 GUEST_LINUX_INITRD_PATH 一致。
GUEST_LINUX_FILES := imgs/guests/x86_64/bzImage imgs/guests/x86_64/initrd.gz
else ifeq ($(ARCH),aarch64)
GUEST_LINUX_FILES := imgs/guests/aarch64/linux.bin $(GUEST_LINUX_DTB_SRC) \
                     $(GUEST_LINUX_BASE_DTB_SRC) $(GUEST_LINUX_MULTI_DTBS) \
                     imgs/guests/aarch64/initrd.gz
else
# RISC-V 的 guest 镜像（DTB 是 imgs/guests/rv64/linux.dts 用 dtc 生成的，见该文件头；
# linux-smpN.dtb 由 tools/gen_rv64_smp_dtb.py 生成，运行期 -c N 按 N 选）
GUEST_LINUX_FILES := imgs/guests/rv64/linux.bin imgs/guests/rv64/linux.dtb \
                     $(RISCV_GUEST_SMP_DTBS) imgs/guests/rv64/initrd.gz
endif
else
GUEST_LINUX_FILES :=
endif

# 切换 GIC=v2|v3、GUEST_NPU=0|1 或 GUEST_VCPUS 时 $(GUEST_LINUX_DTB_SRC) 会换文件，但换个更旧
# 的文件不会让 rootfs 的 mtime 落后，make 就不会重建镜像 —— 于是 rootfs 里留着
# 上一版的 DTB。用带变体后缀的戳文件把这个变化显式暴露给 make。
#
# GUEST_NPU 也必须进变体名：两版的 DTB 大小只差 629 字节，装错的那一版
# 会让 guest 要么没有 npu 节点（跑不了 NPU），要么白挖 48 MiB RAM，
# 而 rootfs 本身看起来完全正常。
GUEST_GIC_STAMP := $(BUILD_DIR)/.guest-gic-$(if $(filter v3,$(GIC_VER)),v3,v2)$(if $(GUEST_NPU),-npu,)$(if $(filter-out 1,$(GUEST_VCPUS)),-vcpus$(GUEST_VCPUS),)
$(GUEST_GIC_STAMP):
	@rm -f $(BUILD_DIR)/.guest-gic-*
	@touch $@
# ROOTFS_SIZE_MB / ROOTFS_PHYS_ADDR 来自自动生成的 $(PLATFORM_MK)
QEMU_ROOTFS_FLAGS = -device loader,file=$(ROOTFS_IMG),addr=$(ROOTFS_PHYS_ADDR),force-raw=on

# 创建 ext4 rootfs 镜像（无需 sudo）
# 依赖：Host 已安装 e2fsprogs（mkfs.ext4 >= 1.43 支持 -d 选项）
# 每次 apps 变动时自动重建；切换架构直接使用各自的镜像文件，无需 make clean。
# 安装 / guest / LTP / dropbear 等细节在 tools/make_rootfs.sh，配置经环境变量传入。
$(ROOTFS_IMG): $(MAKEFILE_LIST) $(PLATFORM_CONFIG_DEPS) $(TOOLS_DIR)/make_rootfs.sh $(APPS_BINS) $(APPS_C_ELFS) $(LTP_BINS) $(EPOLL_PERF_BIN) $(RACETEST_BIN) $(TASKIPC_BIN) $(TASKCOMP_BIN) $(RTPS_BIN) $(RTCTL_BIN) $(RTEVENT_BIN) $(RTDEMO_USER_BIN) $(DMATEST_BIN) $(DRMTEST_BIN) $(VMM_RUN_BIN) $(NGINX_BIN) $(GUEST_LINUX_FILES) $(GUEST_GIC_STAMP) | $(BUILD_DIR)
	@ARCH="$(ARCH)" \
	 ROOTFS_STAGE="$(ROOTFS_STAGE)" \
	 ROOTFS_SIZE_MB="$(ROOTFS_SIZE_MB)" \
	 ROOTFS_MBR="$(ROOTFS_MBR)" \
	 FAT_PART_MB="$(FAT_PART_MB)" \
	 FAT_MOUNT_POINT="$(FAT_MOUNT_POINT)" \
	 BUILD_DIR="$(BUILD_DIR)" \
	 LTP_BIN_DIR="$(LTP_BIN_DIR)" \
	 VMM_RUN_BIN="$(VMM_RUN_BIN)" \
	 EPOLL_PERF_BIN="$(EPOLL_PERF_BIN)" \
	 RACETEST_BIN="$(RACETEST_BIN)" \
	 RTPS_BIN="$(RTPS_BIN)" \
	 RTCTL_BIN="$(RTCTL_BIN)" \
	 RTEVENT_BIN="$(RTEVENT_BIN)" \
	 RTDEMO_USER_BIN="$(RTDEMO_USER_BIN)" \
	 TASKIPC_BIN="$(TASKIPC_BIN)" \
	 TASKCOMP_BIN="$(TASKCOMP_BIN)" \
	 DMATEST_BIN="$(DMATEST_BIN)" \
	 DRMTEST_BIN="$(DRMTEST_BIN)" \
	 NGINX_BIN="$(NGINX_BIN)" \
	 RISCV_GUEST_SMP_DTBS="$(RISCV_GUEST_SMP_DTBS)" \
	 GUEST_LINUX_DTB_SRC="$(GUEST_LINUX_DTB_SRC)" \
	 GUEST_LINUX_BASE_DTB_SRC="$(GUEST_LINUX_BASE_DTB_SRC)" \
	 GUEST_LINUX_MULTI_DTBS="$(GUEST_LINUX_MULTI_DTBS)" \
	 $(TOOLS_DIR)/make_rootfs.sh "$@"

rootfs: $(ROOTFS_IMG)
	@echo "Rootfs up to date: $(ROOTFS_IMG)"

# 运行内核 + 加载 rootfs 镜像到 QEMU 客户机内存
run-fs: kernel $(ROOTFS_IMG)
	@echo "Starting QEMU for $(ARCH) with rootfs at $(ROOTFS_PHYS_ADDR)..."
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS)

# 用 MBR 测试镜像启动（p1 FAT + p2 ext4），给手工验证用
run-fs-mbr: kernel $(ROOTFS_IMG)
	@if [ "$(ROOTFS_MBR)" != "1" ]; then \
	    echo "ERROR: use ROOTFS_MBR=1 (this target uses the MBR test image)."; exit 1; \
	fi
	$(QEMU) $(QEMU_FLAGS) $(QEMU_ROOTFS_FLAGS)

# test-fat: 自动构建 MBR 镜像并跑 FAT 读写/目录操作回归（x86_64 最快）
test-fat: kernel $(ROOTFS_IMG)
	@if [ "$(ROOTFS_MBR)" != "1" ]; then \
	    echo "ERROR: use ROOTFS_MBR=1, e.g."; \
	    echo "  make PLATFORM=qemu-virt-x86_64 ROOTFS_MBR=1 LOG=info test-fat"; \
	    exit 1; \
	fi
	KERNEL_BIN=$(KERNEL_BIN) ROOTFS_IMG=$(ROOTFS_IMG) ROOTFS_ADDR=$(ROOTFS_PHYS_ADDR) \
	    tools/fat_regress.sh $(ARCH)
