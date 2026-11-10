# RT 调度性能实测（x86_64 KVM，2026-11-10）

`RT_DEMO=1` 启动自测（`tests/unit-test/rt_demo.c`）的实测结果。
用于回答两个问题：

1. 同优先级体系内，RT 任务被唤醒后多快能真正运行？
2. 一个正在跑内核代码的 NORMAL 任务，被更高优先级 RT 任务抢占需要多久？

> 只测 x86_64 + KVM。aarch64/riscv64 是 TCG，绝对延迟没有参考意义。
> 所有任务钉 CPU0，只用内核线程 API，不经过 syscall。

## 复现

```sh
# 启动后自动跑 demo，结果在串口 "=== RT DEMO ..." 行
make PLATFORM=qemu-virt-x86_64 RT_DEMO=1 LOG=info SMP=1 run

# 带 rootfs 也可以（demo 在 fs 初始化之后、shell 之前运行）
make PLATFORM=qemu-virt-x86_64 RT_DEMO=1 LOG=info SMP=1 \
     ROOTFS_IMG=tests/.artifacts/build/normal/smp1/qemu-virt-x86_64/rootfs-x86_64.img run-fs
```

采集环境：QEMU `q35 -enable-kvm -cpu host`，100 Hz tick，`LOG=info`。

## 场景一：RT ping-pong（wake→run）

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

## 场景二：NORMAL → RT 抢占（tick 约束）

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

## 结论

1. **RT 任务之间的唤醒是微秒级**（~0.25–0.4 µs），R2 的 RT 队列 +
   立即 `need_resched` 路径工作正常。
2. **瓶颈是“从 NORMAL 上下文唤醒 RT”**：`sched_kick_after_enqueue()`
   只置 `need_resched`，但正在跑内核代码的 NORMAL 任务没有任何
   trap/返回边界去检查它，必须等 timer 中断 —— 于是最坏 10 ms、
   平均也接近 10 ms。这条延迟与被唤醒 RT 任务的优先级无关。
3. 这条延迟**不是 tick 频率问题**：把 tick 提到 1 kHz 只能把均值
   降到 ~0.5 ms，最坏值仍是一个周期。正确做法是在本核入队更高
   优先级 RT 任务时**主动制造一个调度点**。

## 后续 R3 建议（按收益排序）

1. **本核 RT 抢占用 self-IPI**：`sched_kick_after_enqueue()` 在
   “目标核就是本核、且新任务优先级高于当前任务”时，向本核发一个
   重调度 IPI（或等价的软件中断），让中断返回路径立即走
   `sched_check_and_yield()`。demo 场景二的延迟应从 ~10 ms 降到
   亚微秒~几微秒。
2. **长内核循环加显式抢占点**：不能只靠 IPI 返回；如果编译器把
   循环优化成一个没有函数调用的死循环，IPI 回来仍会在中断返回点
   调度，这一点没问题；但禁用中断的长临界区仍会阻塞。R3 的
   IRQ-off 收敛（`signal_check_uart`、`task_execve`）继续做。
3. **不要用更高 tick 频率掩盖问题**：功耗/开销大，且最坏值不变；
   可作为没有 self-IPI 时的临时手段。

## 已知边界

- 宿主 KVM 的调度抖动会污染 max（SMP=4 第一次跑出 18.7 µs 的
  ping-pong max）；比较时看 min/avg 和直方图，不只看单次 max。
- demo 自身 `KLOG_INFO` 是异步的，测量期间只记录时间戳、不写串口。
- 场景二的 hog 是“纯内核忙循环”，故意没有 `cond_resched`；
  用户态 syscall 返回路径本来就有调度检查，不在这个最坏场景里。
