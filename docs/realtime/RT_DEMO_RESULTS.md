# RT 调度性能实测（x86_64 KVM，2026-11-10）

`RT_DEMO=1` 启动自测（`tests/unit-test/rt_demo.c`）的实测结果。
用于回答两个问题：

1. RT 任务被唤醒后多快能真正运行？
2. 一个正在跑内核代码的 NORMAL 任务，被更高优先级 RT 任务抢占需要多久？

> 只测 x86_64 + KVM。aarch64/riscv64 是 TCG，绝对延迟没有参考意义，
> 仅用于确认 self-IPI 在三个架构上都能工作。
> 所有任务钉 CPU0，只用内核线程 API，不经过 syscall。

## 复现

```sh
# 启动后自动跑 demo，结果在串口 "=== RT DEMO ..." 行
make PLATFORM=qemu-virt-x86_64 RT_DEMO=1 LOG=info SMP=1 run

# 带 rootfs 也可以（demo 在 fs 初始化之后、shell 之前运行）
make PLATFORM=qemu-virt-x86_64 RT_DEMO=1 LOG=info SMP=1 \
     ROOTFS_IMG=tests/.artifacts/build/normal/smp1/qemu-virt-x86_64/rootfs-x86_64.img run-fs

# 统一测试框架：三架构同时跑 RT_TEST + RT_DEMO，并检查延迟上限
make -C tests rt-test
```

`rt-test` 是回归门禁：`RT_TEST` 的场景 4 断言 NORMAL→RT 立即抢占
平均 < 2ms（没有 self-IPI 时≈5ms），框架再对 demo 的 ping-pong 平均值
设 1ms 上限、抢占平均值设 2ms 上限。具体数据仍看串口
`=== RT DEMO ...` 行或 `tests/.artifacts/runs/<id>/logs/rt-test/`。

场景 5 是模板 worker + 共享页门禁：create → 共享页参数/输入 →
start → 运行期参数 → stop → destroy。有 oneshot 的架构（x86 KVM）
同时断言周期为 1ms、200ms 窗口内至少完成 3/4 理论周期；aarch64/riscv
的早期自测窗口里 tick 回调尚未启用，只验生命周期与共享页（标记
`periodic=0`）。

采集环境：QEMU `q35 -enable-kvm -cpu host`，100 Hz tick，`LOG=info`。

## 观察 RT 任务

`/proc/<pid>/status` 增加两个字段（数值与 Linux 一致：0=SCHED_NORMAL,
1=SCHED_FIFO, 2=SCHED_RR）：

```text
Policy:	1 (SCHED_FIFO)
RtPrio:	1
```

用户态工具 `/bin/rtps`（`apps/c/rtps.c`）按 PID 列出所有任务，RT 行尾
标 `[RT]`；`rtps -r` 只看 RT：

```text
/ # rtps
  PID  PPID POLICY        PRIO S NAME
    1     0 SCHED_FIFO       1 S klogd  [RT]
    2     0 SCHED_NORMAL     0 R net-poll
    3     0 SCHED_NORMAL     0 S busybox

/ # rtps -r
  PID  PPID POLICY        PRIO S NAME
    1     0 SCHED_FIFO       1 S klogd  [RT]
```

`rtps` 随 rootfs 自动安装（`make ... rootfs`）；只编它用
`make PLATFORM=<p> rtps`。busybox `ps` 仍然只显示 PID/STATE/COMMAND，
不显示调度策略。

## 改造前基线

### 场景一：RT ping-pong（wake→run）

RT FIFO 50 worker 与 FIFO 10 waker 互相 `task_block`/`task_unblock`，
5000 轮。waker 在 `task_unblock(worker)` 之前记时间戳，worker 被调度到
之后立即结算 —— 包含 waker 从 unblock 到 block 的路径。

| 配置 | 轮次 | min | avg | max | <1 µs 占比 |
|---|---:|---:|---:|---:|---:|
| SMP=1 第 1 次 | 5000 | 251 ns | 257 ns | 9.65 µs | 4997/5000 |
| SMP=1 第 2 次 | 5000 | 250 ns | 297 ns | 5.07 µs | 4938/5000 |
| SMP=1 第 3 次 | 5000 | 250 ns | 254 ns | 2.15 µs | 4999/5000 |
| SMP=4 | 5000 | 402 ns | 406 ns | 2.40 µs | 4998/5000 |

- SMP=1 稳定在 **~250 ns**；第 2 次的 avg 偏高，是宿主 KVM 抖动让
  61 个样本落到 1–5 µs（其余仍在 1 µs 内）。
- SMP=4 升到 **~400 ns**：demo 任务都钉在 CPU0，差别主要来自
  `sched_unblock/sched_schedule` 在 SMP 下走 `rq_lock` 原子路径，
  以及其它核 timer/日志的 cache 干扰。
- 个别 µs 级 max 是宿主 KVM 调度抖动，不是调度器路径本身。

### 场景二：NORMAL → RT 抢占（tick 约束）

NORMAL hog 唤醒 RT FIFO 60 probe 后继续忙循环，probe 只能等下一次
timer 中断的抢占点才运行。共 50 轮，每轮重新记录时间戳。

| 配置 | 轮次 | min | avg | max | <1 ms | 1–9 ms | ≥9 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| SMP=1 第 1 次 | 50 | 5.57 ms | 9.92 ms | 10.03 ms | 0 | 1 | 49 |
| SMP=1 第 2 次 | 50 | 4.93 ms | 9.90 ms | 10.03 ms | 0 | 1 | 49 |
| SMP=1 第 3 次 | 50 | 4.33 ms | 9.89 ms | 10.03 ms | 0 | 1 | 49 |
| SMP=4 | 50 | 0.71 ms | 9.83 ms | 10.04 ms | 1 | 0 | 49 |

