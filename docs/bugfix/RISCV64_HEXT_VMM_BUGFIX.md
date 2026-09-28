# RISC-V H-extension VMM 三线程并发 Bug 修复记录

## 目标

让以下三个线程在 RISC-V H-extension 模式下稳定并发运行：

| 线程 | 名称 | 特权级 | 行为 |
|------|------|--------|------|
| Thread 1 | `rv_host` | HS-mode | 内核监控循环，每 20 tick 打印一次 |
| Thread 2 | `vcpu0` | VS-mode | WFI 循环，每 20 次 WFI 发一次 GUEST_ECALL_PRINT |
| Thread 3 | `u_loop` | U-mode | 打印 Hello、获取 PID，正常退出 |

---

## Bug #1 — hstatus.SPV 污染 host 路径（根因）

### 现象

- `vcpu0` 和 `rv_host` 正常运行
- `u_loop` 进入用户态后立即触发 Inst PF：`pc=0x10000 va=0x10000`
- 软件 walk satp 页表完全正确（L1[0]→L0[0]→leaf[16]，U=1 X=1 V=1）
- hstatus.SPV=0、SPP=0、satp=用户 PGD，诊断数据全部看起来正常

### 根本原因

RISC-V H-extension 规范规定：**VS-mode → HS-mode 陷入时，硬件自动将 `hstatus.SPV` 置 1，且不会自动清零。**

```
guest (VS-mode) 执行 WFI/ecall
    → 陷入 HS-mode（hstatus.SPV 硬件置 1）
    → hext_trap_vector 保存 vCPU 状态，恢复 host 寄存器，ret 返回 C
    → vmm_arch_exit_handler 处理后 task_yield()
    → 调度器切换到 u_loop
    → arch_task_switch / arch_switch_to_user → sret

此时 hstatus.SPV 仍为 1！
sret 时 CPU 决策：
  - SPP=0（返回 U-mode？）+ SPV=1 → 进入 VU-mode（虚拟用户模式）
  - VU-mode 使用 vsatp（默认=0）翻译 VA 0x10000 → 未映射 → Inst PF
```

软件 walk 用的是 `satp`（用户 PGD，正确），而 CPU 实际用的是 `vsatp=0`，所以表面上"satp 正确但 PF"。

### 修复

**位置**：`kernel/vmm/riscv64/hext_vcpu.S`，`hext_trap_vector` 步骤 3b

在保存完 vCPU 状态（step 3）、恢复 host 寄存器（step 4）之前，**立即清零 `hstatus.SPV+SPVP`**：

```asm
/* 3b. 清 hstatus.SPV(bit7)+SPVP(bit8) —— 核心修复 */
csrr    t0, CSR_HSTATUS
li      t1, ~0x180
and     t0, t0, t1
csrw    CSR_HSTATUS, t0
```

这样整个 host 后续路径（sched_check_and_yield → sret）都在普通 HS-mode 语义下运行，不再误进入 VS/VU-mode。

---

## Bug #2 — sched.c 用户进程 prev->pgd 被覆写

### 现象

- 用户进程 `u_loop` 首次调度时页表正确
- 多次调度后 PF（偶发性）

### 根本原因

`arch_task_switch` 第三参数 `prev_pgd_ptr` 非 NULL 时，会把当前 `csrr satp` 值写入 `*prev_pgd_ptr`（即 `prev->pgd`）。

用户进程**首次被调度时**，`arch_switch_to_user` 还未执行（satp 仍指向内核页表）。若在这个时间窗口被抢占：

```
内核 satp → arch_task_switch 把内核 satp 写入 u_loop->pgd
→ 下次调度 u_loop 时，next_pgd_for_switch = u_loop->pgd = 内核 PGD 物理地址
→ satp 切到内核 PGD
→ sret 进入 U-mode，VA 0x10000 不在内核 PGD 中 → Inst PF
```

### 修复

**位置**：`kernel/task/sched.c`，`sched_schedule`

对用户进程，`prev_pgd_save` 传 `NULL`，跳过 satp 回写：

```c
uint64_t **prev_pgd_save = prev->is_user_process ? NULL : &prev->pgd;
arch_task_switch(&prev->sp, switch_sp, prev_pgd_save, next_pgd_for_switch);
```

用户进程的 `pgd` 在 `vm_create_user_process` 时固定分配，不需要动态保存。

---

## 防御性修复（纵深防御）

即使 Bug #1 已在 `hext_trap_vector` 一次性修复，以下位置也额外清除 `hstatus.SPV+SPVP`，防止其他路径的边角 case：

