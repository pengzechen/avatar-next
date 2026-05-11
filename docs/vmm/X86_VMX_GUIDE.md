# x86_64 VMX VMM 实现指南

## 概述

Avatar OS 在 x86_64 上实现了基于 Intel VT-x（VMX）的轻量级 Hypervisor。  
设计目标与 AArch64 EL2 VMM 对齐：**三线程并发模型**。

```
Thread 1: x86_host_loop    — VMX root mode（内核循环，每 20 tick 打印）
Thread 2: vcpu0 (vmm_run)  — VMX non-root mode（HLT 循环 + VMCALL）
Thread 3: u_loop           — Ring 3 用户进程（syscall 测试）
```

---

## 文件结构

```
include/x86_64/vmx.h            — VMX 常量、VMCS 字段编码、辅助类型
include/vmm.h                   — vcpu_t 布局（x86_64 / AArch64 / stub）、架构钩子声明
kernel/vmm/vmm.c                — 架构无关 vmm_run_vcpu 主循环 + vm_create/vcpu_task_create
kernel/vmm/x86_64/vmx.c        — VMX 初始化、VMCS 管理、架构钩子实现
kernel/vmm/x86_64/vmx_run.S    — VMLAUNCH / VMRESUME 汇编（guest GPR 保存/恢复）
kernel/vmm/aarch64/el2_run.c   — AArch64 架构钩子实现（对比参考）
apps/x86_64/guest_test.S       — VMX non-root guest 测试程序
apps/x86_64/user_test.S        — Ring 3 用户态测试程序（syscall 测试）
```

---

## 架构钩子接口

`vmm.h` 声明四个架构钩子，由 `vmm_run_vcpu`（`vmm.c`）调用：

```c
void vmm_arch_restore_guest_ctx(vcpu_t *vcpu);
int  vmm_arch_enter_guest(vcpu_t *vcpu);    /* 1=成功, 0=失败 */
int  vmm_arch_exit_handler(vcpu_t *vcpu);
void vmm_arch_save_guest_ctx(vcpu_t *vcpu);
```

| 架构 | 实现文件 | `enter_guest` 指令 |
|---|---|---|
| AArch64 | `aarch64/el2_run.c` | `eret` |
| x86_64  | `x86_64/vmx.c`      | `vmlaunch` / `vmresume` |

`vmm_run_vcpu` 主循环（`vmm.c`）：

```
restore_guest_ctx
while (1):
    enter_guest  →  VMLAUNCH/VMRESUME
    exit_handler →  EL2_RESUME / EL2_VMEXIT / EL2_VMABORT / EL2_EXIT
```

---

## VMX 初始化流程

### 1. 硬件检测（`vmx_check_support`）

```c
CPUID.1.ECX[5] = 1           // VMX 支持位
MSR_IA32_FEATURE_CONTROL[2,0] = 0b11  // 固件允许 VMX
```

> **注意**：QEMU TCG 不模拟 VMX。必须使用 `-enable-kvm -cpu host`。

### 2. CR 寄存器调整（`vmx_global_init`）

```
CR0 满足 MSR_IA32_VMX_CR0_FIXED0/1 要求
CR4 满足 MSR_IA32_VMX_CR4_FIXED0/1 要求，并置位 CR4.VMXE(bit 13)
```

### 3. VMXON

```c
// VMXON 区域写入版本号
*(uint32_t *)g_vmxon_region = g_vmx_basic.revision;

// 物理地址转换（内核运行在 0xffff800000000000 高半区）
uint64_t vmxon_pa = virt_to_phys(g_vmxon_region);
vmxon [vmxon_pa];   // m64 操作数必须是物理地址
```

### 4. VMCS 初始化（`vcpu_vmcs_init`）

```
写入 revision_id → vmclear → vmptrld → vmwrite 所有字段
→ 第二次 vmclear（flush：强制写回 CPU 缓存，重置 launch state = "clear"）
```

第二次 `vmclear` 是关键：若省略，后续 `vmptrld + vmlaunch` 会以
"VMRESUME with non-launched VMCS"（VMX Instruction Error 5）失败。

---

## VMCS 字段设置摘要

### 控制字段

| 字段 | 值 | 说明 |
|---|---|---|
| `PIN_CONTROLS` | `PIN_NMI \| PIN_VIRT_NMI` | 协商后实际值 |
| `CPU_EXEC_CTRL0` | `CPU_HLT` | HLT 触发 VM exit |
| `EXI_CONTROLS` | `EXI_HOST_64 \| EXI_LOAD_EFER \| EXI_SAVE_EFER` | Host 64-bit |
| `ENT_CONTROLS` | `ENT_GUEST_64 \| ENT_LOAD_EFER` | Guest IA-32e |
| `EXC_BITMAP` | `0` | 不捕获 guest 异常 |

所有控制字段通过 capability MSR 协商（`allowed-1 & allowed-0`）。

### Guest 状态

| 字段 | 值 |
|---|---|
| `GUEST_CR3` | `vmx_read_cr3()`（与 host 共享，不使用 EPT）|
| `GUEST_RIP` | `entry`（`x86_guest_test_entry`）|
| `GUEST_RSP` | `g_guest_stack[vcpu_id] + 4096` |
| `GUEST_RFLAGS` | `0x2`（IF=0，Reserved=1）|
| `GUEST_AR_CS` | `0xa09b`（64-bit code, L=1, DPL=0）|

### Host 状态

