# Avatar OS 实时测试路线图

> 2026-11-12 定稿。后续 RT 测试按本文推进；每次里程碑完成后，把结果补进
> `CYCLICTEST.md`、`RT_DEMO_RESULTS.md`、`R0_BASELINE.md`，不要只留在聊天/CI 日志里。

## 1. 目的

把“Avatar OS 到底有多实时”从一次 demo 变成可持续的工程指标：

- 每次改调度/hrtimer/中断/锁，都有一组**可复现、可比较**的测试；
- 测的是**最坏情况**（p99.9/max/overrun），不是平均值；
- 区分 **QEMU/TCG / QEMU/KVM / 裸机**，区分**内核 RT 线程 / 用户态 RT / 端到端**；
- 逐步从“轻载 CPU hog”走到“真实机器人负载 + 72h soak + 硬件示波器”。

## 2. 当前覆盖（T0，已完成）

| 测试 | 覆盖 | 位置 |
| --- | --- | --- |
| `RT_TEST=1` 场景 1–3 | RT 优先级顺序、FIFO 不被 tick 轮转 | `tests/unit-test/rt_sched_test.c` |
| 场景 4 | NORMAL→RT 立即抢占（self-IPI） | 同上 |
| 场景 5 | 模板 worker + 共享页（create/start/stop/destroy、1ms/10ms） | 同上 |
| 场景 5b | 统计窗口 RESET（保留 out_seq、清共享页镜像）+ watchdog checks/trips | 同上 |
| 场景 6 | 普通 mutex SMP 并发 + `rt_mutex` 直接 PI | `rt_mutex_test.c` |
| 场景 7 | `rt_event` any/all/timeout + event 模板共享环 | `rt_event_test.c` |
| 场景 8 | **内核态 cyclictest**：FIFO 80、1ms/10ms 绝对 deadline、NORMAL hog、hog_iters；x86 额外拆出 timerlat IRQ 延迟 | `cyclictest.c` |
| 场景 9 | **内核态 oslat**：FIFO 85 忙循环 500ms，记录执行间隔和 1/10/100µs 超阈值次数 | `oslat.c` |
| `RT_DEMO=1` | RT ping-pong 唤醒 + NORMAL→RT 抢占 | `rt_demo.c` |
| R0/R0.5 | `/proc/rtstat`：wake/syscall/tick_gap；`RT_IRQ_TRACE=1` 真 IRQ-off + preempt-off | `kernel/rt/rt_latency.c` |
| preemptoff 自测 | `RT_TEST=1 RT_IRQ_TRACE=1` 时持 50µs preempt-off，断言计数/max | `tests/unit-test/rt_sched_test.c` |
| 回归门禁 | `make -C tests rt-test` 三架构；SMP=1/4 | `tests/suites/rt_test.py` |

**当前基线（不是目标）：**

- x86_64 KVM，1ms 周期，2000 样本，CPU hog：
  - p50 ≈ 2.4µs，p99 ≈ 4.6–7.3µs，p99.9 ≈ 10.8–17.8µs，max ≈ 12.7–19.3µs；
  - overrun 0，`hog_iters` ≈ 64.7 亿次/2.1s；
- x86_64 KVM 调度器路径：RT ping-pong / NORMAL→RT 约 2.1–2.3µs；
- IRQ-off：x86 KVM ≈ 0.36ms；riscv64 TCG ≈ 1.74ms（TCG 放大，仅同架构对比）；
- aarch64/riscv64 目前**没有 oneshot hrtimer**，cyclictest 回退到 10ms tick；
  绝对值不可与 x86 1ms 比较。

## 3. 测试原则

1. **同硬件、同测量点、同时长、同负载**才可比：
   - 裸机对裸机，KVM 对 KVM；内核线程对内核线程，用户态对用户态；
   - 至少跑 10k 周期；正式基线跑 1h，稳定性跑 72h。
2. **看尾部**：p50 只说明路径成本；验收看 p99、p99.9、max、overrun。
3. **区分场景**：
   - 空载 / 单 CPU hog / 多 RT / 网络 / 磁盘 / NPU / IRQ 风暴；
   - 单核 / SMP / CPU 隔离 / vCPU RT。
4. **每轮记录**：日期、硬件、QEMU/KVM/TCG、内核 config、SMP、周期、负载、
   样本数、p50/p99/p99.9/max、overrun、`hog_iters`、IRQ-off max。
