# tests/Makefile — Avatar OS 测试框架入口
#
# 根 Makefile 已经很长了，所以测试编排全部收在这里：
#
#   make -C tests check          # 快速门禁：build + boot + kernel 自测
#   make -C tests check-full     # 再加 LTP smoke
#   make -C tests check-mm       # 内存重构专用：+ LTP mm + VMM guest
#   make -C tests build          # 三架构构建矩阵（normal/guest/vmm/string/pmm）
#   make -C tests boot           # 三架构启动冒烟
#   make -C tests kernel         # PMM / string / VMM_TEST 自测
#   make -C tests ltp-build      # 交叉编译 LTP（三架构）
#   make -C tests ltp-smoke      # LTP 快子集
#   make -C tests ltp-mm         # LTP 内存子集
#   make -C tests ltp-full       # LTP 全量
#   make -C tests vmm-direct     # guest Linux 直启 + 交互 shell
#   make -C tests vmm-helper     # helper 模式 + 交互 + 停止/重启
#   make -C tests vmm-multivm    # 多 VM detach/attach
#   make -C tests soak           # 重复跑 check（N=次数）
#
# 产物：tests/.artifacts/build/<variant>/smp<N>/<platform>/
# 报告：tests/.artifacts/runs/<run-id>/report.{json,md}
#
# 环境变量：
#   ARCHES=aarch64 riscv64 x86_64   SMP=1   LOG=warn   JOBS=$(nproc)
#   TIMEOUT_SCALE=1.0（TCG 慢机器可以调到 2~3）
#   AVATAR_TEST_NO_KVM=1（强制不用 KVM）

PYTHON  ?= python3
ARCHES  ?= aarch64 riscv64 x86_64
SMP     ?= 1
SMP4    ?= 4
# 测试框架依赖 INFO 级标记（PMM 账目、Launching busybox shell、自测 PASS 行），
# 所以默认 info；用 LOG=warn 跑 check 会让 boot/helper 找不到判据。
LOG     ?= info
JOBS    ?= $(shell nproc)
TIMEOUT_SCALE ?= 1.0
ARTIFACTS ?= $(CURDIR)/.artifacts
N       ?= 10

# LTP 源码 / 选定测例。源码不入库：`make env` 拉取到 tests/ltp/src。
LTP_VERSION ?= 20260529
LTP_SRC     ?= $(CURDIR)/ltp/src

RUN = $(PYTHON) $(CURDIR)/run.py \
      --artifacts $(ARTIFACTS) \
      --log $(LOG) --smp $(SMP) --jobs $(JOBS) \
      --timeout-scale $(TIMEOUT_SCALE) \
      --arch $(ARCHES)

.PHONY: help list env check check-fast check-full check-mm check-smp check-mm-smp \
        build boot kernel apps racetest \
        ltp-build ltp-smoke ltp-mm ltp-full \
        vmm-direct vmm-helper vmm-guest vmm-multivm soak \
        clean clean-artifacts

help:
	@echo "Avatar OS test framework"
	@echo ""
	@echo "  make -C tests check         # build + boot + kernel + apps（SMP=1）"
	@echo "  make -C tests check-full    # check + LTP smoke"
	@echo "  make -C tests check-mm      # 内存重构门禁（含 LTP mm + VMM guest）"
	@echo "  make -C tests check-smp     # check + racetest（SMP=$(SMP4)）"
	@echo "  make -C tests check-mm-smp  # 内存门禁 SMP=$(SMP4)"
	@echo "  make -C tests build         # 三架构构建矩阵"
	@echo "  make -C tests boot          # 三架构启动冒烟"
	@echo "  make -C tests kernel        # PMM / string / VMM_TEST"
	@echo "  make -C tests apps          # apps/c：pthread / mutex"
	@echo "  make -C tests racetest      # apps/c：SMP 竞争压力（需 SMP>1）"
	@echo "  make -C tests env           # 拉取 LTP 源码 → tests/ltp/src"
	@echo "  make -C tests ltp-build     # 交叉编译 LTP（自动先跑 env）"
	@echo "  make -C tests ltp-smoke     # LTP smoke"
	@echo "  make -C tests ltp-mm        # LTP 内存子集"
	@echo "  make -C tests ltp-full      # LTP 全量"
	@echo "  make -C tests vmm-direct    # guest 直启 + 交互 shell"
	@echo "  make -C tests vmm-helper    # helper + 交互 + 停止/重启"
	@echo "  make -C tests vmm-multivm   # 多 VM 回归"
	@echo "  make -C tests soak          # 重复 check（N=$(N)）"
	@echo "  make -C tests clean         # 清掉 tests/.artifacts"
	@echo ""
	@echo "变量：ARCHES='$(ARCHES)' SMP=$(SMP) SMP4=$(SMP4) LOG=$(LOG) JOBS=$(JOBS)"
	@echo "      TIMEOUT_SCALE=$(TIMEOUT_SCALE)（TCG 慢机器调到 2~3）"

list:
	@$(PYTHON) $(CURDIR)/run.py --list

check check-fast:
	$(RUN) --suite check

check-full:
	$(RUN) --suite check,ltp-smoke

check-mm:
	$(RUN) --suite check-mm

# SMP 矩阵：SMP 是编译期标志，必须走独立的 BUILD_ROOT（框架已按 smp 分目录）。
# racetest 是诊断 suite，单独跑；已知 riscv64 SMP=4 的 wake 会偶发变红。
check-smp:
	$(MAKE) --no-print-directory check SMP=$(SMP4)
	$(MAKE) --no-print-directory racetest SMP=$(SMP4)

check-mm-smp:
	$(MAKE) --no-print-directory check-mm SMP=$(SMP4)

build:
	$(RUN) --suite build

boot:
	$(RUN) --suite boot

kernel:
	$(RUN) --suite kernel

apps:
	$(RUN) --suite apps

racetest:
	$(RUN) --suite racetest

env:
	@bash $(CURDIR)/ltp/fetch.sh "$(LTP_SRC)" "$(LTP_VERSION)"

ltp-build: env
	@for a in $(ARCHES); do \
		echo "== LTP build $$a =="; \
		bash $(CURDIR)/ltp/build.sh $$a || exit 1; \
	done

ltp-smoke:
	$(RUN) --suite ltp-smoke

ltp-mm:
	$(RUN) --suite ltp-mm

ltp-full:
	$(RUN) --suite ltp-full

vmm-direct:
	$(RUN) --suite vmm-direct

vmm-helper:
	$(RUN) --suite vmm-helper

vmm-guest:
	$(RUN) --suite vmm-guest

vmm-multivm:
	$(RUN) --suite vmm-multivm

soak:
	@i=0; while [ $$i -lt $(N) ]; do \
		i=$$((i+1)); \
		echo "===== soak $$i/$(N) ====="; \
		$(MAKE) --no-print-directory check || exit 1; \
	done

clean: clean-artifacts

clean-artifacts:
	rm -rf $(ARTIFACTS)
	find $(CURDIR) -name __pycache__ -type d -prune -exec rm -rf {} +