| 字段 | 值 |
|---|---|
| `HOST_CR3` | `vmx_read_cr3()` |
| `HOST_RIP` | `vmx_return`（`vmx_run.S` 中定义）|
| `HOST_RSP` | 每次 `vmlaunch/vmresume` 前在汇编中动态写入 |
| `HOST_SEL_CS` | `0x10`（GDT[2], DPL=0, 64-bit）|
| `HOST_SEL_TR` | `0x30`（GDT[6+7], 16B TSS）|

---

## 汇编：vmx_run.S

```
vmx_enter_guest(vcpu_t *rdi):
  push host callee-saved {rbp,rbx,r12-r15}
  push rdi (vcpu*)                    ← HOST_RSP 指向此处
  VMWRITE HOST_RSP = rsp
  VMWRITE HOST_RIP = vmx_return
  r11d = vcpu->launched
  加载 guest GPR 从 vcpu->regs
  test r11d → jnz .Ldo_resume
  vmlaunch / vmresume

vmx_return:                           ← VM exit 后 CPU 跳至此
  xchg r15, (rsp)                    → r15=vcpu*, (rsp)=guest r15
  保存所有 guest GPR 到 vcpu->regs
  恢复 host callee-saved
  return 1 (成功)
```

---

## VM Exit 处理（`vmx_exit_handler`）

| Exit Reason | 处理 | 返回 |
|---|---|---|
| `VMX_REASON_HLT` (12) | RIP += 1，`task_yield()` | `EL2_RESUME` |
| `VMX_REASON_VMCALL` (18) | 分派 hypercall，RIP += 3 | `EL2_RESUME` / `EL2_VMEXIT` |
| `VMX_REASON_CPUID` (10) | RIP += 2，GPR 不变 | `EL2_RESUME` |
| `VMX_REASON_EXC_NMI` (0) | 打印 vector + RIP | `EL2_EXIT` |
| 其他 | 打印诊断信息 | `EL2_EXIT` |

### Hypercall 编号

```c
#define VMX_HYPERCALL_DONE   0   // guest 正常结束 → EL2_VMEXIT
#define VMX_HYPERCALL_PRINT  1   // 打印迭代计数（rsi=iter），task_yield → EL2_RESUME
```

---

## Guest 程序：guest_test.S

`apps/x86_64/guest_test.S`，符号 `x86_guest_test_entry`：

```asm
x86_guest_test_entry:
    xorl %ebx, %ebx           // 计数器 = 0
.Lloop:
    hlt                        // VM exit → VMM yield → resume
    incl %ebx
    ebx % 20 == 0?
      → vmcall(VMX_HYPERCALL_PRINT, iter=ebx)
    jmp .Lloop
```

运行时节奏（QEMU KVM 实测）：每 20 次 HLT 约对应 1 个 host tick（10ms）。

---

## 用户程序：user_test.S

`apps/x86_64/user_test.S`，符号 `user_test_program`（Ring 3）：

使用 **Linux x86_64 原生 syscall 号**（由 `x86_translate_syscall` 映射）：

| 操作 | Linux x86_64 nr | 调用约定 |
|---|---|---|
| `write` | 1 | `rdi=fd(1), rsi=buf, rdx=count` |
| `getpid` | 39 | — |
| `sched_yield` | 24 | — |
| `exit` | 60 | `rdi=code` |

> **坑**：旧版本使用自定义号（`SYS_WRITE=20`），被 `x86_translate_syscall`
> 映射为 `writev` 而非 `write`，导致无输出。

---

## 物理地址要求

`VMXON`、`VMCLEAR`、`VMPTRLD` 的 `m64` 操作数是**物理地址**。

内核虚拟地址 → 物理地址：

```c
#include "mm_vm.h"
uint64_t pa = virt_to_phys(ptr);  // pa = (uint64_t)ptr - KERNEL_VMA
```

`KERNEL_VMA = 0xffff800000000000`，内核加载在 `0xffff800000200000`。

---

## QEMU 启动参数

```makefile
QEMU_FLAGS := -machine q35 -enable-kvm -cpu host -m 2G -nographic -kernel $(KERNEL_BIN)
```

- `-enable-kvm`：启用 KVM 硬件虚拟化（必须，TCG 不支持 VMX）
- `-cpu host`：透传宿主机 CPU，暴露真实 VMX capability bits

---

## 预期运行输出

```
[INFO] Thread 1 [VMX root]:  id=1
[INFO] [VMX] VMX enabled and locked by firmware
[INFO] [VMX] VMXON success (revision=0x11e57ed0)
[INFO] [VMX] vmx_vm_init: 1 vCPU(s) ready
[INFO] [VMX] VMCS initialized for vcpu0, entry=0xffff8000002...
[INFO] Thread 2 [VMX guest]: id=2 entry=0xffff8000002...
[INFO] Thread 3 [user]:      id=3 entry=0xffff8000002...
[INFO] [vmm] vcpu0 task started
[INFO] [VMM] Starting vcpu0
[user_test] Starting in Ring 3
[user_test] My PID: 3
[user_test] #                        ← Ring 3 每 yield 一次
[INFO] [x86_host] tick=20 (VMX root) ← Thread 1 每 20 tick
[INFO] [VMX] VMCALL_PRINT: iter=20 (vcpu0)  ← Thread 2 每 20 HLT
[user_test] #
...
```

---

## 已知限制

- **单核**：没有 VMCS per-CPU 隔离，多核需要每 CPU 各自 VMXON + VMCS
- **无 EPT**：Guest 与 Host 共享 CR3，不支持 guest 物理内存隔离
- **无嵌套分页**：Guest 异常直接 `EL2_EXIT`，不转发给 guest IDT
- **无 VPID**：每次 VM entry/exit 均隐式 TLB flush
