# RK3588 移植指南

本文记录将 Avatar OS 移植到 Rockchip RK3588 的完整过程，包括每一个关键 Bug 的根因分析和修复方案。

---

## 硬件概览

| 项目 | 描述 |
|------|------|
| SoC | Rockchip RK3588 |
| CPU | 4× Cortex-A55 (小核) + 4× Cortex-A76 (大核)，共 8 核 |
| 架构 | AArch64，VHE (EL2) 模式 |
| RAM | 通常 4–16 GB LPDDR5 |
| UART | DesignWare UART2，物理地址 `0xFEB50000`，reg_shift=2 |
| GIC | GICv3，GICD `0xFE600000`，GICR `0xFE680000`，每核 stride=128KB |
| 固件 | ATF (BL31) 常驻 EL3，提供 PSCI 0.2 |
| 启动 | U-Boot TFTP → `go 0x400000` |

### MPIDR 拓扑

| CPU 逻辑 ID | MPIDR | 核型 |
|-------------|-------|------|
| 0 | `0x000000` | A55 |
| 1 | `0x000100` | A55 |
| 2 | `0x000200` | A55 |
| 3 | `0x000300` | A55 |
| 4 | `0x000400` | A76 |
| 5 | `0x000500` | A76 |
| 6 | `0x000600` | A76 |
| 7 | `0x000700` | A76 |

> 注：Aff1 字段（bits[15:8]）= 逻辑 CPU 编号（0–7），Aff0 字段始终为 0。

---

## 平台配置

构建命令：

```bash
make ARCH=aarch64 PLATFORM=rk3588-aarch64 SMP=8 LOG=info kernel
```

平台文件：`platforms/rk3588-aarch64/platform.lua`

内存布局（`kernel/mm/aarch64/vm_early.c`）：

| 区段 | 物理地址 | 说明 |
|------|----------|------|
| RAM Bank0 | `0x00000000–0x3FFFFFFF` | 普通内存 |
| RAM Bank1 | `0x40000000–0x7FFFFFFF` | 普通内存（内核加载在 0x400000）|
| RAM Bank2 | `0x80000000–0xBFFFFFFF` | 普通内存 |
| MMIO | `0xC0000000–0xFFFFFFFF` | 设备内存（UART/GIC/NPU 等）|

---

## 移植过程与 Bug 记录

### Bug 1：UART reg_shift 导致串口输出乱码

**现象**：内核启动后串口完全无输出或输出乱码。

**根因**：
DesignWare UART 每个寄存器占 4 字节（reg_shift=2），而 QEMU virt 默认的 16550 UART 每个寄存器占 1 字节（reg_shift=0）。若使用错误的 reg_shift，LSR 的地址将偏移到非法位置（如 `base+0x05` 而非 `base+0x14`），导致轮询等待发送就绪时永远卡死。

**修复**：
- 在 `vm_early.c` 的 `vm_init()` 中，`#ifdef PLATFORM_RK3588` 区块提前硬编码：
  ```c
  dw_uart_base      = 0xFEB50000UL;
  dw_uart_reg_shift = 2;
  ```
- 此修复必须在 `KLOG_INFO` 第一次调用前执行（`vm_init` 是第一个合适的时机）。

---

### Bug 2：PSCI CPU_ON MPIDR 编码错误

**现象**：PSCI CPU_ON 对 CPU1–7 返回错误码（ATF 拒绝 MPIDR）。

**根因**：
代码中使用 `mpidr = i`（直接用逻辑编号），但 RK3588 ATF 要求 MPIDR 按实际硬件格式传递（Aff1=i，Aff0=0），即 `mpidr = i << 8`。

**修复**（`kernel/task/cpu.c`）：
```c
#if defined(PLATFORM_RK3588)
    c->hw_id = (uint64_t)i << 8;  // Aff1=i, Aff0=0
#else
    c->hw_id = i;
#endif
```

---

### Bug 3：GICv3 secondary init 顺序错误

**现象**：secondary 核 GIC 初始化挂死（卡在等待 ChildrenAsleep）。

**根因**：
初始化顺序错误 —— 等待 GICR 的 `ChildrenAsleep` 清零时，`ICC_SRE_EL1` 还未开启，CPU 接口不可用，后续 ICC 寄存器操作触发异常。

