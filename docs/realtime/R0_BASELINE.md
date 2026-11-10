# R0 延迟基线（2026-11-10）

用于 R3/后续实时化改造的前后对比。**比较时必须在同一架构、同一
QEMU 配置、同一 rootfs 和同一启动流程下重跑**，不要跨架构比较绝对值
（aarch64/riscv64 是 TCG 模拟，绝对延迟被显著放大）。

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

取**第二次** `cat`（系统已稳定运行约 10 秒；数值是启动以来的累计值）。

列含义：

```text
cpu wake_max wake_last wake_n syscall_max syscall_last syscall_n tick_gap_max tick_gap_last ticks
```

单位均为纳秒。`tick_gap` 是相邻 timer tick 间隔，作为“长关中断/长临界区”
的近似指标（正常约 10ms）；不是精确 irq-off trace。

## 基线原始结果

### x86_64（KVM，SMP=1）

```text
rtstat: per-CPU latency (ns)
cpu wake_max wake_last wake_n syscall_max syscall_last syscall_n tick_gap_max tick_gap_last ticks
cpu0 1008 788 2 33798673 285 312 35398811 10494224 1059
```

### aarch64（TCG，SMP=1）

```text
rtstat: per-CPU latency (ns)
cpu wake_max wake_last wake_n syscall_max syscall_last syscall_n tick_gap_max tick_gap_last ticks
cpu0 160208 160208 1 14113712 2512 218 10692944 10692944 561
```

### riscv64（TCG，SMP=1）

```text
rtstat: per-CPU latency (ns)
cpu wake_max wake_last wake_n syscall_max syscall_last syscall_n tick_gap_max tick_gap_last ticks
cpu0 193700 193700 1 126820700 2300 216 129849100 129849100 564
```

> riscv64/aarch64 的数百微秒级 wake 和百毫秒级 tick_gap 主要来自 QEMU
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

- 当前 x86_64 的 `tick_gap_max ≈ 35 ms` 说明启动期/长临界区把 tick
  拖长了 3 倍以上；R3 要把它压到接近 10ms tick 周期。
- `syscall_max ≈ 34 ms` 是当前最长的用户 syscall 内核执行时间；R3 的
  重点是把长 syscall/长锁拆开，让这个值降到毫秒级或更低。
- `wake_max ≈ 1 µs`（x86 KVM）已经有 RT 调度的效果；后续关注在负载下
  的 p99/最坏值，以及 vCPU kick→run 延迟。
- R0.5 将增加 arch_irq_save/restore 级真正 irq-off 采样；届时本文件
  会增加 `irq_off_max` 列。

## 复跑注意

- 先 `make PLATFORM=<p> clean` 再编，确保 R0 字段/宏全量生效。
- 不要并行跑 QEMU/构建：TCG 架构会被压慢，基线和回归都会失真。
- 同一份 rootfs、同一 `sleep` 流程；比较累计值时注意启动时间差异。
