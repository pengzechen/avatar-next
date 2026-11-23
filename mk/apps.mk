# mk/apps.mk — 用户态程序（apps/）的变量与编译规则
# 由顶层 Makefile 在 driver/drivers.mk 之后 include：DMA/NPU 等选择已确定。

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

ifneq ($(filter $(ARCH),aarch64 riscv64),)
EPOLL_PERF_CC    := $(ARCH)-linux-musl-gcc
EPOLL_PERF_BIN   := apps/epoll_perf-$(ARCH)
else
EPOLL_PERF_CC    :=
EPOLL_PERF_BIN   :=
endif

# racetest：并发竞争态复现工具（apps/c/racetest.c）。
# 三架构都编 —— 它只依赖 fork/pipe/waitpid，没有架构特定代码。
# 跑法见 apps/c/racetest.c 顶部；**部分子命令要 SMP>1** 才可能复现。
#
# 产物落在 $(BUILD_DIR)/ 下，**不进 git**（build/ 已被 .gitignore 覆盖）。
# 注意别放到 apps/ —— 那里已经有一批提交进仓库的二进制（busybox、
# mutex_test 等），是历史遗留，新东西不要再往里加。
RACETEST_CC      := $(ARCH)-linux-musl-gcc
RACETEST_BIN     := $(BUILD_DIR)/racetest

# task-ipc：task 基础设施的用户态验收（apps/c/task_ipc_test.c）。
# 三架构都编；用 getcpu/clock_gettime 量化「跨核唤醒有没有立即生效」。
# 跑法见文件头。产物同样只落在 $(BUILD_DIR)/。
TASKIPC_CC       := $(ARCH)-linux-musl-gcc
TASKIPC_BIN      := $(BUILD_DIR)/task-ipc

# task-completion：completion 模块的用户态 A/B（需要 TASK_TEST=1 内核）。
TASKCOMP_CC      := $(ARCH)-linux-musl-gcc
TASKCOMP_BIN     := $(BUILD_DIR)/task-completion

# rtps：/proc/<pid>/status 的 RT 任务观察器（apps/c/rtps.c）。
# 静态链接，装进 /bin/rtps；busybox ps 看不到 Policy/RtPrio，用这个看。
RTPS_CC          := $(ARCH)-linux-musl-gcc
RTPS_BIN         := $(BUILD_DIR)/rtps

# rtctl：/dev/rtctl 控制面（apps/c/rtctl.c）。创建/启动/停止 RT 模板 worker。
RTCTL_CC         := $(ARCH)-linux-musl-gcc
RTCTL_BIN        := $(BUILD_DIR)/rtctl

# rt-event-test：event 模板 + 共享事件环的用户态验收。
RTEVENT_CC       := $(ARCH)-linux-musl-gcc
RTEVENT_BIN      := $(BUILD_DIR)/rt-event-test

# rt-demo：event / 1ms 周期 / watchdog / /proc/rtworkers 的用户态演示。
RTDEMO_USER_CC   := $(ARCH)-linux-musl-gcc
RTDEMO_USER_BIN  := $(BUILD_DIR)/rt-demo

# dmatest：dma-buf / dma-heap（P0）的验收程序。纯 ioctl + mmap，与架构无关。
# 只在 DMA=1 时构建/安装 —— 否则节点根本不存在，装了也是白装。
DMATEST_CC       := $(ARCH)-linux-musl-gcc
ifeq ($(DMA),1)
DMATEST_BIN      := $(BUILD_DIR)/dmatest
else
DMATEST_BIN      :=
endif

# drmtest：DRM 渲染节点 + GEM 层（P2+P4）的验收。需要渲染节点（NPU=rknpu）
# 和 dma-heap（DMA=1，由 NPU=rknpu 自动带上）。
DRMTEST_CC       := $(ARCH)-linux-musl-gcc
ifeq ($(NPU),rknpu)
DRMTEST_BIN      := $(BUILD_DIR)/drmtest
else
DRMTEST_BIN      :=
endif

# vmm-run：宿主 shell 里启动/驱动 guest 的用户态 helper（对标 kvmm-run）。
# 只给「已实现 Linux guest 启动」的架构构建与安装（与 include/vmm/vmm.h 的
# VMM_GUEST_LINUX_SUPPORTED 保持一致）。helper 本身是纯 C + ioctl，
# 与架构无关，三个架构共用同一份 apps/c/vmm_run.c。
ifneq ($(filter $(ARCH),aarch64 riscv64 x86_64),)
VMM_RUN_BIN      := apps/vmm-run-$(ARCH)
else
VMM_RUN_BIN      :=
endif
NGINX_BIN        := $(wildcard apps/nginx-$(ARCH))

