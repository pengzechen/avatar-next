# 中断上下文切换问题与解决方案

## 问题概述

在实现抢占式调度时，发现**在中断处理程序中直接进行任务切换会导致系统失效**。具体表现为：

- AArch64：任务只切换一次，之后调度器停止工作
- RISC-V：同上
- x86_64：同上

**根本原因**：在异常/中断上下文中直接调用 `arch_task_switch()` 破坏了异常返回路径。

---

## 背景知识

### 异常返回机制

每个架构都有专用的异常返回指令，用于恢复完整的处理器状态：

| 架构 | 异常返回指令 | 恢复的状态 |
|------|------------|-----------|
| **AArch64** | `eret` | ELR_EL1 (PC) + SPSR_EL1 (PState) |
| **RISC-V** | `sret` | sepc (PC) + sstatus (PState) |
| **x86_64** | `iretq` | RIP + RFLAGS + CS + SS |

这些指令**不仅仅是跳转**，它们：
- 恢复被中断时的程序计数器
- 恢复处理器状态（中断标志、特权级等）
- 执行架构特定的状态转换

### 普通返回指令

`ret` 指令只是：
- 从栈（或 x30/ra 寄存器）读取返回地址
- 跳转到该地址
- **不恢复任何处理器状态**

---

## 问题分析

### 错误的实现流程

```
Timer IRQ 发生
  ↓
CPU 自动保存异常状态到专用寄存器
  ↓
跳转到异常向量表
  ↓
exception.S: SAVE_REGS (保存通用寄存器到栈)
  ↓
调用 C handler: handle_irq_exception()
  ↓
  ├─ irq_ack()
  ├─ timer_handler()
  │   ├─ g_system_ticks++
  │   └─ g_tick_cb()  → sched_tick()
  │       └─ sched_schedule()  ← 直接调用！
  │           └─ arch_task_switch(&prev->sp, next->sp)
  │               └─ ret  ← 跳转到新任务
  │
  └─ irq_eoi()
```

**问题点**：

1. `arch_task_switch` 使用 `ret` 返回，不是 `eret`/`sret`/`iretq`
2. 新任务开始运行时，CPU 的异常寄存器（ELR_EL1/sepc/RIP）仍然指向**被中断的任务**
3. 新任务在错误的异常上下文中运行
4. 下一次中断时，异常状态被覆盖，**被中断任务的返回地址丢失**
5. 调度器状态混乱，后续切换失效

### 后果示例（AArch64）

```c
初始状态：
  task_a 运行中，ELR_EL1 = task_a 的某条指令地址

IRQ 发生：
  CPU 保存：ELR_EL1 ← task_a 的下一条指令
  CPU 保存：SPSR_EL1 ← task_a 的 PState

错误切换：
  arch_task_switch 跳转到 task_b
  task_b 开始运行

  但此时：
    ELR_EL1 仍然指向 task_a ❌
    SPSR_EL1 仍然是 task_a 的状态 ❌

下一次 IRQ：
  CPU 覆盖 ELR_EL1/SPSR_EL1
  task_a 的返回地址永久丢失 ❌

最终结果：
  调度器状态混乱，无法正确切换任务
```

---

## 解决方案：延迟调度

### 核心思想

**不要在 C handler 中执行任务切换**，而是：
1. 在 C handler 中只设置"需要重调度"的标志
2. 在异常返回路径（汇编代码）中检查这个标志
3. 如果设置了，执行实际的上下文切换
4. 然后正常返回到异常处理流程

### 正确的实现流程

```
Timer IRQ 发生
  ↓
CPU 自动保存异常状态
  ↓
exception.S: SAVE_REGS
  ↓
调用 C handler: handle_irq_exception()
  ↓
  ├─ irq_ack()
  ├─ timer_handler()
  │   ├─ g_system_ticks++
  │   └─ g_tick_cb()  → sched_tick()
  │       └─ g_need_resched = true  ← 只设置标志！
  │
  └─ irq_eoi()
  ↓
返回到 exception.S
  ↓
bl sched_check_and_yield  ← 检查标志
  ↓
  如果 g_need_resched == true:
    g_need_resched = false
    sched_schedule()
    └─ arch_task_switch(&prev->sp, next->sp)
        └─ ret  ← 在异常上下文中切换，OK
  ↓
继续执行 RESTORE_REGS
  ↓
eret / sret / iretq  ← 正确恢复异常状态！
```

### 为什么这样可行？

1. **异常上下文完整**：`eret`/`sret`/`iretq` 能够正确恢复异常状态
2. **切换时机安全**：在 C handler 返回后、异常返回前进行切换
3. **状态一致性**：被中断任务的异常状态不会被破坏
4. **通用性**：适用于所有架构

---

## 代码实现

### 1. 调度器修改（sched.c）

> 注：下方为最初版本示意。当前实现中 `need_resched` 已是 **per-CPU** 字段
> （`cpu_current()->need_resched`），不再是全局 `g_need_resched`，以支持 SMP。
> 逻辑不变：tick 只置标志，切换在异常返回路径执行。

