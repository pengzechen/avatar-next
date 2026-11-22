# oslat：RT 忙循环的 OS 噪声

> 2026-11-23。属于 `RT_TEST=1`，由 `make -C tests rt-test` 执行。

## 1. 它测什么

rt-tests 的 `oslat` 测的是“一个已经在 CPU 上忙跑的 RT 线程，被
OS 噪声打断多久”。它和 cyclictest 不同：

- cyclictest 测 **deadline → 线程被唤醒**；
- oslat 测 **线程连续执行时，相邻两次推进之间的最大间隔**。

间隔的来源包括 timer tick、UART IRQ、关中断区间、宿主调度
（QEMU/KVM/TCG）。本实现是内核态等价物：

- FIFO **85** 的内核 RT 线程，钉 **CPU0**；
- 连续读 `timer_get_ns()`，记录相邻两次读取的间隔；
- 跑 **500ms**（控制 CI 时间），不睡眠、不分配、不碰 VFS；
- 输出 `samples/min/avg/max`，以及超过 **1µs / 10µs / 100µs**
  的次数（oslat 的 overrun 计数）。

x86_64 是主要目标；aarch64/riscv64 也执行同一测试，但它们的
`timer_get_ns()` 分辨率和 TCG 宿主调度不同，**绝对值只适合同架构
前后对比**，不要和用户态 `oslat` 直接比较。

## 2. 输出

```text
=== OSLAT: PASS duration=500000000 samples=39767928 min=6 avg=12 max=167392 \
    over_1us=653 over_10us=122 over_100us=8 ===
```

单位纳秒。`samples` 是忙循环实际迭代数，用来证明 RT 线程真的运行过；
`min/avg` 包含读时钟本身的开销，`max` 才是 OS 噪声的上界。

一次 x86_64 KVM（SMP=1）实测：

```text
duration=500000000ns samples=39767928 min/avg/max=6/12/167392ns
over_1us=653 over_10us=122 over_100us=8
```

aarch64/riscv64 TCG（同一次门禁）：

```text
aarch64: samples=5053712  min/avg/max=80/98/51920ns   over_1/10/100us=870/60/0
riscv64: samples=4860596  min/avg/max=0/102/54700ns   over_1/10/100us=738/66/0
```

## 3. 判据

`tests/suites/rt_test.py` 只做**结构性门禁**：

- `duration == 500ms`；
- `samples > 0`（RT 线程确实推进过）；
- `min <= avg <= max`；
- `over_1us >= over_10us >= over_100us`。

不在所有架构上卡绝对延迟：QEMU/TCG 的宿主噪声不可控。真实硬件的
门槛应来自目标控制环预算（见 `RT_TEST_ROADMAP.md` 的 T1/T3）。

## 4. 与其它指标的关系

| 指标 | 回答的问题 |
|---|---|
| cyclictest Thread | deadline 到了，RT 线程多久开始跑？ |
| cyclictest IRQ（timerlat） | deadline 到了，timer ISR 多久进来？ |
| oslat | 已经在跑的 RT 线程，被 OS 打断多久？ |
| `/proc/rtstat` wake | `sched_unblock` 到真正切入多久？ |
| `RT_IRQ_TRACE=1` irq_off | 最长关中断区间多久？ |

oslat 的 max 通常大于 cyclictest 的 IRQ 部分，因为它包含每次 tick/
UART 中断和宿主抢占；如果 oslat max 突然变大而 cyclictest 不变，
优先查 IRQ 负载、关中断和宿主环境，而不是 hrtimer 唤醒路径。

## 5. 代码位置

- `tests/unit-test/oslat.c` —— 测试本体；
- `tests/unit-test/rt_sched_test.c` —— 在 cyclictest 之后调用；
- `tests/suites/rt_test.py` —— 解析和门禁；
- `kernel/time/hrtimer.c` / `include/time/hrtimer.h` —— timerlat IRQ 拆分；
- `docs/realtime/CYCLICTEST.md` —— 周期唤醒延迟。