50 个样本里 49 个落在 ≥9 ms：**被唤醒的 RT 任务要等下一次 tick 才运行**，
平均延迟 ≈ tick 周期（10 ms）减去唤醒点距离下一个 tick 的随机相位。
min 4.33 ms / 0.71 ms 只是恰好唤醒了相位靠后的样本。

## self-IPI 立即抢占（已实现）

`kernel/task/sched.c: sched_kick_after_enqueue()` 在本核入队更高优先级的
RT 任务时，除了置 `need_resched`，再向本核发一个重调度 IPI。中断入口
本身就是一个 trap，它的返回路径会立刻消费 `need_resched`，把
“等下一次 timer tick”变成“等一次 IPI 往返”。

实现上的三个取舍：

- **idle/boot 上下文不发**：idle 不在就绪队列里，它自己的 trap/yield
  路径会消费标志；boot 阶段发 IPI 还会提前打断内核初始化。
- **中断上下文不发**：本轮 ISR 的返回路径一定会消费 `need_resched`，
  再发一个只是多一次嵌套中断。
- **用户进程仍然发**：用户态需要立即被打断；如果它正在 syscall 内核侧，
  `trap_preempt_kernel_side()` 会拒绝在任意 syscall 代码点切换，延迟到
  syscall 返回边界。

### 改造后：SMP=1（三次）与 SMP=4

| 场景 | 配置 | 轮次 | min | avg | max |
|---|---|---:|---:|---:|---:|
| ping-pong | SMP=1 第 1 次 | 5000 | 2.08 µs | 2.15 µs | 11.7 µs |
| ping-pong | SMP=1 第 2 次 | 5000 | 2.09 µs | 2.17 µs | 17.2 µs |
| ping-pong | SMP=1 第 3 次 | 5000 | 2.10 µs | 2.14 µs | 12.0 µs |
| ping-pong | SMP=4 | 5000 | 2.24 µs | 2.34 µs | 21.1 µs |
| preempt | SMP=1 第 1 次 | 50 | 2.11 µs | 2.17 µs | 4.15 µs |
| preempt | SMP=1 第 2 次 | 50 | 2.10 µs | 2.18 µs | 5.08 µs |
| preempt | SMP=1 第 3 次 | 50 | 2.11 µs | 2.13 µs | 2.82 µs |
| preempt | SMP=4 | 50 | 2.27 µs | 2.29 µs | 2.81 µs |

- **NORMAL→RT 从 ~9.9 ms 降到 ~2.1 µs**（约 4600 倍）。
- **RT→RT 从 ~250 ns 变成 ~2.1 µs**：每次 RT 唤醒现在都要走一次
  self-IPI 往返。这是换取“任意上下文唤醒 RT 都有界”的代价。
- 150 个 preempt 样本里 149 个落在 2–5 µs；其余为宿主 KVM 抖动。
- 改造后三条路径（RT→RT、NORMAL→RT、用户态→RT）的唤醒延迟都收敛到
  **同一个 ~2 µs 量级**，不再依赖 tick 相位。

### 三架构确认（TCG，仅验证正确性）

| 架构 | ping-pong min/avg | preempt min/avg |
|---|---|---|
| aarch64 GICv3 | 9.3 µs / 10.1 µs | 9.4 µs / 10.0 µs |
| riscv64 PLIC | 16.5 µs / 21.3 µs | 17.1 µs / 20.3 µs |

riscv 的 self-IPI 走 SBI ecall，开销最大；但功能正常，且都比 tick 周期
低两个数量级。

## 结论

1. **RT 唤醒延迟现在由 self-IPI 往返决定（x86 KVM ~2 µs）**，与当前
   运行的是 RT / NORMAL / 用户任务无关，也不再受 100 Hz tick 影响。
2. 改造前 RT→RT 的 ~250 ns 是“直接调度点”的极限值；改造后多花的
   ~1.8 µs 是立即抢占的固定成本。对软实时目标来说，用 2 µs 换掉
   10 ms 的最坏情况是明显划算的。
3. 剩余的长延迟来源不再是调度器唤醒路径，而是 IRQ-off 临界区
   （`signal_check_uart`、`task_execve` 内的短锁）—— 见
   `R0_BASELINE.md`。

## 后续 R3

1. **IRQ-off 收敛**：`signal_check_uart` 的 UART FIFO 临界区、
   execve 内的短锁。它们决定 RT 任务真正拿到 CPU 前的最坏阻塞。
2. **vCPU kick→run**：vCPU 任务被 `task_kick()` 唤醒的延迟还没有
   专门测量；self-IPI 只在“入队更高优先级 RT”时触发，`task_kick`
   走的是 `sched_resched_cpu()` 的“目标核 == 本核则只置标志”分支。
3. **长内核循环的显式抢占点**：self-IPI 解决的是“中断返回点能不能
   到”的问题；关中断的长临界区仍然只能靠拆锁/缩短临界区。

## 已知边界

- 宿主 KVM 的调度抖动会污染 max（SMP=4 跑出过 21 µs 的 ping-pong
  max）；比较时看 min/avg 和直方图，不只看单次 max。
- demo 自身 `KLOG_INFO` 是异步的，测量期间只记录时间戳、不写串口。
- self-IPI 让内核线程在 `task_unblock()` 返回前就可能被抢占。demo 的
  ping-pong 原来用 “unblock 后马上 block” 的写法，立即抢占下会丢一次
  唤醒；已改成“计数 + task_yield”等待，对任意抢占时机都成立。
- 场景二的 hog 是“纯内核忙循环”，故意没有 `cond_resched`；
  用户态 syscall 返回路径本来就有调度检查，不在这个最坏场景里。