```c
/* Timer tick 回调：只设置本核的重调度标志 */
void
sched_tick(void)
{
    cpu_current()->need_resched = true;
}

/* 由异常返回路径调用：检查并执行调度 */
bool
sched_check_and_yield(void)
{
    cpu_t *c = cpu_current();
    if (c->need_resched) {
        c->need_resched = false;
        sched_schedule();
        return true;
    }
    return false;
}
```

> **与中断开关策略的关系**（详见 `INTERRUPT_CONTROL_COMPARISON.md`）：
> 这套“延迟调度”机制要求被抢占的实体在被中断时处于**开中断**状态，
> 中断才能进入并最终在异常返回路径触发切换。因此：
> - **内核线程**全程开中断（`task_trampoline` / `task_idle_loop` 入口
>   `arch_irq_enable()`），可被 timer 抢占；
> - **EL0 任务**在用户态开中断（`SPSR=0x340`）；AArch64 下其陷入内核侧的
>   syscall/异常处理在保存完整 trap frame 后也会开 IRQ，但 timer 只置
>   `need_resched`，实际任务切换延迟到 syscall 返回边界，避免任意 C 代码点
>   交错执行 syscall 路径。

### 2. AArch64 异常处理（exception.S）

```asm
.macro HANDLE_IRQ
.p2align 7
    SAVE_REGS
    mov     x0, sp
    bl      handle_irq_exception

    /* 检查是否需要重调度 */
    bl      sched_check_and_yield

    b       .Lexception_return
.endm

.extern sched_check_and_yield
```

### 3. RISC-V 异常处理（exception.S）

```asm
/* 调用 C 处理函数 */
mv    a0, sp
call  handle_exception

/* 检查是否需要重调度 */
call  sched_check_and_yield

/* 恢复 CSR */
ld    t0, 32*8(sp)
csrw  sepc, t0
...

sret

.extern sched_check_and_yield
```

### 4. x86_64 异常处理（exception.S）

```asm
/* 传递参数并调用 C handler */
movq  %rsp, %rdi
call  handle_exception

/* 检查是否需要重调度 */
call  sched_check_and_yield

/* 恢复通用寄存器 */
popq  %r15
...

iretq

.extern sched_check_and_yield
```

---

## 架构对比

### 异常状态寄存器

| 架构 | PC 寄存器 | PState 寄存器 | 返回指令 |
|------|----------|--------------|---------|
| AArch64 | ELR_EL1 | SPSR_EL1 | `eret` |
| RISC-V | sepc | sstatus | `sret` |
| x86_64 | RIP (栈上) | RFLAGS (栈上) | `iretq` |

### 任务切换指令

| 架构 | 切换指令 | 返回指令 | 说明 |
|------|---------|---------|------|
| AArch64 | `arch_task_switch` | `ret` | 只跳转到 x30 |
| RISC-V | `arch_task_switch` | `ret` | 只跳转到 ra |
| x86_64 | `arch_task_switch` | `ret` | 只跳转到栈顶地址 |

**关键**：`arch_task_switch` 的 `ret` 不能替代异常返回指令！

---

## 类比理解

这个问题的本质类似于：

**错误做法**：
```
在厨房做菜做到一半（异常处理）
突然直接跳到另一个房间继续做（ret 跳转）
但厨房的火还开着（异常状态未恢复）
```

**正确做法**：
```
先把菜做完，清理厨房，关火（完成异常处理）
然后再去下一个房间（在安全点切换）
```

---

## 验证方法

### 1. 添加调试日志

```c
bool
sched_check_and_yield(void)
{
    if (g_need_resched) {
        KLOG_DEBUG("[sched] rescheduling on exception return\n");
        g_need_resched = false;
        sched_schedule();
        return true;
    }
    return false;
}
```

### 2. 观察任务切换

修复后的预期输出：
```
[DEBUG] [sched] switch: prev='task_a' -> next='task_b'
[INFO] [task_b] count=50 ticks=2
[DEBUG] [sched] switch: prev='task_b' -> next='task_c'
[INFO] [task_c] count=50 ticks=3
[DEBUG] [sched] switch: prev='task_c' -> next='task_a'
[INFO] [task_a] count=50 ticks=4
...
```

任务应该能够持续在 task_a、task_b、task_c 之间循环切换。

### 3. 测试所有架构

```bash
# AArch64
make ARCH=aarch64

# RISC-V
make ARCH=riscv64

# x86_64
make ARCH=x86_64
```

所有架构都应该能够正常进行抢占式调度。

---

## 总结

### 关键要点

1. **异常返回必须用专用指令**：`eret`/`sret`/`iretq`
2. **普通 `ret` 不能恢复异常状态**
3. **不要在 C handler 中直接切换任务**
4. **使用延迟调度：设置标志 → 异常返回前检查并切换**

### 适用场景

这个解决方案适用于：
- 抢占式调度（timer tick 触发）
- 任何需要在异常处理中切换任务的场景
- 所有需要异常/中断处理的架构

### 参考资料

- ARM Architecture Reference Manual: `eret` 指令
- RISC-V Privileged Architecture: `sret` 指令
- Intel SDM: `iretq` 指令
- Linux OOPS: `TIF_NEED_RESCHED` 机制（类似设计）

---

**文档版本**: 1.0
**创建日期**: 2026-05-02
**作者**: Avatar OS Team
**适用架构**: AArch64, RISC-V, x86_64
