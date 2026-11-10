# 用户可配置 RT worker（模板）

**状态**：V1 已实现并通过 x86_64（KVM）+ aarch64/riscv64（TCG）构建验证；
用户态工具 `/bin/rtctl` 可以创建、启动、停止、喂输入、读输出和统计。
x86_64 已支持 **1ms 周期**（LAPIC TSC-deadline oneshot hrtimer）。

## 一、定位

- **数据面**：内核 RT 任务（`TASK_DOMAIN_KERNEL + SCHED_FIFO`），按
  `period_ns` 周期调用一个**模板**的 `step()`。`step()` 必须有界、无分配、
  无睡眠、无 VFS、不长时间持锁。
- **控制面**：用户态通过 `/dev/rtctl` 的 ioctl 配置/控制 worker。
- **设备面**：模板输出可选绑定到一个 `rtdev`（`include/rtdev.h`）。
  设备在 CREATE 时独占绑定，STOP/DESTROY 时调用 `safe_state()`。

周期由 **hrtimer** 驱动（`kernel/time/hrtimer.c`）：

- **x86_64**：LAPIC Timer 切到 TSC-deadline oneshot 模式，每核把
  `min(下一个 100Hz tick, 本核最早的 sleep_expire_ns)` 写进
  `IA32_TSC_DEADLINE`。worker 最小周期 **1ms**（1000Hz）。
- **aarch64/riscv64**：暂未接 oneshot，回退到 100Hz tick；
  `period_ns` 仍必须是 10ms 的整数倍（CREATE 会拒绝其它值）。
- 100Hz 的 tick 记账（`g_system_ticks`、`sched_tick`、`itimer_tick`）
  仍然保留；hrtimer 只额外增加精确唤醒点。

## 二、使用

```sh
# 周期 20ms，增益 1.0，输出写 vout 设备
rtctl create demo periodic_io 50 0 20 65536 0 0 0 dev:vout
rtctl start 1
rtctl set 1 21          # 输入 21
rtctl get 1             # 输出 21，seq 持续增加
rtctl stats 1           # ticks / misses / overruns / exec_max
rtps                    # demo 出现在 RT 任务列表，SCHED_FIFO prio 50
rtctl stop 1
rtctl destroy 1
```

实测（x86_64 KVM）：

```text
/ # rtctl create demo periodic_io 50 0 10 131072 0 0 1000 dev:vout
created id=1 name=demo tpl=periodic_io
/ # rtctl start 1
/ # rtctl set 1 21
/ # rtctl get 1
id=1 value=42 seq=200
/ # rtctl stats 1
ticks=300 misses=0 overruns=0 exec_last=304 ns exec_max=1544 ns
wake_late_last=... ns wake_late_max=... ns
input=21 output=42 seq=300 dev=vout dev_writes=300
```

### 1ms（x86_64）

```sh
rtctl create fast periodic_io 50 0 1 65536 0 0 0
rtctl start 1
sleep 10
rtctl stats 1
```

实测（x86_64 KVM，10 秒）：

```text
ticks=10005 misses=1 overruns=0 exec_last=202 ns exec_max=3297 ns
wake_late_last=260775 ns wake_late_max=1684160 ns
```

- 10005 个周期 ≈ 1ms/周期；1 次 miss（宿主 KVM 抖动把某次唤醒拖过半个周期）。
- `wake_late_*` 是相对理论 deadline 的绝对延迟，包含启动相位和宿主 KVM
  调度抖动；安静时（SMP=4 双 worker 测试）`wake_late_last` 在 **3–6 µs**。
  判断“周期是否稳定”看 ticks 和 misses；判断“尾延迟”看 `wake_late_max`。
- 同一路径也提高了用户态 `nanosleep` 的唤醒精度（`sleep_expire_ns`）。

PID 模板：

```sh
# kp=1.0, ki=0, kd=0, out=[0,1000], setpoint=500, integral_limit=100000
rtctl create pid1 pid 60 0 10 65536 0 0 0 1000 500 100000
rtctl start 1
rtctl set 1 100        # 测量值
rtctl get 1            # (500-100)*1.0 = 400
```

## 三、ABI（`include/rt_worker.h`）