**修复**（`driver/irq/gicv3.c`，`gicv3_init_secondary()`）：
```
正确顺序：
1. msr ICC_SRE_EL1, #0x7 + ISB  （先开 SRE，CPU 接口才可用）
2. GICR_WAKER.ProcessorSleep = 0 + DSB
3. 轮询等待 ChildrenAsleep = 0
4. ICC_PMR / ICC_CTLR / ICC_IGRPEN1
```

---

### Bug 4：BSP 缺少 ICC_SRE_EL2 初始化

**现象**：RK3588 真实硬件上 BSP 的 timer 中断不触发，QEMU 正常（QEMU 默认 ICC_SRE=1）。

**根因**：
真实硬件复位后 `ICC_SRE_EL2=0`，不允许 EL1 访问 GIC CPU 接口。QEMU 默认 SRE=1 掩盖了此问题。

**修复**（`boot/aarch64/boot.S`，BSP EL2 init 段）：
```asm
mrs     x1, S3_4_C12_C9_5   /* ICC_SRE_EL2 */
orr     x1, x1, #0xF        /* SRE|DFB|DIB|Enable */
msr     S3_4_C12_C9_5, x1
isb
```
同样适用于 secondary 核的 EL2 init 段（必须两处都加）。

---

### Bug 5：Secondary 核在 mmu_init 前死锁（ATF 移交时 MMU 已开）

**现象**：所有 secondary 核 PSCI 返回成功（ATF AFFINITY_INFO=0 即 ON），但 `c->online` 永远不被设置。通过 early UART checkpoint 定位到死在 `mmu_init` 内部的 `tlbi alle2is` 或 `msr ttbr0_el1` 处。

**根因**：
RK3588 ATF (BL31) 在 PSCI CPU_ON 移交 secondary 时，**SCTLR_EL1.M = 1**（MMU 仍开着，使用 ATF 自己的页表）。我们的 `mmu_init` 直接写 `ttbr0_el1`/`tcr_el1`，下一条指令立即用半成品页表 walk，触发取指 translation fault；secondary 此时 VBAR_EL1 未设，直接挂死。

**修复**（`kernel/mm/aarch64/mmu.S`，`mmu_init` 入口）：
```asm
/* 先关 MMU（仅清 M 位），再切页表，再开 MMU+Cache */
mrs     x2, sctlr_el1
bic     x2, x2, #(1 << 0)   /* M = 0 */
msr     sctlr_el1, x2
isb
/* ... tlbi, 写 TTBR/TCR/MAIR ... */
/* 开 MMU 时同时打开 I/D cache */
orr x2, x2, #(1 << 0)   /* M */
orr x2, x2, #(1 << 2)   /* C - DCache（必须同时开，否则 walker 降级为 NC）*/
orr x2, x2, #(1 << 12)  /* I - ICache */
msr sctlr_el1, x2
```

**教训**：不能假设 EL2 入口 SCTLR 为 0；ATF/UEFI 都可能开着 MMU 移交。

---

### Bug 6：DCache 关闭导致 page-table walker 看不到 BSP 写的页表

**现象**：修复 Bug 5（仅清 M 位）后，secondary 核 `mmu_init` 走完 `msr sctlr_el1`（M=1）那条指令后仍挂死。

**根因**：
- BSP 调用 `vm_init()` 写入 `kernel_pt0/pt1`，由于 BSP DCache 开着，页表项仍在 BSP 的 DCache dirty 行中，尚未写回 RAM。
- Secondary 进入时 `SCTLR_EL1.C = 0`（DCache 关）。即便 `TCR_EL1` 设置了 `IRGN/ORGN=1`（cacheable）+ `SH=3`（inner shareable），**当 SCTLR.C=0 时，硬件强制将所有 cacheable 访问降级为 non-cacheable**，page-table walker 直接读 RAM，拿到的是 BSP 写之前的旧值（全 0）→ Level-0 translation fault。

**修复**：
开 MMU 时必须同时设置 `SCTLR_EL1.C = 1`（见 Bug 5 修复代码）。DCache 开启后，walker 通过 inner-shareable snoop 能看到 BSP dirty cache 中的最新页表项，无需显式 `dc cvac` 刷回 RAM。