# 用户应用程序：汇编为 .bin / .bin.elf，由 rootfs 装载（非内嵌）
$(BUILD_DIR)/apps_%.o: $(APPS_DIR)/%.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DAPP_ELF=1 -c $< -o $@

# 这些 .o 只经由模式规则（上面的 apps_%.o 与下面的 %.bin）产生，从未在
# Makefile 里被显式指名，因此 make 会把它们判为 intermediate 并在构建结束时
# 自动删除（终端上会看到一行 "rm build/.../apps_hello.o ..."）。
# 标记为 secondary 即可保留它们，避免每次重建 rootfs 都重新编译。
# 注意：apps_guest_test.o / apps_el0_loop.o 有显式规则，本来就不会被删。
# 加 ifneq 是因为裸 ".SECONDARY:"（无前置条件）含义是"所有目标都不删"，
# 若某架构 apps/ 下没有 .S，会意外变成全局语义。
ifneq ($(APPS_OBJECTS),)
.SECONDARY: $(APPS_OBJECTS)
endif

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

ifneq ($(EPOLL_PERF_BIN),)
epoll-perf: $(EPOLL_PERF_BIN)

$(EPOLL_PERF_BIN): apps/c/epoll_perf.c
	$(EPOLL_PERF_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "epoll perf app created: $@"
else
epoll-perf:
	@echo "epoll_perf is only wired for ARCH=aarch64 or ARCH=riscv64"
	@exit 1
endif

# §11c-2  racetest：并发竞争态复现工具，静态链接
racetest: $(RACETEST_BIN)

$(RACETEST_BIN): apps/c/racetest.c
	$(RACETEST_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "racetest app created: $@"

# §11c-2  task-ipc：跨核唤醒/分发的用户态验收，静态链接
task-ipc: $(TASKIPC_BIN)

$(TASKIPC_BIN): apps/c/task_ipc_test.c
	$(TASKIPC_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "task-ipc app created: $@"

# §11c-2  task-completion：completion 用户态测试，需要 TASK_TEST=1 内核
task-completion: $(TASKCOMP_BIN)

$(TASKCOMP_BIN): apps/c/task_completion_test.c
	$(TASKCOMP_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "task-completion app created: $@"

# §11c-2  rtps：/proc/<pid>/status 的 RT 任务观察器，静态链接
rtps: $(RTPS_BIN)

$(RTPS_BIN): apps/c/rtps.c
	$(RTPS_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "rtps app created: $@"

# §11c-2  rtctl：/dev/rtctl 控制面，静态链接
rtctl: $(RTCTL_BIN)

$(RTCTL_BIN): apps/c/rtctl.c apps/c/rtctl_abi.h
	$(RTCTL_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "rtctl app created: $@"

# rt-event-test：静态链接，事件模板端到端验收
$(RTEVENT_BIN): apps/c/rt_event_test.c apps/c/rtctl_abi.h
	$(RTEVENT_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "rt-event-test app created: $@"

$(RTDEMO_USER_BIN): apps/c/rt_demo_user.c apps/c/rtctl_abi.h
	$(RTDEMO_USER_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "rt-demo app created: $@"

# §11c-2  vmm-run：静态链接，免得还依赖 rootfs 里的动态 loader
$(VMM_RUN_BIN): apps/c/vmm_run.c
	$(ARCH)-linux-musl-gcc -O2 -Wall -Wextra -static $< -o $@
	@echo "vmm-run helper created: $@"

# dmatest（仅 DMA=1 时有用例）
ifneq ($(DMATEST_BIN),)
$(DMATEST_BIN): apps/c/dmatest.c
	$(DMATEST_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "dmatest created: $@"
dmatest: $(DMATEST_BIN)
else
dmatest:
	@echo "dmatest requires DMA=1 (see Makefile section 6e-1b)"
	@exit 1
endif

ifneq ($(DRMTEST_BIN),)
$(DRMTEST_BIN): apps/c/drmtest.c
	$(DRMTEST_CC) -O2 -Wall -Wextra -static $< -o $@
	@echo "drmtest created: $@"
drmtest: $(DRMTEST_BIN)
else
drmtest:
	@echo "drmtest requires NPU=rknpu (see Makefile section 6b)"
	@exit 1
endif