5. **不要**拿 QEMU TCG 的绝对值做产品结论；不要用平均值代替尾部。

## 4. 测试矩阵

| 维度 | 取值 |
| --- | --- |
| 架构/环境 | x86_64 KVM、x86_64 TCG、aarch64 TCG、aarch64 RK3588 裸机、riscv64 TCG、SG2002 裸机 |
| SMP | 1 / 2 / 4 / 8（按板子） |
| 中断控制器 | GICv2 / GICv3 / PLIC / LAPIC |
| 周期 | 10ms / 1ms / 100µs（有 oneshot 后） |
| 负载 | 无 / 1 CPU hog / N RT 任务 / 网络 flood / 磁盘 I/O / NPU 推理 / IRQ 风暴 |
| 时长 | 2s 冒烟 / 1h 基线 / 72h soak |
| 测量点 | 内核 RT 线程 / 用户态 RT / 端到端 GPIO+示波器 / vCPU kick→run |

**门禁候选（按产品和硬件再定阈值）：**

- `overruns == 0`（1h/72h）；
- 1kHz 控制环：p99 < 50µs、max < 100µs（示例，不是定稿）；
- IRQ-off max < 20–50µs；
- `hog_iters > 0`（负载真的运行过）；
- 无 assert/panic、无 deadlock、无 watchdog 误触发。

## 5. 路线图

### T0：现有基线（已完成，2026-11-12）

- RT_TEST 场景 1–8 + RT_DEMO；
- 内核态 cyclictest + `hog_iters` + timerlat IRQ/Thread 拆分（x86）
  + oslat + `tests/suites/rt_test.py` 门禁；
- 文档：`CYCLICTEST.md`、`OSLAT.md`、`RT_DEMO_RESULTS.md`、`R0_BASELINE.md`；
- 三架构 `make -C tests rt-test` 通过。

### T1：裸机延迟实验室（下一步，2–4 周）

**目标：** 把“QEMU 数字”变成“目标硬件数字”。

- [ ] x86_64 裸机启动 Avatar OS，跑同一 cyclictest；
- [ ] RK3588 裸机启动，跑同一 cyclictest；
- [ ] aarch64/riscv64 接 **oneshot hrtimer**（ARM generic timer / SBI timer），
      周期从 10ms 降到 1ms（后续 100µs）；
- [ ] CPU 隔离：RT 核不参与负载均衡、不接普通 IRQ、tick 可停；
- [ ] CPU affinity / IRQ affinity 修复与验证（含 `sched_setaffinity` 122/123 bug）；
- [ ] 负载矩阵第一版：CPU hog、网络 flood、磁盘 I/O；
- [ ] 1h 基线；记录 p50/p99/p99.9/max/overrun/IRQ-off max；
- [ ] GPIO 翻转 + 示波器端到端测量方法落地（1kHz 方波）。

**验收：** 同一硬件同一负载下，有/无隔离的对比数据；1h 无 overrun；
文档写入 `CYCLICTEST.md`/`R0_BASELINE.md`。

### T2：用户态 RT 与内存确定性（1–2 个月）

**目标：** 让机器人应用而不是内核线程跑 RT。

- [ ] `sched_setscheduler` / `sched_setattr` / `sched_getattr`；
- [ ] `mlockall` / `mlock` / 栈预缺页；
- [ ] `clock_nanosleep`（绝对时间）、`timer_create`/`timerfd`；
- [ ] PI futex；完整 PI 链/多锁传播（RT_LOCKS V1 限制）；
- [ ] 用户态 `/bin/cyclictest`，与内核版同场景对比；
- [ ] `libavatar-rt`：周期任务、绝对 deadline、shared-memory SPSC/MPSC、
      PI mutex、watchdog、safe-state；
- [ ] 内存确定性测试：RT 循环内无 page fault、无动态分配、无 syscall。

**验收：** 用户态 cyclictest 1ms 周期在目标硬件上达到预算；内核/用户态
差值可解释；mlock/prefault 测试通过。

### T3：重载、多 RT 与虚拟化（2–3 个月）

**目标：** 从“单 RT + CPU hog”走到“真实机器人负载”。