---

### Bug 7：SMP timer test FAIL（CPU1 ticks 不增）

**现象**：SMP timer 健康检测报 `[smp-test] FAIL`，CPU1 的 `local_ticks` 在整个测试期间固定不变。

**根因**：
busybox 任务在 `cpu_smp_timer_test` 之前被 `task_create`，调度器在 SMP test 开始前把它调度到了 CPU1。busybox 进入 EL0 后连续执行 syscall（`brk`/`mmap` 等），syscall 入口关 IRQ，整个测试期间 CPU1 被 busybox 完全占据且收不到 timer 中断。

**修复**（`kernel/main.c`）：
调整启动顺序：
```
cpu_bring_up_all()    // SMP 拉起
timer_set_tick_cb()   // 安装 tick 回调
cpu_smp_timer_test()  // 先跑 SMP 健康检测
task_create("busybox") // 之后再创建 busybox
```

---

### Bug 8：多核并发 UART 输出乱码

**现象**：8 核 online 后，日志行相互交错，单行内容被其他核的字符插入。

**根因**：
`klog` 的底层 UART putchar 没有 spinlock 保护，多核同时写 UART 寄存器互相覆盖。

**修复**（`lib/klog.c` 或对应 UART 驱动）：
在每条完整日志消息的输出前后加 IRQ-safe spinlock（`spin_lock_irqsave` / `spin_unlock_irqrestore`），确保一条消息原子输出。

---

## 最终启动流程

```
U-Boot TFTP 加载 kernel_aarch64.bin 到 0x400000
  ↓
go 0x400000 → _start (EL2/VHE)
  ↓
BSP: ICC_SRE_EL2 初始化 → vm_init (页表) → mmu_init → 跳高 VA
  ↓
kernel_main: PMM → FS → Lua → GICv3 → Timer → NPU
  ↓
cpu_bring_up_all: PSCI CPU_ON x7 → 每核 _secondary_start → cpu_secondary_bootstrap
  ↓
timer_set_tick_cb → cpu_smp_timer_test (8核 PASS)
  ↓
task_create("busybox") → ELF 加载 → busybox shell
```

---

## 关键配置速查

### TCR_EL1

```c
/* tcr.h */
IRGN0/ORGN0/IRGN1/ORGN1 = 1  // Inner/Outer Cacheable Write-Back
SH0/SH1 = 3                   // Inner Shareable
T0SZ/T1SZ = 16                // 48-bit VA
```

> **必须设置 IRGN/ORGN/SH**，否则 page-table walker 做 non-cacheable 读，BSP 的 DCache dirty 页表项对 secondary 不可见。

### SCTLR_EL1 开 MMU 时

```c
M  = 1   // MMU enable
C  = 1   // DCache enable（必须！否则 walker 降级 non-cacheable）
I  = 1   // ICache enable
SPAN = 1 // 不自动设 PAN
```

### PSCI MPIDR（RK3588）

```c
// 逻辑 CPU i 的 MPIDR = i << 8（Aff1=i, Aff0=0）
hw_id = (uint64_t)i << 8;
```

---

## 调试技巧

### Early UART checkpoint（MMU 开启前）

在 `_secondary_start` 和 `mmu_init` 中用直接 MMIO 写 UART 打印单字符：

```asm
ldr     x9, =0xfeb50000
mov     x10, #65        /* 'A' */
str     x10, [x9]
```

用不同字符标记检查点（A/B/C/D/E/F），根据最后输出的字符定位挂死位置。注意：MMU 开后要改用高 VA 地址 `0xffff0000feb50000`。

### 验证 ATF 是否开着 MMU 移交

在 `mmu_init` 入口读 `SCTLR_EL1`：若 bit0=1 则 ATF 移交时 MMU 已开，必须先关再切页表。

### SMP timer 健康检测

`kernel/task/cpu.c` 中的 `cpu_smp_timer_test()` 每 500ms 采样一次各核 `local_ticks`，连续 3 轮均有增量则 PASS：

```
[smp-test] round=0 cpu1 local_ticks=53 (+50)  ← 正常
[smp-test] round=0 cpu1 local_ticks=17 (+0)   ← 卡死
```
