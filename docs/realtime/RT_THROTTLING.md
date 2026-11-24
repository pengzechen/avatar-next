# RT throttling（RT 带宽限制）

> 一句话：**每个 CPU 每个 1s 周期内，RT 类（含被 rt_mutex PI 提升的任务）
> 最多跑 950ms；用完就暂时跳过 RT 队列，让 NORMAL / idle 拿到 5% 的 CPU，
> 下个周期再放行。** 防止一个 RT 忙循环把整颗核饿死。

## 一、为什么需要

`pick_next()` 的默认顺序是「最高 RT 队列 → NORMAL → idle」。只要有一个
RT 任务一直 ready，NORMAL 就永远拿不到 CPU。`rt-test` 里的
`rt-throttle` 场景在未实现 throttling 时：

```
=== RT THROTTLE TEST: FAIL rt_iters=146861037 normal_iters=0 ===
```

FIFO 80 的纯忙循环把 NORMAL(10) 完全饿死。Linux 用
`sched_rt_runtime_us=950000` / `sched_rt_period_us=1000000` 做同样的事，
这里按每 CPU 一份预算实现。

## 二、语义

- **范围**：每 CPU 独立预算。RT 任务和「被 PI 提升的 NORMAL 任务」都算
  RT（`task_sched_is_rt()`），一起消耗预算。
- **记账**：只统计当前任务是 RT 的时间。任务切换 / tick 时结账，所以
  NORMAL 运行的时间不会被算进 RT 预算。
- **限流**：本周期 `rt_time >= rt_runtime` 后置 `rt_throttled=true`；
  `pick_next()` 在这段时间**整类跳过** RT 队列，改选 NORMAL / idle。
  RT 任务仍留在 `rt_queue` 里，状态不变，下个周期继续跑。
- **周期滚动**：`sched_tick` / `sched_schedule` 里发现
  `now - period_start >= period` 时清零 `rt_time`、取消限流，并置
  `need_resched`，让被压住的 RT 任务重新参与调度。
- **默认值**：`period=1s`、`runtime=950ms`（95%），在
  `sched_rq_init()` 里为每个 CPU 初始化。
- **关闭**：`period == 0`、`runtime == 0`、或 `runtime >= period`。

## 三、实现位置

| 位置 | 作用 |
|---|---|
| `cpu_t` | `rt_period_ns` / `rt_runtime_ns` / `rt_period_start_ns` / `rt_time_ns` / `rt_account_ns` / `rt_throttle_events` / `rt_throttled` |
| `rt_throttle_account()`（`kernel/task/sched.c`） | 结账 + 周期滚动 + 超预算置 `rt_throttled`/`need_resched` |
| `sched_schedule()` 入口 | 切换前结账；`pick_next()` 依据 `rt_throttled` 决定是否选 RT |
| `sched_tick()` | 每 tick 结账；RT 忙循环不会主动让出，靠这里置 `need_resched` |
| `sched_rt_throttle_set()` | 测试/调试入口，重置所有 CPU 的配置与记账 |

`rt_throttle_account()` 必须同时挂在 `sched_schedule()` 和
`sched_tick()` 上：前者保证主动让出/阻塞前结清，后者保证**不切换的 RT
忙循环**也能在预算耗尽时被 tick 抢占下去。

> ⚠️ 因为记账在 `sched_tick` 上，**scheduler tick 必须安装**
> （正常启动由 `main.c` 调 `timer_set_tick_cb(sched_tick)`）。RT 自测
> 窗口为了隔离会把它摘掉，所以 `rt_throttle_test` 自己临时安装/摘除。

## 四、回归

`RT_TEST=1` 的 `rt-throttle` 场景（`tests/unit-test/rt_throttle_test.c`）：

- NORMAL(10) 与 FIFO(80) 同时就绪，各自纯忙循环；
- 窗口 2.5s（2.5 个默认周期）；
- 断言：RT 跑过、NORMAL 跑过、NORMAL 迭代数 ≥ RT 的 1%、至少进入过
  一次限流（`events > 0`）。

修复前后（x86_64 SMP=1）：

```text
before: === RT THROTTLE TEST: FAIL rt_iters=146861037 normal_iters=0 ===
after : === RT THROTTLE TEST: PASS rt_iters=151858143 normal_iters=5809629 events=2 ===
```

`tests/suites/rt_test.py` 会解析 `RT THROTTLE TEST: PASS` 并校验上面三个
条件。

## 五、已知限制

1. **tick 粒度**：预算检查挂在 100Hz tick 上，最多超预算一个 tick
   （10ms）；对 1s 周期是 1%。要高精度可以改成 hrtimer 周期定时器。
2. **配置接口不并发安全**：`sched_rt_throttle_set()` 直接写所有 CPU 的
   字段，只适合在 RT 负载起来之前调用（测试/调试）。
3. **没有 `/proc` 旋钮**：暂不暴露到用户态；默认值编译期固定。
4. **只限制 RT 类**：NORMAL 之间仍是 tick 轮转，没有 CFS/公平调度。
5. 没有 deadline class，也没有像 Linux 那样把 throttled 任务单独挂链；
   这里只是让 `pick_next()` 暂时跳过整个 RT 类。
