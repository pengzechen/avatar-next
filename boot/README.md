# boot/ — 架构引导与异常处理

## 概述

`boot/` 模块负责内核的最早期初始化和异常/中断分发。它完成从裸机上电（或 bootloader 移交）到跳入 C 入口 `kernel_main()` 之间的全部工作，并在整个内核运行期间持续提供异常向量与中断分发服务。

## 目录结构

```
boot/
├── common/
│   ├── boot.h              # 跨架构公共声明（kernel_main, platform_init, run_all_tests）
│   └── platform_ops.h      # 平台抽象接口（platform_panic, platform_shutdown）
├── aarch64/
│   ├── boot.S              # BSP/AP 启动入口、EL2 VHE 初始化、MMU 跳板
│   ├── exception.S          # 异常向量表（VBAR_EL1）、寄存器保存/恢复宏
│   ├── exception.c          # 同步异常、IRQ、系统调用分发
│   └── link.ld              # 链接脚本（高半区 VA、物理别名符号）
├── riscv64/
│   ├── boot.S              # S-mode 启动、FPU 使能、MMU 跳板、BSS 清零
│   ├── exception.S          # trap_vector 入口、sscratch 模式检测、FP 保存/恢复
│   ├── exception.c          # scause 分发、ecall 处理、页错误诊断
│   └── link.ld              # 双地址布局（低 .text.boot + 高 VMA 内核）
└── x86_64/
    ├── boot.S              # Multiboot1 头、32→64 位切换、内嵌页表
    ├── exception.S          # ISR 存根（IDT 0-31 + LAPIC）、isr_common_stub
    ├── exception.c          # IDT 初始化、SYSCALL MSR 配置、异常分发
    ├── syscall_wrapper.S    # SYSCALL/SYSRET 快速路径（per-CPU scratch）
    ├── tss.c               # TSS 初始化、RSP0 更新（Ring 3→0 栈切换）
    ├── tss.h               # TSS 函数声明
    └── link.ld              # Multiboot1 布局
```

## 实现的功能

### 1. 启动流程（boot.S）

每个架构的 `_start` 完成以下步骤：

| 步骤 | AArch64 | RISC-V 64 | x86_64 |
|------|---------|-----------|--------|
| 特权级设置 | EL2 VHE 初始化（HCR_EL2、CPTR、GICv3） | S-mode（OpenSBI 已设置） | 32→64 位模式切换 |
| FPU 使能 | CPTR_EL2.TFP 清除 | sstatus.FS = Initial | — |
| 栈设置 | BSS 中 32KB 栈 | 16KB boot 栈 → 64KB 内核栈 | BSS 中 16KB 栈 |
| BSS 清零 | 8 字节循环（物理别名） | `clear_bss` 子程序 | Multiboot loader 保证 |
| 页表建立 | `vm_init()` | `mmu_init()` 内部建立 | boot.S 内嵌静态页表 |
| MMU 启用 | `mmu_init(TTBR0, TTBR1)` | `mmu_init()` | boot.S 中 `cr0 \| PG` |
| VA 跳板 | `ldr x0, =1f; br x0` | `la + add + jr` | 恒等映射消除需求 |
| 异常向量 | `msr vbar_el1` | `WRITE_STVEC` | `lidt` |
| 进入 C | `blr kernel_main` | `jr kernel_main` | `call kernel_main` |

AArch64 额外支持 **多核启动**：`_secondary_start` 由 PSCI CPU_ON 进入，复用 BSP 页表，使用 `secondary_boot_stacks` 独立栈，最终调用 `cpu_secondary_bootstrap(cpu_id)`。

### 2. 异常/中断处理（exception.S + exception.c）

#### 向量入口（汇编层）

- **AArch64**：16 条向量（EL1t/EL1h/EL0_64/EL0_32 × Sync/IRQ/FIQ/SError），`SAVE_REGS`/`RESTORE_REGS` 宏保存 `trap_frame_t`
- **RISC-V**：`trap_vector` 单入口，`sscratch=0` 约定区分内核/用户态来源，保存 32 GP + 4 CSR + 32 FP + fcsr（552 字节）
- **x86_64**：`ISR_NOERR`/`ISR_ERR` 宏生成 32 个 CPU 异常存根 + LAPIC 向量，统一经 `isr_common_stub` 构建 `trap_frame_t`

#### C 分发层

| 功能 | AArch64 | RISC-V | x86_64 |
|------|---------|--------|--------|
| 同步异常 | `handle_sync_exception`（EL1）<br>`handle_el0_sync_exception`（EL0） | `handle_exception` 中 `!INTERRUPT` 分支 | `handle_exception` 中 `vec < 32` |
| 系统调用 | EC=0x15 → `syscall_handler()` | CAUSE_USER_ECALL → `syscall_handler()` | SYSCALL/SYSRET 快速路径（`syscall_wrapper.S`） |
| 硬件中断 | `handle_irq_exception` → GIC IAR/EOI | scause 高位 → `interrupt_handlers[]` | `irq_handlers[vec]` 回调 |
| 页错误诊断 | ESR EC/DFSC 解析 | 命名常量 + 完整寄存器 dump | CR2 + error code 位解析 |
| 中断注册 | `irq_install(vector, handler)` | `irq_install(cause, handler)` | `irq_install(vector, handler)` |

### 3. 系统调用快速路径（x86_64）

`syscall_wrapper.S` 实现 `SYSCALL`/`SYSRET` 指令路径：
- `swapgs` 切换到内核 GS（per-CPU `cpu_t*`）
- 用户 RSP 暂存到 `gs:CPU_SCRATCH_RSP`（per-CPU，SMP 安全）
- 构建完整 `trap_frame_t` 后调用 `syscall_handler()`
- 恢复寄存器后 `sysretq` 返回用户态

