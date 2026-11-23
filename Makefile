# Avatar OS — 顶层 Makefile
# 用法: make PLATFORM=<platform> [LOG=none|error|warn|info|debug|trace] [target]
# 快速参考: make help
#
# 顶层只保留「装配顺序」：参数/源文件/工具链 → 驱动 → 变体 → 第三方 →
# rootfs → 编译链接 → QEMU/测试 → 格式化/帮助。各片段的职责见文件头。
include mk/config.mk
include mk/sources.mk
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
include mk/variants.mk
include mk/third_party.mk
include mk/rootfs.mk
include mk/build.mk
include mk/qemu.mk
include mk/format.mk
include mk/help.mk

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

# ─── 片段索引 ───────────────────────────────────────────────────────────────────
# driver/drivers.mk    驱动选择；新增驱动在这里加进 DRIVER_SRCS_* 组。
# mk/config.mk         PLATFORM/ARCH/LOG/SMP/FP/目录、platform.mk 生成、默认目标。
# mk/sources.mk        kernel/ 自动发现、对象映射、lwext4/lwIP/FatFs 头文件分组。
# mk/toolchain.mk      三架构 CC/CFLAGS/LDFLAGS/QEMU；改架构标志先看这里。
# mk/apps.mk           用户态程序变量与编译规则；新增 apps/c/*.c 通常无需改这里。
# mk/variants.mk       GUEST_LINUX/VMM_TEST/...、LOG_MODULES、变体与配置戳。
# mk/third_party.mk    lwext4/lwIP/FatFs 变量、--wrap 列表、工作树检查。
# mk/rootfs.mk         rootfs 配置 / guest 镜像布局 / rootfs、run-fs、test-fat。
# mk/build.mk          顶层目标、编译/链接、两趟 kallsyms、clean。
# mk/qemu.mk           run/run-net/test-* 等 QEMU 目标。
# mk/format.mk         format、format-check、apidoc、apidoc-check。
# mk/help.mk           make help 文本。
# tools/make_rootfs.sh rootfs 镜像的具体安装步骤（由 mk/rootfs.mk 调用）。
#
# 新增内核源文件：放进 kernel/ 下即可，无需改 Makefile（见 mk/sources.mk）。
