# R0 延迟基线（2026-11-10）

用于 R3/后续实时化改造的前后对比。**比较时必须在同一架构、同一
QEMU 配置、同一 rootfs 和同一启动流程下重跑**，不要跨架构比较绝对值
（aarch64/riscv64 是 TCG 模拟，绝对延迟被显著放大）。

> 2026-11-10 R0.5 起，`task_init()` 后会 `rt_latency_reset()`：下表是
> **运行时基线**（不含 fs_init/ext4 装载等启动期长临界区）。最早一版
> 的 boot-inclusive 数值保存在 commit `258d67a` 及其 `R0_BASELINE.md`。

## 采集环境

| 架构 | QEMU | 加速 | SMP | rootfs |
|---|---|---|---|---|
| x86_64 | `qemu-system-x86_64 -machine q35` | KVM | 1 | `tests/.artifacts/build/normal/smp1/qemu-virt-x86_64/rootfs-x86_64.img` |
| aarch64 | `qemu-system-aarch64 -M virt` | TCG | 1 | `tests/.artifacts/build/normal/smp1/qemu-virt-aarch64/rootfs-aarch64.img` |
| riscv64 | `qemu-system-riscv64 -M virt` | TCG | 1 | `tests/.artifacts/build/normal/smp1/qemu-virt-riscv64/rootfs-riscv64.img` |

内核：`realtime` 分支，R0 延迟测量版本（`task->wake_ns`、
`kernel/debug/rt_latency.c`、`/proc/rtstat`）。
`LOG=info`。rootfs 里是正常的 busybox shell，没有额外负载。

## 采集步骤

```sh
R=tests/.artifacts/build/normal/smp1/qemu-virt-<arch>/rootfs-<arch>.img
{ sleep 8; echo "cat /proc/rtstat"; sleep 2; echo "cat /proc/rtstat"; sleep 2; } |
  timeout 50s make PLATFORM=qemu-virt-<arch> ROOTFS_IMG=$R LOG=info run-fs
```

取**第二次** `cat`（系统已稳定运行约 8~10 秒；数值从 `task_init()` 后的
reset 开始累计）。

列含义：

```text
cpu wake_max wake_last wake_n syscall_max syscall_max_id syscall_last syscall_n
tick_gap_max tick_gap_last ticks irq_off_max irq_off_last irq_off_n irq_off_pc
```

单位均为纳秒。`syscall_max_id` 是内部 syscall 号（221 = execve）。
`tick_gap` 是相邻 timer tick 间隔（正常约 10ms）。`irq_off_*` 需要
`RT_IRQ_TRACE=1` 诊断变体；`irq_off_pc` 是最长区间的调用点，可用
`addr2line -e build/<platform>/kernel_<arch>.elf <pc>` 定位。

```sh
# 真实 IRQ-off 跟踪（会多一次函数调用开销，默认不开启）
make PLATFORM=qemu-virt-x86_64 RT_IRQ_TRACE=1 kernel
```

## 基线原始结果（运行时）

### x86_64（KVM，SMP=1）

R3 前（syscall 全程 IRQ-off）：

```text
cpu wake_max wake_last wake_n syscall_max syscall_max_id syscall_last syscall_n tick_gap_max tick_gap_last ticks
cpu0 1028 1028 1 10879711 221 371 221 14618756 14618756 577
```

R3 后（x86 syscall 路径 `sti`/`cli`）：

```text
cpu0 2048 2048 1 15283923 221 315 274 13207498 10017080 746
```

R3 后（异步 klog + fs mutex）+ `RT_IRQ_TRACE=1`（**clean 全量重编**）：

```text
cpu0 11278548 419 20 10998971 221 543 127 10017963 10017377 363 361529 3341 2581598 0xffff80000022b0fc
```

- `irq_off_max ≈ 0.36 ms`；当前最长区间落在 `signal_check_uart()`。
- 异步 klog 之前是 2.5ms（最长在 `klog_write` 的 UART 输出），
  再之前“~6.3µs”的读数是新旧 `.o` 混编导致的失真；`RT_IRQ_TRACE`
  已加入 `_CFG_SIG`，切换开关会全量重编。
- `wake_max` 的十几毫秒包含启动期 klogd 第一次真正运行前的累计
  等待；稳定后 `wake_last` 是亚微秒级。
- R3 前 `syscall_max ≈ 10.9 ms` 就是整段 IRQ-off；现在 syscall 本身仍
  可到 11~15 ms（最长是 execve，221），但中断已打开，不再是 IRQ-off
  窗口。

### aarch64（TCG，SMP=1）

```text
cpu0 161680 161680 1 14341776 221 40160 218 11660720 10111152 621 0 0 0 0x0
```

### riscv64（TCG，SMP=1）

R3 后（syscall 路径已开 SIE + 异步 klog + fs mutex）+
`RT_IRQ_TRACE=1`（clean 全量重编）：

```text
cpu0 61041800 7700 19 51851900 221 67700 124 11150500 10041800 500 1742800 1700 729515 0xffffffc080229186
```

- `irq_off_max ≈ 1.74 ms`；当前最长区间落在 `task_execve`（TCG 放大）。
  异步 klog 前是 8.9ms（最长在 `klog_write` 的 UART 输出），fs mutex
  前是 3.8ms（最长在 ext4 wrap 锁）。
- `tick_gap_max` 已从 R3 前的 43ms 量级回到约 10~11ms。
- aarch64 本轮未启用 trace，`irq_off_*` 为 0。

> riscv64/aarch64 的数百微秒级 wake 和较大的 tick_gap 主要来自 QEMU
> TCG 调度；它们只用于同架构前后对比，不代表硬件性能。

## 同批次回归结果

- LTP 全量 103 例：
  - x86_64 SMP=1：103/103 PASS
  - aarch64 SMP=1：103/103 PASS
  - riscv64 SMP=1：103/103 PASS
  - x86_64 SMP=4：103/103 PASS
  - aarch64 SMP=2：103/103 PASS
- `RT_TEST=1`（RT 调度类自测）：x86_64 / aarch64 / riscv64 均
  `RT SCHED TEST: PASS`。
- 三架构 clean build 通过。

## 判读与目标

- x86_64/riscv64 syscall 内核侧已开中断后，`tick_gap_max` 回到 10ms
  量级；`syscall_max` 仍会显示 execve 的 11~58ms（TCG），但它已不是
  IRQ-off 窗口。
- 异步 klog（64 KiB ring + FIFO 1 klogd）把 UART 输出移出 IRQ-off；
  ext4/FatFs 在 task_init 后改用睡眠 mutex。IRQ-off 从 x86 2.5ms /
  riscv 8.9ms 降到 **0.36ms / 1.74ms**。
- 当前剩余最大 IRQ-off：x86 `signal_check_uart()` 抽 UART FIFO；
  riscv `task_execve` 内的短锁。后续继续收敛。
- `wake_last` x86 KVM 亚微秒级；`wake_max` 的十几毫秒包含启动期
  klogd 首次运行前的累计等待，后续关注负载下 p99/最坏值。
- R0.5 的真实 irq-off 采样由 `RT_IRQ_TRACE=1` 提供；该开关影响所有
  TU，已加入 `_CFG_SIG` 强制全量重编。默认关闭以避免热路径开销。

## 复跑注意

- 先 `make PLATFORM=<p> clean` 再编，确保 R0 字段/宏全量生效。
- 不要并行跑 QEMU/构建：TCG 架构会被压慢，基线和回归都会失真。
- 同一份 rootfs、同一 `sleep` 流程；比较累计值时注意启动时间差异。
