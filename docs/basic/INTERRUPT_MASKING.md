# 中断屏蔽（C 代码统一接口）

> **一句话**：C 代码要关/开中断、要读中断状态，只用
> `include/<arch>/exception_impl.h` 里的 `arch_irq_*`。别的写法一律删掉。
> 汇编（`.S`）不受此约束。

策略层面（什么时候该关中断、EL0 任务的内核侧为什么关、谁负责开）见
[INTERRUPT_CONTROL_COMPARISON.md](INTERRUPT_CONTROL_COMPARISON.md) 与
[INTERRUPT_CONTEXT_SWITCH.md](INTERRUPT_CONTEXT_SWITCH.md)。本文只讲**接口**。

## 接口

实现在 `include/aarch64/exception_impl.h` / `include/riscv64/exception_impl.h` /
`include/x86_64/exception_impl.h`，由 `include/exception.h` 按架构分发暴露
（与 `barrier.h` / `cache.h` 同一种模式）。`kernel/task/switch.h` 顶部已经
include 了 `exception.h`，所以包含它的文件天然可用。

| 接口 | 语义 |
|---|---|
| `arch_irq_flags()` | 读当前中断状态字（不改动），返回值直接交给 `arch_irq_restore()` |
| `arch_irq_is_enabled()` | 当前是否**允许** IRQ（处于 irqsave 临界区时返回 0） |
| `arch_irq_save()` | 关中断并返回旧状态 |
| `arch_irq_restore(flags)` | 恢复到 `arch_irq_save()`/`arch_irq_flags()` 取到的状态 |
| `arch_irq_disable()` | 无条件关中断（不需要旧状态时用） |
| `arch_irq_enable()` | 无条件开中断（新任务首次运行等场景） |

### 各架构映射

| | aarch64 | riscv64 | x86_64 |
|---|---|---|---|
| 状态字 | `DAIF`（IRQ = bit 7） | `sstatus`（SIE = bit 1） | `RFLAGS`（IF = bit 9） |
| save | `mrs daif` + `msr daifset, #2` | `csrrci sstatus, 2` | `pushfq; popq; cli` |
| restore | `msr daif, %0` | `csrw sstatus, %0` | `pushq; popfq` |
| disable | `msr daifset, #2` | `csrci sstatus, 2` | `cli` |
| enable | `msr daifclr, #2` | `csrsi sstatus, 2` | `sti` |

注意 riscv64 的 restore 恢复的是**整份 sstatus**（与改动前一致），不是只写 SIE 位。

## 用法

```c
#include "exception.h"     /* 或者任何已经包含它的头（如 task/switch.h） */

/* 临界区：关中断 → 做事 → 还原成"进来之前的样子" */
uint64_t flags = arch_irq_save();
...
arch_irq_restore(flags);

/* 只在需要"当前是否可被中断"时用 */
if (!arch_irq_is_enabled())     /* 例如 sched 里的抢占判断 */
    return;

/* 确实不关心旧状态时才用无条件版本 */
arch_irq_disable();
```

**必须配对**：`arch_irq_save()` 拿到的值要在**同一条执行路径**上还给
`arch_irq_restore()`。不要用"全局变量/锁对象"暂存它（SMP 下会被别的 CPU 覆盖，
见 [SPINLOCK.md](SPINLOCK.md) 里 `irq_flags` 的那段历史）。

## 之前是什么样（为什么要统一）

同一件事曾有**四份并行实现**，加三处"读中断状态"的重复：

| 位置 | 状态 |
|---|---|
| `kernel/task/switch.h` 的 `arch_irq_*` | 唯一的正式 API，31 处使用 → **已迁到 exception_impl.h** |
| `include/<arch>/spin_lock_impl.h` 里的 `aarch64_irq_save` / `riscv_irq_save` / `x86_64_irq_save` | 三个私有副本，外部调用者 0 → **已删**，改调 `arch_irq_*` |
| `driver/irq/irq.h` 的 `enable_irqs/disable_irqs`、`driver/irq/gicv2.h` 的 `enable_interrupts/disable_interrupts/get_daif` | 调用者 0 的死代码，第四份 `msr daif*` → **已删** |
| `platforms/qemu/qemu_platform.c` 的 `qemu_panic()` | 按架构各写一遍（aarch64 还写成 `#0xF`，掩得比 `#2` 多） → **已改调 `arch_irq_disable()`** |
| `kernel/task/preempt.c`（三个架构各一份）+ `kernel/task/cpu.c` + `include/x86_64/vmx.h` 的状态读取 | 逐字节重复 → **已改调 `arch_irq_is_enabled()` / `arch_irq_flags()`** |

删除这些副本不只是整洁问题：`arch_irq_save/restore` 的语义很容易写错
（例如 `halt_arch.h` 里 riscv 曾用 `csrci mstatus, 0x8` —— MIE 是 M 模式的位，
S 模式内核该用 `sstatus.SIE`，而且从 S 模式写 `mstatus` 本身就会触发非法指令异常）。
只有一份实现才谈得上"改一次，处处正确"。

## 例外：汇编

`.S` 文件（`boot/*/boot.S`、`boot/*/exception.S`、`kernel/task/*/switch.S`、
`kernel/mm/*/mmu.S`、`kernel/vmm/aarch64/*.S`）里的 `msr daifset #2` /
`csrci sstatus,2` / `cli` 保持原样 —— C 函数用不了，汇编里写寄存器是唯一选择。
改动它们时请对照本表的语义，别把"保存旧值"写成"无条件关"。

## 新代码检查清单

- [ ] 关中断用 `arch_irq_save/restore`（或 `arch_irq_disable`），**没有**内联汇编
- [ ] `save` 的返回值只存在调用点的局部变量里
- [ ] "当前是否可中断"的判断用 `arch_irq_is_enabled()`，不要自己位运算状态字
- [ ] 需要"关中断 + 加锁"时用 `spin_lock_irqsave(&lock, &flags)`（见 SPINLOCK.md），
      不要手写 `arch_irq_save()` 再 `spin_lock()`
- [ ] 中断屏蔽期间不要做长耗时操作（打印、等 UART、睡眠）
