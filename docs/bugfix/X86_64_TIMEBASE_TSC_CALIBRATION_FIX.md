# x86_64 时间基准：PIT 标定失效导致 CLOCK_MONOTONIC 快 1000 倍

**日期**: 2026-09-17
**影响架构**: x86_64（`ARCH_X86_64`）
**严重性**: 高（全系统墙上时钟尺度错误）
**触发条件**: `PLATFORM=qemu-virt-x86_64` 用 `run-net`（`-machine microvm`）启动

---

## 一、症状：LTP 唯一失败的测例

`SMP=1` 和 `SMP=4` 下，LTP 25 个测例里 `epoll_wait04` 稳定失败：

```
epoll_wait04.c:35: TFAIL: epoll_wait() waited for 7052us with a timeout equal to zero
```

测例要求 `epoll_wait(epfd, evs, 1, 0)`（**timeout=0**）必须立即返回，允许误差 1000us。
日志里稳定报 ~5200us，不是随机抖动。

`epoll_wait` 的实现本身没问题（`kernel/syscall/io/epoll.c` 的
`epoll_pwait_handler` 在 `timeout_ms == 0` 且无就绪项时直接 `regs[0] = 0; return;`）。
**真实耗时只有约 5.2 微秒 —— 是时钟把它报成了 5.2 毫秒。**

---

## 二、根因：PIT 标定依赖一个 microvm 上不存在的端口

`driver/irq/lapic.c` 的 `lapic_timer_init()` 用 PIT channel 2 量一段 "10ms"，
再拿这段时间里的 TSC 增量反推 TSC 频率：

```c
g_tsc_freq_hz = (tsc_after - tsc_before) * 100ULL;   /* ×100 = 每秒 */
```

而那段 "10ms" 是靠**读 port 0x61 的 bit5（PIT channel 2 的 OUT 位）**等出来的：

```c
while ((inb(PIT_CAL_PORT_CTRL) & 0x20u) == 0)   /* 0x61 bit5 = OUT */
    ;
```

**问题在于 port 0x61 不是 PIT 的端口，而是南桥（PIIX/ICH）实现的。**
`run-net` 用的 `-machine microvm` 没有南桥，该端口未实现。实测：

| 机器 | `inb(0x61)` |
|------|-------------|
| `-machine q35`（`run` 用） | `0x30` —— 有南桥，写命令后 OUT 会真的变化 |
| `-machine microvm`（`run-net` 用） | **`0xff`** —— 恒为全 1 |

`0xff` 的 bit5 恒为 1，等待循环**立刻退出**，"10ms" 实际只剩几次 port I/O 的时间
（约 12us），于是：

```
TSC frequency: 3 MHz          ← 真实值约 2419 MHz，差了约 800 倍
```

`g_tsc_freq_hz` 偏小 ⇒ `timer_counter_to_ns()`（`driver/timer/timer.c`）
算出的纳秒数偏大 ⇒ **`CLOCK_MONOTONIC` 快约 1000 倍**。
真实的 5.2us 被报成 5200us，`epoll_wait04` 于是"超时"。

> 影响面不止这一个测例：`nanosleep`、`select`/`poll`/`epoll` 的超时、
> `clock_gettime`、进程 CPU 时间统计（`stime_ns`）全部建立在这个频率上。
> 同一份错误还让 LAPIC timer 的中断周期算成 ~10ms 的 1/4.5，tick 频率远高于 100 Hz。

---

## 三、修复

**核心思路：标定不再碰 port 0x61，只读 PIT 自己的计数器。**

`driver/irq/lapic.c`：

1. `pit_start_oneshot()` —— channel 2 以 mode 0 从 0xFFFF 起倒计数（只用 0x43/0x42）
2. `pit_latch_read()` —— `0x80` latch 后从 0x42 读回当前计数（只用 0x43/0x42）
3. `pit_poll_tsc_freq()` —— 装好满量程 one-shot，自旋固定一批 TSC tick，
   再 latch 读回这段时间 PIT 走了多少 count：
   ```
   TSC 频率 = 自旋的 TSC tick 数 / (PIT count / 1.193182 MHz)
   ```
   实测 `PIT counter 990 counts over 2000000 tsc ticks` → **2410 MHz**，
   与宿主真实值（CPUID.15H: 38.4MHz × 126/2 = 2419.2 MHz）误差 0.4%
4. `pit_wait_10ms()` —— 只在 TSC 频率未知时，用同一套计数器轮询等一个 10ms 量级窗口

**另外两条防御**：

- **合理性校验**：`TSC_MIN_PLAUSIBLE_HZ`（50 MHz）。低于它一律判失败，
  不再把值写进 `g_tsc_freq_hz`。**正是少了这道校验，3 MHz 这种明显错误的值
  才能一路传到 CLOCK_MONOTONIC。**
- **失败即退化，而非用错值**：两条路都拿不到频率时 `g_tsc_freq_hz = 0`，
  此时 `timer_counter_to_ns()` 会退化成按 `g_system_ticks` 算的粗粒度时钟
  （分辨率 = 1 tick，但**量级正确**）。宁可分辨率差，也不要尺度错。

顺带把 `PIT_CAL_PORT_CTRL`(0x61) 从 `driver/irq/lapic.h` 删掉，
换成一段说明，避免以后又有人拿它判断 PIT 状态。

**CPUID 这条路为什么没成为主路径**：代码里保留了 `CPUID.15H` / `CPUID.16H`
探测（对裸机和别的 VMM 有用），但实测 **QEMU 8.2 的 `-cpu host` 在客户机里
把这两叶全部置零**（宿主本身是 `15H=[2,126,38400000]`、`16H.base=2400`），
所以本项目的 QEMU 场景实际都会落到 PIT 实测那条路。

---

## 四、验证结果

| 配置 | TSC 频率 | 异常数 | LTP |
|------|----------|--------|-----|
| SMP=4 / microvm 修复前 | 3 MHz | 0 | 24 PASS / **1 FAIL** |
| SMP=4 / microvm 修复后 | 2410 MHz | 0 | **25 PASS / 0 FAIL** |
| SMP=1 / microvm 修复后 | 2412 MHz | 0 | **25 PASS / 0 FAIL** |
| SMP=4 / **q35** | 2417 MHz | — | `epoll_wait04` TPASS |

`epoll_wait04` 在 microvm 与 q35 上均转为 `TPASS`。
aarch64 / riscv64 全量重编通过（本次改动仅涉及 x86_64 的 lapic 驱动）。

---

## 五、检查清单

- [ ] x86_64 上**任何**依赖 PIT 的代码，只能用 0x43（命令）/ 0x42（数据），
      **不要用 0x61** —— 那是南桥的端口，microvm 上没有
- [ ] 频率/时间类标定结果一律加合理性区间校验，不要无条件采信
- [ ] 标定失败时选择"退化为粗粒度"而不是"采用可疑值"
- [ ] 怀疑时钟不对时，先看启动日志里的 `TSC frequency:` 一行是否在合理区间
      （现代 x86_64 是 GHz 量级；打印出 MHz 量级基本就是标定坏了）
- [ ] `run`（q35）与 `run-net`（microvm）设备模型不同，
      **只在其中一个上验过的驱动假设，另一个上未必成立**