- [ ] 多 RT 任务 + 不同优先级 + PI/优先级反转；
- [ ] 网络 flood、磁盘 I/O、NPU 推理并行下的 cyclictest；
- [ ] IRQ 风暴/中断亲和性/线程化 ISR；
- [ ] SMP=4/8 下跨核干扰、cache/memory bandwidth；
- [ ] vCPU kick→run；RT vCPU 优先级/亲和性；
- [ ] VMM 分区：RT 分区 + Linux guest（ROS 2/AI）+ 共享内存；
- [ ] 长尾定位：IRQ-off、锁、驱动、页错误、SMI/BIOS。

**验收：** 负载矩阵下的 p99.9/max 报告；无 overrun 或明确的 miss 预算；
1h 稳定。

### T4：72h soak、故障注入与 CI 硬件门禁（持续）

- [ ] 72h soak（空载 + 重载各一轮）；
- [ ] 故障注入：kill RT 任务、watchdog 超时、safe-state；
- [ ] 长跑 p99.9/max 漂移、内存泄漏、任务泄漏；
- [ ] 自托管 runner 跑硬件 RT 门禁（不是只跑 QEMU）；
- [ ] 每次 RT 相关 commit 跑 10k 周期冒烟 + 1h nightly。

### T5：安全/信息安全/认证（6–12 个月+，按产品决定）

- [ ] W^X、SMAP/PAN、用户指针审计、故障隔离；
- [ ] secure boot、A/B、签名更新、回滚；
- [ ] TSN/PTP、CAN FD、传感器时间戳；
- [ ] 车规 SoC 移植（SMMU/TSN/安全岛/锁步）；
- [ ] ISO 26262 / IEC 61508 安全案例（或安全岛 + QM Linux 的 ASIL 分解）。

## 6. 命令与工具

```bash
# 三架构 RT 冒烟（含 cyclictest）
make -C tests rt-test

# 指定架构/SMP
make -C tests rt-test ARCHES=x86_64 SMP=4

# 手工启动 + 串口日志
make PLATFORM=qemu-virt-x86_64 RT_TEST=1 LOG=info run
make PLATFORM=qemu-virt-x86_64 RT_TEST=1 LOG=info run-fs

# 真实 IRQ-off 跟踪（会全量重编）
make PLATFORM=qemu-virt-x86_64 RT_IRQ_TRACE=1 kernel

# RT ping-pong / 抢占 demo
make PLATFORM=qemu-virt-x86_64 RT_DEMO=1 LOG=info run

# 画 cyclictest 汇总图
python3 tools/plot_cyclictest.py \
  --period-ns 1000000 --warmup 100 --samples 2000 \
  --min-ns ... --avg-ns ... --p50-ns ... \
  --p99-ns ... --p999-ns ... --max-ns ... \
  --overruns 0 --early 0 --out docs/realtime/cyclictest_1ms.png
```

## 7. 结果记录模板

每次里程碑结果至少写：

```text
日期 / commit:
硬件 / 环境:      x86_64 KVM | x86_64 裸机 | RK3588 裸机 | ...
内核 config:       PLATFORM=... SMP=... GIC=... LOG=...
测试:              cyclictest kernel/user | RT_DEMO | ...
周期 / 样本 / 时长: 1000000 ns / 2000 / 2.1 s
负载:              1 NORMAL hog / network flood / ...
p50 / p99 / p99.9 / max:
overruns / early:
hog_iters:
IRQ-off max:
备注/异常:
```

## 8. 不要做的事

- 不要用 QEMU TCG 的绝对延迟做产品结论；
- 不要只报 avg；必须报 p99.9/max/overrun；
- 不要并行跑 QEMU/构建（TCG 会失真）；
- 改头文件/`SMP=`/`LOG=`/`GIC=` 后先 `make clean`（构建不跟踪头依赖）；
- 不要在 RT 循环里 syscall、分配、VFS、页错误；
- 不要把“软实时内核”宣传成硬实时/ASIL 认证；
- 不要用消费级板子做车规承诺。

## 9. 相关文档

- `ARCHITECTURE.md`：RT 任务模型、调度类、R0–R6 状态；
- `CYCLICTEST.md`：当前 cyclictest 场景、指标、图；
- `RT_DEMO_RESULTS.md`：self-IPI 前后 ping-pong/抢占数据；
- `R0_BASELINE.md`：wake/syscall/tick_gap/IRQ-off 基线；
- `RT_LOCKS.md`：mutex/rt_mutex PI 与已知限制；
- `RT_EVENTS.md`：rt_event/event 模板；
- `RT_TEMPLATES.md`：模板 worker/rtdev 契约；
- `RT_USER_DEMO.md`：用户态 rt-demo。
