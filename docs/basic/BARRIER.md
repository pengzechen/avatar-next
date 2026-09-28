# 内存屏障（barrier）

> **一句话**：C 代码要屏障，只用 `include/barrier.h` 的接口。
> `.c` 文件里不许再出现 `dsb` / `dmb` / `isb` / `fence` / `mfence` 的内联汇编。

## 接口

`include/barrier.h` 按架构分发到 `include/<arch>/barrier_impl.h`（同 `cache.h` 的模式）。
全部是 `static inline` 函数，只有 `barrier_compiler` 是宏。

| 接口 | aarch64 | riscv64 | x86_64 | 语义 |
|---|---|---|---|---|
| `barrier_compiler()` | `"" ::: "memory"` | 同 | 同 | 只挡编译器重排，不产生指令 |
| `barrier_data()` | `dmb ish` | `fence rw, rw` | `mfence` | 数据屏障（读写都管） |
| `barrier_data_read()` | `dmb ishld` | `fence r, r` | `lfence` | 读屏障 |
| `barrier_data_write()` | `dmb ishst` | `fence w, w` | `sfence` | 写屏障 |
| `barrier_sync()` | `dsb sy` | `fence iorw, iorw` | `mfence` | **同步屏障**（见下） |
| `barrier_instr_full()` | `isb` | `fence iorw, iorw` | `mfence` | 指令同步（取指 / 系统寄存器写之后） |
| `barrier_acquire()` | `dmb ishld` | `fence r, r` | 仅编译器屏障 | 获取语义 |
| `barrier_release()` | `dmb ishst` | `fence w, w` | 仅编译器屏障 | 释放语义 |

Linux 风格别名：`mb() rmb() wmb() isb() smp_mb() smp_rmb() smp_wmb() smp_acquire() smp_release()`。

## `barrier_data()` 与 `barrier_sync()` 的区别（最容易用错的地方）

- `barrier_data()` = **dmb**：只保证**顺序**（前面的访存在后面的访存之前被观察到），
  不等待访存真正完成。
- `barrier_sync()` = **dsb**：**一直等到**之前的访存真正完成、对全系统可见。

对设备寄存器、页表项这类"写完必须确保已经到达"的场景，**必须**用 `barrier_sync()`。
典型三处：

1. 写 GIC 寄存器（GICD / GICR / ICC_*）之后；
2. TLB / EPT 失效（`tlbi` / `hfence` / `sfence.vma`）之后；
3. 页表项写完、切换 TTBR / SATP 之前。

**`dmb` 比 `dsb` 弱**，拿 `barrier_data()` 去替换这些位置的 `dsb` 会把原本正确的
同步削弱 —— 这正是统一屏障时最容易踩的坑，所以 API 里专门补了 `barrier_sync()`。
反过来，把 `dmb` 升级成 `barrier_sync()` 永远安全，只是更慢。

## 现状：调用点分布

| 位置 | 内容 |
|---|---|
| `include/mmio.h` | 20 处：设备读写封装内部（`barrier_data()` / `barrier_data_write()`） |
| `driver/eth/virtio_net.c` | 9 处 `wmb()` / `rmb()`：virtqueue 的 avail / used 环 |
| `kernel/task/cpu.c` | 5 处 `wmb()`：per-CPU 上线标志等 |
| `kernel/task/{mutex,sched}.c` | `barrier_compiler()` |
| `include/{aarch64,riscv64,x86_64}/{cache_impl,halt_arch}.h` | 架构原语内部 |
| `driver/irq/gicv3.c`、`kernel/mm/aarch64/stage2.c`、`kernel/mm/vm_user.c` | 原本是裸 `dsb` / `isb`，**已改为 `barrier_sync()` / `barrier_instr_full()`** |

顺带修掉的：`include/x86_64/cache_impl.h` 的 `clflushopt` 分支原来**缺 SFENCE**
（`CLFLUSHOPT` 与其它写不保序，Intel SDM 要求配 SFENCE），已补 `barrier_data_write()`。

`barrier_acquire()` / `barrier_release()` / `barrier_data_read()` 目前**没有调用者**：
留着是因为它们是完整的语义集合（x86 的 acquire/release 只需编译器屏障），
但别以为有人在用。

## 不在 API 范围内、也不该收编的东西

- **TLB / 指令缓存失效指令本身**：`tlbi`（aarch64）、`hfence.gvma` / `sfence.vma`
  （riscv64）、`ICACHE.IALL` / `SYNC.I`（SG2002）—— 它们是"动作"不是"屏障"。
  用屏障替换会**丢掉失效动作**。这些保留内联汇编，紧随其后的等待才用
  `barrier_sync()` / `barrier_instr_full()`（例：`kernel/mm/aarch64/stage2.c` 的
  `tlbi vmalls12e1is` + `barrier_sync()` + `barrier_instr_full()`）。
- **`.S` 汇编文件**：`boot/*/boot.S`、`kernel/mm/*/mmu.S`、`kernel/task/*/switch.S`、
  `kernel/vmm/aarch64/*.S` 等约 45 处 `dsb`/`isb`/`fence`。C 接口在那里不可用，
  保持原样；改动时对照上表的强度语义。
- **`cpuid`（x86）**：`driver/irq/lapic.c` 与 `include/x86_64/vmx.h` 用它取 CPU
  频率 / VMX 能力，序列化只是副作用，不是屏障，**不要**替换。

## 新代码检查清单

- [ ] 只需要顺序 → `barrier_data()` / 别名 `mb() rmb() wmb()`
- [ ] 需要"写完确实到达"（设备、页表、TLB 之后）→ `barrier_sync()`
- [ ] 写系统寄存器后要让后续指令看到 → `barrier_instr_full()`
- [ ] 只防编译器重排 → `barrier_compiler()`
- [ ] `.c` 里没有新增的 `dsb`/`dmb`/`isb`/`fence` 内联汇编
      （除非是 `tlbi` 那类"动作"指令，并在旁边注明为什么不用 API）
