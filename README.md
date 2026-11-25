# Avatar OS

![OS](https://img.shields.io/badge/OS-Avatar-blue)
![Arch](https://img.shields.io/badge/arch-AArch64%20%7C%20RISC--V%2064%20%7C%20x86__64-green)
![Linux ABI](https://img.shields.io/badge/ABI-Linux%20syscalls-blueviolet)
![RT](https://img.shields.io/badge/RT-FIFO%2FRR%20%7C%20PI%20chain%20%7C%20throttling-red)
![VMM](https://img.shields.io/badge/VMM-multi--VM%20%7C%20multi--vCPU-purple)
![License](https://img.shields.io/badge/license-MIT-orange)

**Avatar OS 是一个 64 位、freestanding、跨三架构（AArch64 / RISC-V 64 / x86_64）的操作系统内核。**
它想做的不是“再写一个教学内核”，而是一个能同时承载 **Linux 用户态、硬实时控制、虚拟化和 AI 加速器** 的嵌入式内核底座。四条主线同时推进：

- **Linux 兼容**：真的跑 Linux 用户态 —— busybox shell、动态链接 musl、pthread/futex、ext4/FAT、lwIP、dropbear，以及 LTP 回归。不是“实现了几个 syscall 的 demo”。
- **实时（RT）**：自己的 RT 调度类（FIFO/RR 1..99）、每核 RT 队列、self-IPI 立即抢占、**嵌套 PI 链**、**RT throttling**、每任务抢占计数；x86_64 用 TSC-deadline 做 1ms hrtimer。cyclictest / oslat / rt_worker / rt_event / rt_mutex 全部在仓库里，跑一条命令就能看数。
- **虚拟化**：AArch64 EL2/Stage-2、RISC-V H 扩展、x86 VMX/EPT 三套硬件后端；同一个内核里跑**多台 guest Linux**（多 VM、多 vCPU），各自启动到交互式 shell。
- **AI 加速器一等公民（进行中）**：今天 RK3588 NPU 可以透传给 aarch64 guest Linux，dma-buf/dma-heap、rknpu/DRM 已接入；方向是**异构执行类 + TCM/DMA 一等内存 + AI/RT 混合调度**（K3 X100/A100 设计草案见 [hetero](docs/hetero/HETEROGENEOUS_AI_DESIGN.md)）。

> **状态**：研究 / 原型阶段，不是可直接量产的安全关键系统。x86_64 的 1ms RT 已可用；aarch64/riscv64 仍是 10ms tick 回退；用户态 RT、CPU 隔离、裸机长跑与功能安全认证还在路线图上。详见 [实时路线图](docs/realtime/RT_TEST_ROADMAP.md) 和下面的「状态与边界」。

---

## 四条主线，今天能做到什么

| 主线 | 今天能做什么 | 从这里进 |
|---|---|---|
| **Linux 兼容** | busybox shell、动态 musl 程序、pthread/futex、ext4/FAT、lwIP、dropbear（aarch64）；LTP 测试集 | `run-fs`、`make -C tests apps`、`make -C tests ltp-*`、[syscall 现状](docs/SYSCALL_IMPLEMENTATION.md) |
| **实时** | RT 优先级抢占、嵌套 PI、RT throttling、1ms cyclictest；x86 SMP=4 实测 p99 ≈ 6.9µs、max ≈ 15.9µs、0 overrun（QEMU/KVM） | `make -C tests rt-test`、`make -C tests check-rt`、[realtime](docs/realtime/ARCHITECTURE.md) |
| **虚拟化** | 三架构 guest Linux 到 shell；同内核多 VM、多 vCPU；`/dev/vmm` + `vmm-run` | `test-guest-linux`、`make -C tests vmm-*`、[VMM 控制台](docs/vmm/GUEST_CONSOLE.md) |
| **AI / 异构** | RK3588 NPU → aarch64 guest Linux；dma-buf/dma-heap；rknpu/DRM；AI 执行类设计草案 | [NPU 透传](docs/vmm/AARCH64_GUEST_NPU.md)、[异构 AI 设计](docs/hetero/HETEROGENEOUS_AI_DESIGN.md) |

---

## 60 秒上手

### 0. 依赖

```bash
# Ubuntu / Debian（QEMU + rootfs 工具 + 本地编译）
sudo apt install build-essential e2fsprogs \
  qemu-system-x86 qemu-system-arm qemu-system-misc
```

- **x86_64**：host `gcc` 即可；如果 PATH 里有 `x86_64-linux-musl-gcc` 会优先使用。
- **aarch64 / riscv64**：需要 `aarch64-linux-musl-gcc` / `riscv64-linux-musl-gcc`
  （[musl.cc](https://musl.cc/) 的 `aarch64-linux-musl-cross`、`riscv64-linux-musl-cross`，
  解压后把 `bin/` 加进 PATH）。
- **x86_64 QEMU 默认走 KVM**：确认 `/dev/kvm` 可读写（`sudo usermod -aG kvm $USER` 后重新登录）。
- rootfs 用 `mkfs.ext4 -d` 生成，**不需要 sudo，也不需要 loop mount**。

### 1. 克隆、构建、启动

```bash
git clone https://github.com/pengzechen/avatar-next.git
cd avatar-next

# x86_64：一条命令完成内核 + rootfs + 启动（默认 SMP=1）
make PLATFORM=qemu-virt-x86_64 run-fs
```

看到 `~ #` 就进 busybox shell 了。退出 QEMU 用 `Ctrl-A X`。

另外两个架构（需要对应交叉编译器）：

```bash
make PLATFORM=qemu-virt-aarch64 run-fs   # TCG
make PLATFORM=qemu-virt-riscv64 run-fs   # TCG
```

> 第一次构建需要几分钟；`run-fs` 会自动构建 `kernel` 和 `rootfs`。
> 只想生成镜像：`make PLATFORM=qemu-virt-x86_64 rootfs`。
> 所有命令：`make help`。

### 2. 在 shell 里玩

```bash
~ # uname -a
~ # cat /proc/rtstat          # 每 CPU 的唤醒/IRQ-off/preempt-off/切换计数
~ # /bin/rtps                 # 看 RT 任务的 Policy / RtPrio
~ # /bin/rt-demo              # event + 1ms 周期 + watchdog + /proc/rtworkers
~ # /bin/rt-event-test        # event 模板 + 共享事件环的端到端验收
~ # ls /bin                   # busybox + rt-demo + rt-event-test + vmm-run ...
```

### 3. 一条命令看实时数据

```bash
# 三架构 SMP=1（TCG 约 30s/架构）或指定 x86 SMP=4
make -C tests rt-test ARCHES=x86_64 SMP=4

# RT 完整门禁：rt-test + rt-trace + SMP=4
make -C tests check-rt

# 把最新 run 生成一份 HTML 全貌报告
python3 tools/gen_rt_report.py
```

典型输出（x86_64 SMP=4，1ms 周期）：

```text
x86_64: cyclictest latency p99/p999/max=6914/9245/15930ns
x86_64: cyclictest irq_n=2000 ... overruns=0
x86_64: pi-chain low/mid=80/80 high_lat=9943ns
x86_64: rt-throttle rt_iters=... normal_iters=... events=2 normal/rt=4.2%
```

---

## 再深入：四条主线怎么跑

### Linux 兼容

```bash
# busybox shell（默认根文件系统）
make PLATFORM=qemu-virt-x86_64 run-fs

# 动态链接的用户态测试（pthread/futex、mutex）
bash apps/c/build.sh                 # 生成 imgs/rootfs-<arch>.img
                                      # 默认从 $HOME/SoftWare/compiler 找交叉工具链，按需改路径
make -C tests apps                   # 跑 /bin/pthread_test、/bin/mutex_test

# LTP：先拉源码 + 交叉编译，再跑子集/全量
make -C tests ltp-build              # 首次会下载 LTP（约 434MB）
make -C tests ltp-smoke
make -C tests ltp-full
```

覆盖范围与已知桩：见 [docs/SYSCALL_IMPLEMENTATION.md](docs/SYSCALL_IMPLEMENTATION.md)（104 个具名入口，逐个标注完整/不完整/桩）。
LTP 用例清单与运行方式：见 [docs/tests/LTP_GUIDE.md](docs/tests/LTP_GUIDE.md)。

### 实时（RT）

- 调度：每核 `rt_queue[100] + rt_bitmap`，`pick_next()` 先 RT 后 NORMAL 再 idle；
- 抢占：timer/IPI 只置 `need_resched`，在 trap 返回边界切换；NORMAL→RT 用 self-IPI，不等 tick；
- 锁：`rt_mutex` 支持**嵌套 PI 链**（等待者沿 owner 链传递提升），普通 `mutex` 是带内部自旋锁的睡眠锁；
- 兜底：**RT throttling** 默认每核 1s/950ms，防止 RT 忙循环饿死 NORMAL；
- 定时：x86_64 用 LAPIC TSC-deadline oneshot（1ms）；aarch64/riscv64 暂回退 100Hz tick（10ms 整数倍）。

```bash
make -C tests rt-test ARCHES=x86_64 SMP=4   # 调度语义 + 唤醒/抢占 + PI 链 + throttling
make -C tests rt-trace ARCHES=x86_64 SMP=4  # RT_IRQ_TRACE：/proc/rtstat 诊断面
make -C tests rt-soak N=10                  # 重复 rt-test，抓间歇问题
```

在 guest 里用用户态工具：

```bash
~ # /bin/rt-demo                 # event / 1ms 周期 / watchdog / /proc
~ # /bin/rtctl                   # 创建/启动/停止模板 worker
~ # /bin/rtps                    # 列出 RT 任务
```

数字怎么读、基线在哪：见 [CYCLICTEST.md](docs/realtime/CYCLICTEST.md)、[R0_BASELINE.md](docs/realtime/R0_BASELINE.md)、[RT_DEMO_RESULTS.md](docs/realtime/RT_DEMO_RESULTS.md)。

### 虚拟化（VMM）

三个架构各有硬件后端，能在同一个内核里同时跑多台 guest Linux：

| 架构 | 后端 | guest |
|---|---|---|
| AArch64 | EL2 / Stage-2（GICv2 / GICv3） | Linux 6.2.15 |
| RISC-V 64 | H 扩展（G-stage、vPLIC、SBI、16550A） | Linux 6.2.15 |
| x86_64 | VMX / EPT（vLAPIC、IO-APIC/PIT 桩、PIO 串口） | Linux 6.2.15 |

```bash
# 直启一台 guest Linux（x86_64 需要 KVM；aarch64/riscv 用 TCG）
make PLATFORM=qemu-virt-x86_64  LOG=warn SMP=1 test-guest-linux
make PLATFORM=qemu-virt-aarch64 LOG=warn SMP=1 test-guest-linux
make PLATFORM=qemu-virt-riscv64 LOG=warn SMP=1 test-guest-linux

# 统一测试框架：直启 / helper + 多核 / 同内核多 VM
make -C tests vmm-direct
make -C tests vmm-helper
make -C tests vmm-multivm
```

在 **helper 模式**（宿主 shell + `/dev/vmm`，见 [Guest 控制台](docs/vmm/GUEST_CONSOLE.md)）里可以交互管理 VM：

```bash
/ # vmm-run          # 启动/接入一个 guest
/ # vmm-run -l       # 列出所有 VM
/ # vmm-run -n       # 再起一个
/ # vmm-run -k 1     # 停掉 vm1
```

<details>
<summary>控制台按键（Ctrl+T 前缀）</summary>

| 按键 | 作用 |
|---|---|
| `Ctrl+T ?` | 帮助 |
| `Ctrl+T d` / `k` | 挂起 / 停掉前台并退出 |
| `Ctrl+T l` / `n` | 列出 VM / 新建 VM |
| `Ctrl+T 1..8` | 切到第 n 个 VM（不用退出再进） |

完整协议、多 VM 并发规则见 [docs/vmm/GUEST_CONSOLE.md](docs/vmm/GUEST_CONSOLE.md)。

</details>

VMM 回归三条路各管一个失败面，改 VMM 后**三条都要跑**：

```bash
tools/boot_regress.sh [N]                         # x86 直启，N 次都要进 shell
tools/vmm_helper_regress.sh <arch> [N] [smp] --restart
tools/vmm_multivm_regress.sh <arch> [smp]
```

### AI / 异构加速器（进行中）

今天的落点：

- **RK3588 NPU 透传给 aarch64 guest Linux**：guest 内跑 YOLOv8/ONNX，寄存器 MMIO 转发、DMA 窗口 IPA==PA；见 [AARCH64_GUEST_NPU.md](docs/vmm/AARCH64_GUEST_NPU.md)。
- **设备内存与图形**：dma-buf/dma-heap、rknpu/DRM 已接入（`dmatest`、`drmtest` 目标）。

方向（设计草案，不是已交付 API）：

- **异构执行类**：CPU 能力、VLEN、H/IME/TCM、OPP 作为一等属性，而不是一个统一 `cpu_t`；
- **TCM/DMA 作为内存层**：`mmap`/分配器/零拷贝/配额，不再依赖全局 `/dev/tcm`；
- **AI + RT 混合调度**：控制跑 RT 核，推理跑 AI 核；class-aware runqueue、QoS、尾延迟优先；
- **用户态 AI runtime 合作**：内核只提供 class/TCM/DMA/QoS ABI，ONNX Runtime/llama.cpp 留在用户态。

见 [HETEROGENEOUS_AI_DESIGN.md](docs/hetero/HETEROGENEOUS_AI_DESIGN.md) 和 [IDEAS.md](docs/hetero/IDEAS.md)。

---

## 测试矩阵

测试统一走 `tests/run.py`（产物按变体 + SMP 隔离在 `tests/.artifacts/`）：

| 命令 | 覆盖 |
|---|---|
| `make -C tests build` | 三架构 × 变体构建矩阵 |
| `make -C tests boot` | 三架构启动冒烟 |
| `make -C tests kernel` | PMM / string / VMM_TEST 自测 |
| `make -C tests check` | build + boot + kernel + apps（SMP=1） |
| `make -C tests check-rt` | rt-test + rt-trace + SMP=4 rt-test |
| `make -C tests rt-test` / `rt-trace` | RT 语义 / `/proc/rtstat` 诊断面 |
| `make -C tests apps` | pthread / mutex（需先 `bash apps/c/build.sh`） |
| `make -C tests ltp-smoke` / `ltp-mm` / `ltp-full` | LTP（需先 `ltp-build`） |
| `make -C tests vmm-direct` / `vmm-helper` / `vmm-multivm` | guest 直启 / helper 多核 / 同内核多 VM |
| `make format-check` / `make apidoc-check` | 格式 / C API 文档门禁 |

```bash
make -C tests help        # 全部目标与变量
```

> 已知：`kernel/vmm` 自测在 SMP=1 下对时序敏感（偶发超时，不是 guest 路径本身的问题）；
> 验证真实 guest 用 `make -C tests vmm-direct`。
>
> TCG 慢机器可加 `TIMEOUT_SCALE=2~3`；强制不用 KVM 用 `AVATAR_TEST_NO_KVM=1`
> （x86 的 VMM/guest 需要 KVM，会自动 SKIP）。
> 构建不跟踪头文件依赖：改过 `.h`、或切换 `SMP=` / `LOG=` 后必须先
> `make PLATFORM=<p> clean`，否则会出现“看起来像内存踩踏”的幽灵 bug。

---

## 项目结构

```text
avatar-next/
├── apps/          # 用户程序：busybox / dropbear / rknn / vmm-run + C 测试程序
├── boot/          # 三架构启动、异常/中断入口、页表、链接脚本
├── driver/        # UART、定时器、LAPIC/GIC/PLIC、块设备、virtio、NPU...
├── fs/            # VFS + ext4（lwext4，first-party）+ FAT（FatFs，first-party）
├── include/       # 公共头；include/arch/<arch>/* 架构相关内联实现
├── kernel/        # main、mm、task、linux（ABI personality）、rt、vmm、net、debug、time
├── lib/           # klog、string、vsnprintf、bitmap
├── mk/            # Makefile 分片（平台/工具链/源文件/变体/QEMU/格式）
├── platforms/     # qemu-virt-{x86_64,aarch64,riscv64} + rk3588-aarch64 + sg2002-riscv64
├── tests/         # 统一测试框架（run.py + suites + unit-test + ltp）
├── tools/         # 镜像、回归、报告、kallsyms 等脚本
├── docs/          # 文档（basic / realtime / vmm / fs / hetero / bugfix ...）
└── CLAUDE.md      # AI/贡献者上下文（架构约束、踩坑记录）
```

---

## 构建选项速查

| 变量 / 目标 | 作用 |
|---|---|
| `PLATFORM=qemu-virt-x86_64` / `-aarch64` / `-riscv64` | QEMU 平台（默认 `qemu-virt-aarch64`） |
| `PLATFORM=rk3588-aarch64` / `sg2002-riscv64` | 真实板卡 |
| `LOG=none\|error\|warn\|info\|debug\|trace` | 日志级别（默认 `info`） |
| `SMP=N` | 编译期 SMP 核数（默认 1；改动后需 clean） |
| `FP=0` | 去掉帧指针（backtrace 退化为栈扫描） |
| `CC=<compiler>` | 覆盖交叉编译器 |
| `QEMU_NET_FLAGS=...` | `run-net` 的 QEMU 网络参数 |
| `TIMEOUT_SCALE=2` | 测试框架超时放大 |
| `AVATAR_TEST_NO_KVM=1` | 测试框架强制 TCG |
| `RT_TEST=1` / `RT_DEMO=1` / `RT_IRQ_TRACE=1` | RT 变体（建议走 `tests/`，避免脏对象） |
| `GUEST_LINUX=1` / `VMM_TEST=1` / `PANIC_TEST=1` | guest / VMM / backtrace 演示变体 |

> 变体标志和头文件都不参与依赖跟踪：切换后先 `make clean`。
> 更细的构建陷阱见 [CLAUDE.md](CLAUDE.md) 和 [docs/tests/TESTS.md](docs/tests/TESTS.md)。

---

## 文档地图

**先看**

- [docs/tests/TESTS.md](docs/tests/TESTS.md) — 测试体系、如何加测试
- [docs/PROJECT_REFERENCE.md](docs/PROJECT_REFERENCE.md) — 完整技术参考
- [CLAUDE.md](CLAUDE.md) — AI/贡献者上下文（踩坑与硬约束）

**Linux 兼容**

- [SYSCALL_IMPLEMENTATION.md](docs/SYSCALL_IMPLEMENTATION.md) — 104 个入口的完整/不完整/桩分类
- [LTP_GUIDE.md](docs/tests/LTP_GUIDE.md)、[USER_PROCESS_STATUS.md](docs/USER_PROCESS_STATUS.md)

**实时**

- [realtime/ARCHITECTURE.md](docs/realtime/ARCHITECTURE.md) — 任务/调度/R0–R6 路线
- [RT_LOCKS.md](docs/realtime/RT_LOCKS.md) — mutex / rt_mutex / PI 链
- [RT_THROTTLING.md](docs/realtime/RT_THROTTLING.md) — RT 带宽限制与代价
- [CYCLICTEST.md](docs/realtime/CYCLICTEST.md)、[OSLAT.md](docs/realtime/OSLAT.md)、[R0_BASELINE.md](docs/realtime/R0_BASELINE.md)
- [RT_TEMPLATES.md](docs/realtime/RT_TEMPLATES.md)、[RT_EVENTS.md](docs/realtime/RT_EVENTS.md)、[RT_USER_DEMO.md](docs/realtime/RT_USER_DEMO.md)
- [RT_TEST_ROADMAP.md](docs/realtime/RT_TEST_ROADMAP.md) — 到 72h soak / 安全认证的路线

**虚拟化 / AI**

- [GUEST_CONSOLE.md](docs/vmm/GUEST_CONSOLE.md) — `vmm-run`、Ctrl+T、多 VM
- [X86_GUEST_LINUX.md](docs/vmm/X86_GUEST_LINUX.md)、[RISCV64_GUEST_LINUX.md](docs/vmm/RISCV64_GUEST_LINUX.md)、[MULTI_VCPU.md](docs/vmm/MULTI_VCPU.md)
- [AARCH64_GUEST_NPU.md](docs/vmm/AARCH64_GUEST_NPU.md)、[RK3588_NPU_DRIVER.md](docs/npu/RK3588_NPU_DRIVER.md)
- [HETEROGENEOUS_AI_DESIGN.md](docs/hetero/HETEROGENEOUS_AI_DESIGN.md)

**文件系统 / 调试 / 底层**

- [LWEXT4.md](docs/LWEXT4.md)、[FAT_MOUNT.md](docs/fs/FAT_MOUNT.md)
- [KLOG.md](docs/basic/KLOG.md)、[ASSERT.md](docs/basic/ASSERT.md)、[BACKTRACE.md](docs/basic/BACKTRACE.md)
- [BARRIER.md](docs/basic/BARRIER.md)、[SPINLOCK.md](docs/basic/SPINLOCK.md)、[MMIO.md](docs/basic/MMIO.md)
- [INTERRUPT_CONTEXT_SWITCH.md](docs/INTERRUPT_CONTEXT_SWITCH.md)、[INTERRUPT_CONTROL_COMPARISON.md](docs/INTERRUPT_CONTROL_COMPARISON.md)
- [bugfix/](docs/bugfix/) — 真实踩坑记录（SMP、VMM、栈复用、时间基准……）

---

## 状态与边界

**已经能用的**

- 三架构 busybox shell、动态 musl 用户程序、ext4/FAT、lwIP；
- RT 优先级抢占、嵌套 PI 链、RT throttling、1ms cyclictest（x86_64）；
- 三架构 guest Linux 到交互式 shell；多 VM / 多 vCPU；`/dev/vmm` + `vmm-run`；
- 一套可复现的测试框架与 RT 报告生成器。

**还不够生产**

- **aarch64 / riscv64 没有 hrtimer oneshot**，周期唤醒只有 10ms tick —— 1kHz 控制环在 ARM 上还做不了；
- **没有用户态 RT**：`SCHED_FIFO/RR`、`mlockall`、PI futex、绝对 `clock_nanosleep` 还没对用户进程开放；RT 目前是内核线程 / `rt_worker`；
- **没有 CPU 隔离 / nohz / IRQ steering**，RT 核仍会被 NORMAL 和普通 IRQ 干扰；
- **没有裸机长跑**：现有数字全部来自 QEMU/KVM（2000 样本、尾部很薄），需要目标 SoC 上的 1h/24h/72h soak 与负载矩阵；
- **没有功能安全链路**：看门狗、lockup detector、RT miss 的 safe-state、e-stop/STO、IEC 61508 / ISO 13849 认证都还没有。

**对我们的定位来说**：调度器与 RT 机制已经能打；还差平台化、隔离、用户态 ABI、裸机验证和安全链路。路线见 [RT_TEST_ROADMAP.md](docs/realtime/RT_TEST_ROADMAP.md)。

---

## 贡献

- 提交 Issue / PR 前先跑：`make format-check`、`make apidoc-check`，以及受影响的 `make -C tests ...`。
- 改任务/中断/调度/文件系统/虚拟化之前，先看 [CLAUDE.md](CLAUDE.md) 的「详细文档索引」和对应 `docs/` 文档。
- 测试怎么加：见 [docs/tests/TESTS.md](docs/tests/TESTS.md)。

## 许可证

MIT License。第三方代码（lwext4、FatFs、lwIP、busybox、dropbear 等）保留各自许可证，见对应目录的 `README` / `COPYING`。