### 4. TSS 管理（x86_64）

`tss.c` 初始化 Task State Segment：
- 设置 RSP0（Ring 3→0 特权级切换时的内核栈）
- 在 GDT 中安装 TSS 描述符并加载 TR
- `x86_tss_set_rsp0()` 供任务切换时更新每任务内核栈

### 5. 链接脚本（link.ld）

各架构提供链接脚本，定义：
- 内核虚拟地址基址（AArch64 `0xffff0000_00000000`、RISC-V `0xffffffc0_00000000`）
- `__kernel_start` / `__kernel_end`（PMM 保留区间）
- `__bss_start` / `__bss_end`（BSS 清零范围）
- 物理地址别名符号（`*_phys`，供 MMU 开启前使用）

### 6. 平台抽象（common/）

`platform_ops.h` 声明平台接口，由 `platforms/` 下各平台实现：
- `platform_init()` — 初始化 UART 等硬件
- `platform_panic()` — 致命错误停机
- `platform_shutdown()` — 关机/断电

## 依赖的头文件

| 头文件 | 用途 |
|--------|------|
| `types.h` | 基础类型（`uint64_t`, `bool` 等） |
| `klog.h` | 内核日志输出 |
| `string.h` | `memset`（TSS 初始化） |
| `exception.h` | `trap_frame_t`、IDT 常量（各架构各自的） |
| `aarch64/sysreg.h` | `READ_ESR_EL1`、`READ_FAR_EL1` 等系统寄存器读写宏 |
| `riscv64/sysreg.h` | `CSR_READ`、`CSR_SET`、`WRITE_STVEC` 等 CSR 操作宏 |
| `x86_64/io.h` | `outb`（屏蔽 8259A PIC） |
| `irq/irq.h` | `irq_ack()`、`irq_eoi()`、`gic_write_dir()`（GIC 操作） |
| `irq/lapic.h` | `lapic_init()`（LAPIC 初始化） |
| `task/task.h` | `task_t`、`task_current()` |
| `task/sched.h` | `sched_dequeue()`、`sched_tick()` |
| `task/cpu.h` | `cpu_t`、`cpu_current()` |
| `syscall/syscall.h` | `syscall_handler()` |
| `mm_vm.h` | `mm_vm_get_paddr()` |
| `platform_ops.h` | `platform_panic()`、`platform_shutdown()` |

## 调用的外部函数

### 启动路径调用

| 函数 | 定义位置 | 用途 |
|------|----------|------|
| `vm_init()` | kernel/mm/ | 建立初始页表映射 |
| `vm_get_boot_pgtable()` | kernel/mm/ | 获取恒等映射页表（TTBR0） |
| `vm_get_kernel_pgtable()` | kernel/mm/ | 获取内核高半区页表（TTBR1） |
| `mmu_init()` | kernel/mm/ | 启用 MMU |
| `kernel_main()` | kernel/main.c | C 入口 |
| `cpu_secondary_bootstrap()` | kernel/task/cpu.c | 次级核 C 入口（AArch64 SMP） |

### 异常/中断路径调用

| 函数 | 定义位置 | 用途 |
|------|----------|------|
| `handle_exception()` | boot/\*/exception.c | 汇编→C 异常分发 |
| `syscall_handler()` | kernel/syscall/ | 系统调用处理 |
| `sched_check_and_yield_from_trap()` | kernel/task/sched.c | 异常返回前检查抢占 |
| `sched_check_and_yield()` | kernel/task/sched.c | AArch64/x86_64 抢占检查 |
| `irq_ack()` / `irq_eoi()` | driver/irq/ | GIC 中断确认/结束 |
| `lapic_init()` | driver/irq/ | LAPIC 初始化 |
| `platform_panic()` / `platform_shutdown()` | platforms/ | 平台停机 |

## 导出的符号

### 汇编导出（供其他模块使用）

| 符号 | 架构 | 用途 |
|------|------|------|
| `_start` | 全部 | 内核入口点 |
| `_secondary_start` | AArch64 | AP 核启动入口 |
| `exception_vector_base` | AArch64 | VBAR_EL1 异常向量表 |
| `trap_vector` | RISC-V | stvec 陷阱入口 |
| `isr_stub_table` | x86_64 | IDT 存根地址数组 |
| `syscall_entry` | x86_64 | SYSCALL 指令入口 |
| `stack_top` / `stack_bottom` | 全部 | BSP 启动栈边界 |
| `secondary_boot_stacks` | AArch64 | AP 核启动栈数组 |

### C 导出

| 函数 | 架构 | 用途 |
|------|------|------|
| `exception_init()` | RISC-V / x86_64 | 初始化异常向量 |
| `irq_install()` | 全部 | 注册中断回调 |
| `idt_set_gate()` | x86_64 | 设置单个 IDT 门描述符 |
| `x86_tss_init()` | x86_64 | 初始化 TSS |
| `x86_tss_set_rsp0()` | x86_64 | 更新 TSS.RSP0（任务切换） |
| `riscv_kernel_interrupt_enable/disable()` | RISC-V | 内核态中断开关 |

### 链接脚本导出

| 符号 | 用途 |
|------|------|
| `__kernel_start` / `__kernel_end` | PMM 内核保留区间 |
| `__bss_start` / `__bss_end` | BSS 段边界 |
| `*_phys` 系列 | MMU 开启前使用的物理地址别名 |