### boot/riscv64/exception.S — trap_vector U-mode 出口

```asm
/* 返回 U 态：清除 hstatus.SPV(bit7) + SPVP(bit8) */
csrr  t1, hstatus
li    t2, ~0x180
and   t1, t1, t2
csrw  hstatus, t1
```

**触发场景**：vcpu0 被时钟中断抢占（中断发生时 hstatus.SPV 可能已置 1），进入普通 `trap_vector`，调度到 u_loop 后从此出口 sret。

### kernel/task/riscv64/switch.S — arch_switch_to_user / arch_fork_resume_user

```asm
csrr    t0, hstatus
li      t1, ~0x180
and     t0, t0, t1
csrw    hstatus, t0
```

**触发场景**：用户进程首次进入（trampoline → arch_switch_to_user）时的最后防线。

---

## 改善项：异常诊断增强

### boot/riscv64/exception.c

- PF 打印从只有 `pc/va` 扩充为 `hstatus(SPV/SPVP) + sstatus(SPP) + satp(PPN)`
- 新增 Guest PF code（20=Inst-G，21=Load-G，23=Store-G）支持

```c
KLOG_ERROR("%s PF: pc=0x%lx va=0x%lx sstatus=0x%lx(SPP=%u) hstatus=0x%lx(SPV=%u SPVP=%u) satp=0x%lx(PPN=0x%lx)\n", ...);
```

### kernel/task/task.c — arch_user_entry_debug

新增三级页表 walk 打印，确认用户 VA 0x10000 的映射链路正确。

---

## 验证结果

```
[rv_host] tick=1200 (HS-mode)         ← Thread 1: HS-mode 持续运行
[HEXT] GUEST_ECALL_PRINT: iter=1140   ← Thread 2: VS-mode 持续运行
[rv-user] Hello from RISC-V user!     ← Thread 3: U-mode 正常执行
[rv-user] PID: 3
[rv-user] Exiting...
[syscall] process 'u_loop' exiting    ← 正常退出
```

- PF count = 0
- 三线程在 RISC-V H-extension 下稳定并发

---

## 改动文件汇总

| 文件 | 类型 | 说明 |
|------|------|------|
| `kernel/vmm/riscv64/hext_vcpu.S` | 新增 | H-ext VM 入口/出口汇编；**核心修复：hext_trap_vector 步骤 3b 清 hstatus.SPV** |
| `kernel/vmm/riscv64/hext_run.c` | 新增 | VMM 架构钩子（enter_guest / exit_handler） |
| `kernel/task/sched.c` | 修改 | RISC-V 用户进程 prev_pgd_save=NULL，避免 pgd 被内核 satp 覆写 |
| `boot/riscv64/exception.S` | 修改 | trap_vector U-mode 出口清 hstatus.SPV（防御） |
| `boot/riscv64/exception.c` | 修改 | PF 诊断增强：打印 hstatus/sstatus/satp；支持 Guest PF code |
| `kernel/task/riscv64/switch.S` | 修改 | arch_switch_to_user / arch_fork_resume_user 清 hstatus.SPV（防御） |
| `kernel/task/task.c` | 修改 | arch_user_entry_debug 三级页表 walk 诊断 |
| `kernel/main.c` | 修改 | RISC-V 三线程测试入口 |
| `include/vmm.h` | 修改 | RISC-V vcpu_t 布局、VCPU_RV_* 偏移宏 |

---

## 关键知识点

### RISC-V H-extension sret 决策树

```
sret 执行时：
  sstatus.SPP == 1  → 目标是 S-mode（HS-mode 或 VS-mode）
    hstatus.SPV == 1 → 进入 VS-mode
    hstatus.SPV == 0 → 进入 HS-mode
  sstatus.SPP == 0  → 目标是 U-mode
    hstatus.SPV == 1 → 进入 VU-mode（使用 vsatp！）
    hstatus.SPV == 0 → 进入 U-mode（使用 satp）
```

### hstatus.SPV 生命周期

| 时机 | 值 | 操作者 |
|------|----|--------|
| VS-mode → HS-mode 陷入 | → **1** | 硬件自动 |
| hext_trap_vector step 3b | → **0** | **软件主动清（本次修复）** |
| hext_enter_guest step 8 | → **1** | 软件设置（进入 guest 前） |
| HS-mode sret（返回 VS-mode） | 不变 | — |

**不清零的后果**：一旦有 vcpu 运行过，整个系统 hstatus.SPV 常驻 1，任何后续 sret(SPP=0) 都会误入 VU-mode。
