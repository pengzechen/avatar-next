# `/bin/rt-demo`：用户态 RT 演示

一个程序串起现有 RT 设施：event 模板、1ms/10ms 周期 worker、共享页
输入、watchdog、`/proc/rtworkers`。

## 启动

```bash
# 1. 内核 + rootfs（rootfs 会把 /bin/rt-demo 打进去）
make PLATFORM=qemu-virt-x86_64 kernel
make PLATFORM=qemu-virt-x86_64 rootfs

# 2. 启动
make PLATFORM=qemu-virt-x86_64 run-fs

# 3. 宿主 shell 里
/bin/rt-demo
```

x86_64 用 KVM 时周期 worker 跑 **1ms**（宿主约 2.4GHz TSC-deadline）；
aarch64/riscv64 或没有 oneshot 的 x86 TCG 会自动回退到 **10ms**。
程序自己会打印用了哪个周期。

## 它做什么

1. **event worker**（prio 50，period=0）：supervisor 连续 `POST` 200 个
   事件；每投一个就等共享页 `out_seq` 变化，统计
   `post → out_seq 可见` 的延迟（包含 ioctl 返回和 RT 抢占）。
   `step = event * 2.0`，最后输出应为 400。
2. **周期 worker**（prio 60，x86 1ms）：通过 mmap 共享页写 `input=123`，
   运行 1 秒，打印 ticks/misses/exec_max/wake_max/dev_writes。
   绑定 `vout`，每次 step 都算一次设备写。
3. **watchdog**（prio 70）：监控周期 worker；先确认健康输出 0，
   再停掉周期 worker，150ms 后 watchdog 必须输出安全值 **999**。
4. 打印 `/proc/rtworkers`（含 `EVENTS`/`DROPS` 列），然后清理。

## 期望输出（x86_64 KVM 实测）

```text
=== Avatar RT demo ===
[event] n=200 min=10777 avg=11032 max=14474 ns timeouts=0
[event] ticks=200 posted=200 processed=200 dropped=0 output=400
[PASS] event-driven worker: 200 events, output = last event * 2
[periodic] period=1000000 ns ticks=1000 misses=0 exec_max=11150 ns wake_max=113401 ns dev_writes=1000 output=123
[PASS] periodic worker: shared input -> output, vout writes counted
[watchdog] target healthy output=0
[watchdog] after target stop output=999 (safe=999)
[PASS] watchdog: safe output after target stopped
--- /proc/rtworkers ---
ID NAME             STATE    TPL          PRIO CPU PERIOD_NS      TICKS MISSES OVERRUNS EXEC_MAX_NS WAKE_MAX_NS     EVENTS  DROPS DEV
 1 evt              RUNNING  event          50   0         0        200      0        0         121           0        200      0 -
 2 pulse            STOPPED  periodic_io    60   0   1000000       1203      0        0       11150     1020093          0      0 vout
 3 wd               RUNNING  watchdog       70   0   1000000        352      0        0        1612      174522          0      0 -
-----------------------
=== Done ===
```

- 数值随宿主负载变化；`ticks`/`dev_writes` 应接近 `1s / period`。
- `post→out_seq` 延迟包含用户态 `clock_gettime` 和轮询，因此是
  “端到端可见延迟”，不是纯中断延迟。
- 退出码 0 表示三组 `[PASS]` 全过；测试框架的 apps suite 也会跑它。

## 只手动玩单个 worker

```sh
# event：每投一事件，output = 事件值 * 2
rtctl create evt event 50 0 0 131072
rtctl start 1
rtctl post 1 21
rtctl get 1                 # value=42 seq=1
rtctl stats 1
rtctl shm 1                 # 看环计数
rtctl stop 1; rtctl destroy 1

# 1ms 周期（无 oneshot 的架构用 10ms）
rtctl create fast periodic_io 60 0 1 65536 0 0 0
rtctl start 2
rtctl set 2 123
rtctl stats 2
rtctl stop 2; rtctl destroy 2
```

完整规格见 `RT_TEMPLATES.md`、`RT_EVENTS.md`、`RT_LOCKS.md`。