| ioctl | 作用 |
|---|---|
| `RTW_IOC_CREATE` | 创建 worker（模板/优先级/CPU/周期/参数/设备），返回 id |
| `RTW_IOC_START` | 创建 RT 任务并开始周期执行 |
| `RTW_IOC_STOP` | 置 stop 并唤醒；worker 在下一个周期边界退出 |
| `RTW_IOC_DESTROY` | 释放 worker 和绑定设备；RUNNING 时返回 EBUSY |
| `RTW_IOC_SET_IN` | 原子更新输入值 |
| `RTW_IOC_GET_OUT` | 读最近一次输出和序号 |
| `RTW_IOC_STATS` | ticks/misses/overruns/exec_last/exec_max/... |
| `RTW_IOC_LIST` | 列出所有 worker |

**权限**：V1 只允许 uid 0。调用者传入的用户指针一律经
`copy_from_user_bytes` / `copy_to_user_bytes`，不做裸解引用。

## 四、模板 V1

### `periodic_io`

参数（Q16 定点，1.0 = 65536）：

| 参数 | 含义 |
|---|---|
| `p0` | gain_q16 |
| `p1` | offset |
| `p2` | min（`min >= max` 表示不限幅） |
| `p3` | max |

`out = clamp((in * gain) >> 16 + offset, min, max)`

### `pid`

参数：

| 参数 | 含义 |
|---|---|
| `p0/p1/p2` | kp/ki/kd（Q16） |
| `p3/p4` | out_min/out_max |
| `p5` | setpoint |
| `p6` | integral_limit（0 = 默认 2^40） |

定点实现：积分限幅、微分取相邻周期误差差、输出限幅。输入/增益都有
饱和限制，不会溢出 int64。

## 五、rtdev 设备契约

```c
struct rtdev_ops {
    int (*setup)(struct rtdev *d, const void *cfg, uint32_t len); /* 控制面 */
    int (*write)(struct rtdev *d, const void *buf, uint32_t len); /* RT 面 */
    int (*read)(struct rtdev *d, void *buf, uint32_t len);
    int (*ioctl_rt)(struct rtdev *d, uint32_t cmd, uint64_t arg);
    void (*safe_state)(struct rtdev *d);
    void (*teardown)(struct rtdev *d);                            /* 控制面 */
};
```

- `setup/teardown` 可以睡眠、分配、开关电源；`write/read/ioctl_rt` 必须是
  有界、无分配、无睡眠、无 VFS、短临界区。
- 一个设备同时只能绑给一个 worker；`CREATE` 时 `rtdev_claim()` 独占。
- V1 内置示例设备 `vout`：把每次写记录成 u64，`teardown` 打一行
  `[rtdev] vout: N writes`。真实 GPIO/PWM/UART 驱动按同一契约注册即可；
  没有通过 RT 一致性要求的驱动不要注册 `rtdev_ops`。

## 六、当前限制

- x86_64 支持 1ms 周期；aarch64/riscv64 仍是 10ms 整数倍（未接 oneshot）。
- hrtimer 是**每核**的：worker 的亲和性决定用哪颗核的 TSC-deadline；
  目标核不支持 oneshot 时，CREATE 会拒绝非 10ms 整数倍的周期。
- 输入/输出走 ioctl，还没有共享内存 mmap 快路径；用户 supervisor 的
  `set/get` 是 syscall，但 **RT 循环本身不做 syscall**。
- 模板参数只在 CREATE 时生效，还没有运行期原子更新参数。
- 没有 `/proc/rtworkers`；用 `rtctl list` / `stats` 和 `rtps` 观察。
- 没有真正的 GPIO/PWM/UART `rtdev`；V1 只有 `vout` 示例。
- 模板数量：2（`periodic_io`、`pid`）；`fsm/event/watchdog` 是下一步。
- RT 任务在 `STOP` 后最多要等一个周期才退出。

## 七、下一步

1. 共享内存快路径：`/dev/rtctl` 增加 mmap 或复用 ION/dmabuf，把输入/输出
   放到用户态可映射的页里，消除 supervisor 的 ioctl 往返。
2. 真实 rtdev：GPIO/PWM/ADC 先做，UART 走预分配 TX 环 + NORMAL drainer。
3. `/proc/rtworkers` 与 rt-test 自动门禁。
4. oneshot hrtimer，支持 1kHz。
5. `fsm` / `event` / `watchdog` 模板。
6. 运行期参数双缓冲更新。
