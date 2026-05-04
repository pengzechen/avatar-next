# RISC-V64 页表切换机制详解

## 概述

本文档详细说明 Avatar OS 在 RISC-V64 架构下的页表切换实现，包括调度器切换和异常处理切换两个关键路径。

---

## 1. 基础概念

### 1.1 SATP 寄存器

```
SATP (Supervisor Address Translation and Protection)
 63:60  MODE  - 分页模式（8 = Sv39）
 59:44  ASID  - 地址空间标识符（当前未使用）
 43:0   PPN   - 根页表物理页号
```

**切换页表 = 修改 SATP 寄存器 + 刷新 TLB**

```assembly
csrw satp, <new_value>  # 写入新的 satp
sfence.vma              # 刷新 TLB（可选：指定 ASID 和 VA）
```

### 1.2 页表层次

Avatar OS 使用 3 层页表：

```
内核页表（全局唯一）：
  - 物理地址：0x80205000
  - SATP 值：0x8000000000080205
  - 用途：内核任务、内核态执行

用户页表（每进程独立）：
  - 物理地址：动态分配（如 0x80829000）
  - SATP 值：动态计算
  - 用途：用户进程用户态执行
```

---

## 2. 调度器中的页表切换

### 2.1 代码实现

**位置**: `kernel/task/sched.c:117-136`

```c
void sched_schedule(void)
{
    uint64_t flags = arch_irq_save();
    task_t *prev = g_current_task;
    
    // ... 选择下一个任务 ...
    task_t *next = pick_next();
    
    if (next == prev) {
        prev->state = TASK_RUNNING;
        arch_irq_restore(flags);
        return;
    }
    
    next->state = TASK_RUNNING;
    g_current_task = next;
    
#if ARCH_RISCV64
    /* RISC-V：切换到任务的页表 */
    if (next->is_user_process && next->pgd != 0) {
        /* 切换到用户页表 */
        uint64_t pgd_phys = (uint64_t)next->pgd;
        uint64_t ppn = pgd_phys >> 12;
        uint64_t satp = (8ULL << 60) | ppn;
        __asm__ volatile("csrw satp, %0" : : "r"(satp));
        __asm__ volatile("sfence.vma");
    } else if (next->is_user_process == 0 && prev->is_user_process != 0) {
        /* 从用户任务切换到内核任务：恢复内核页表 */
        uint64_t kernel_ppn = 0x80205;
        uint64_t satp = (8ULL << 60) | kernel_ppn;
        __asm__ volatile("csrw satp, %0" : : "r"(satp));
        __asm__ volatile("sfence.vma");
    }
#endif
    
    /* 切换上下文（保存/恢复寄存器） */
    arch_task_switch(&prev->sp, next->sp, /* pgd */);
    
    arch_irq_restore(flags);
}
```

### 2.2 切换场景

| 从任务类型 | 到任务类型 | 页表操作 | SATP 值 |
|-----------|-----------|---------|---------|
| 用户任务 A | 用户任务 B | 切换到 B 的用户页表 | 0x8000000000<B.PPN> |
| 用户任务 | 内核任务（idle） | 切换到内核页表 | 0x8000000000080205 |
| 内核任务 | 用户任务 | 切换到用户页表 | 0x8000000000<PPN> |
| 内核任务 | 内核任务 | 不切换 | 保持内核页表 |

### 2.3 时序图

```
时间线：
T0: 用户任务 A 运行（satp = A.pgd）
    |
    v [定时器中断]
T1: 进入 exception.S（satp 仍 = A.pgd，当前实现不切换）
    |
    v [调用 sched_schedule()]
T2: 选择下一个任务 B
    |
    v [检测到 A != B]
T3: 切换页表（csrw satp = B.pgd）
    |
    v [sfence.vma]
T4: 切换上下文（arch_task_switch）
    |
    v [返回到 B 的执行点]
T5: 用户任务 B 运行（satp = B.pgd）
```

---

## 3. 异常处理中的页表策略

### 3.1 当前实现（Commit 15c8696）

**位置**: `boot/riscv64/exception.S:108-118`

```assembly
trap_vector:
    # ... 保存寄存器到内核栈 ...
    
    csrr  t0, sstatus
    sd    t0, 35*8(sp)
    
    # 当前实现：不切换页表！
    # 注释说明：
    #   1. 用户页表已完整复制内核映射（L1[0x100], L1[0x102]）
    #   2. 内核可在用户页表下安全运行
    #   3. syscall可同时访问内核和用户空间
    #   4. 避免页表切换的复杂性和性能开销
    
    # 仅恢复 GP（全局指针寄存器）
    la    t0, __global_pointer$
    mv    gp, t0
    
    # 调用 C 处理函数（仍在用户页表下）
    mv    a0, sp
    call  handle_exception
    
    # ... 返回用户态（仍在用户页表下）...
    sret
```

**关键点：**
- **不切换 SATP**：异常处理全程在用户页表下执行
- **依赖条件**：用户页表包含内核高半区映射（L1[0x100], L1[0x102]）
- **性能优势**：免去每次异常的 2 次页表切换（入口 + 出口）

### 3.2 问题与风险

**问题 1：PC-relative 寻址错误**（已发现）
- 症状：全局变量 `g_pmm` 被损坏
- 原因：在用户页表下，PC-relative 偏移计算可能出错
- 影响：所有全局指针变量都可能被损坏

**问题 2：安全隔离不足**
- 内核代码在用户页表下执行，理论上用户可以通过页表操作影响内核

**问题 3：难以扩展**
- 未来如果需要实现 ASID（地址空间隔离）、页表权限隔离等，当前设计无法支持

---

## 4. 标准实现方案（推荐）

### 4.1 入口切换页表

```assembly
trap_vector:
    # ... 保存寄存器 ...
    
    csrr  t0, sstatus
    sd    t0, 35*8(sp)
    
    # 检查 SPP：是否从 U 态陷入
    csrr  t1, sstatus
    andi  t1, t1, SSTATUS_SPP   # SPP=0 表示从 U 态
    bnez  t1, 9f                # 从 S 态陷入，跳过页表切换
    
    # 从 U 态陷入：切换到内核页表
    li    t0, 0x80205           # PPN: 内核 L1 页表 @ 0x80205000
    li    t1, 0x8000000000000000  # MODE=Sv39 (bit 63:60 = 8)
    or    t0, t0, t1
    csrw  satp, t0              # 写入 satp
    sfence.vma                  # 刷新 TLB
9:
    # 恢复 GP
    la    t0, __global_pointer$
    mv    gp, t0
    
    # 调用 C 处理函数（现在在内核页表下）
    mv    a0, sp
    call  handle_exception
    
    # ... 继续处理 ...
```

### 4.2 出口切换页表

```assembly
    # 恢复 sstatus
    ld    t0, 35*8(sp)
    csrw  sstatus, t0
    
    # 检查返回目标：是否返回 U 态
    andi  t1, t0, SSTATUS_SPP
    bnez  t1, 3f                # 返回 S 态，跳过
    
    # 返回 U 态：切换回用户页表
    # 从全局变量读取当前用户任务的 satp
    la    t0, g_current_user_satp
    ld    t1, 0(t0)
    beqz  t1, 3f                # satp=0，跳过（内核任务）
    
    csrw  satp, t1              # 切换到用户页表
    sfence.vma                  # 刷新 TLB
3:
    # 恢复通用寄存器
    ld    x1,   1*8(sp)
    # ... 恢复所有寄存器 ...
    ld    x31, 31*8(sp)
    ld    x2,   2*8(sp)         # 恢复 sp
    
    sret                        # 返回用户态
```

### 4.3 配套修改：维护 g_current_user_satp

**kernel/task/sched.c:**

```c
#if ARCH_RISCV64
/* 全局变量：保存当前用户任务的 satp 值，供 exception 返回时使用 */
uint64_t g_current_user_satp = 0;
#endif

void sched_schedule(void)
{
    // ... 任务切换逻辑 ...
    
#if ARCH_RISCV64
    if (next->is_user_process && next->pgd != 0) {
        uint64_t pgd_phys = (uint64_t)next->pgd;
        uint64_t ppn = pgd_phys >> 12;
        uint64_t satp = (8ULL << 60) | ppn;
        
        g_current_user_satp = satp;  // 保存，供 exception 返回时使用
        
        __asm__ volatile("csrw satp, %0" : : "r"(satp));
        __asm__ volatile("sfence.vma");
    } else {
        g_current_user_satp = 0;     // 内核任务，清零标记
        
        if (prev->is_user_process) {
            /* 从用户切换到内核：恢复内核页表 */
            uint64_t kernel_ppn = 0x80205;
            uint64_t satp = (8ULL << 60) | kernel_ppn;
            __asm__ volatile("csrw satp, %0" : : "r"(satp));
            __asm__ volatile("sfence.vma");
        }
    }
#endif
    
    // ... arch_task_switch ...
}
```

---

## 5. 页表切换开销分析

### 5.1 性能数据

| 操作 | 周期数（估算） | 说明 |
|-----|--------------|-----|
| `csrw satp` | ~10 | CSR 写入 |
| `sfence.vma` | ~50-100 | TLB 刷新（与 TLB 大小相关） |
| **单次切换总计** | ~60-110 | |
| **每次异常（入+出）** | ~120-220 | 2 次切换 |

### 5.2 实际影响

假设系统调用频率：
- 轻负载：1000 次/秒
- 重负载：100,000 次/秒

额外开销（按 200 周期/异常计算）：
- 轻负载：200k 周期/秒 ≈ 0.02% @ 1GHz CPU
- 重负载：20M 周期/秒 ≈ 2% @ 1GHz CPU

**结论：** 对于正确性和安全性，这个开销是可以接受的。

---

## 6. 对比：Linux 的实现

### 6.1 Linux RISC-V entry.S

**简化版本：**

```assembly
# arch/riscv/kernel/entry.S
SYM_CODE_START(handle_exception)
    # 保存寄存器到 pt_regs
    
    # 检查是否从用户态陷入
    csrr t0, sstatus
    andi t0, t0, SR_SPP
    bnez t0, _save_context  # 从内核态陷入
    
    # 从用户态陷入：切换到内核页表
    csrr t0, sscratch       # t0 = 内核 sp
    # Linux 在这里已经在内核页表中了，因为它在
    # 内核初始化时就设置了 SATP，并且所有内核代码
    # 运行在直接映射区域
    
    # 调用 C 异常处理
    la ra, ret_from_exception
    tail do_trap_unknown
    
SYM_CODE_START(ret_from_exception)
    # 检查返回用户态
    andi s0, s0, SR_SPP
    bnez s0, restore_all    # 返回内核态
    
    # 返回用户态：恢复用户寄存器并 sret
    # Linux 实际上不在这里切换 SATP，因为它的
    # 内核页表已经包含了所有必要的映射
    
restore_all:
    # 恢复寄存器
    sret
```

**Linux 的设计：**
- **全局内核页表**：包含所有物理内存的直接映射（Offset Mapping）
- **不频繁切换**：大部分情况下保持内核页表，只在进程切换时切换
- **TLB 优化**：使用 ASID 减少 TLB 刷新

### 6.2 Avatar OS vs Linux

| 特性 | Avatar OS (当前) | Avatar OS (推荐) | Linux |
|-----|------------------|------------------|-------|
| 异常入口切换 | 否 | 是 | 否（不需要） |
| 异常出口切换 | 否 | 是 | 否（不需要） |
| 调度器切换 | 是 | 是 | 是 |
| 内核页表映射 | 高半区 1GB | 高半区 1GB | 全部物理内存 |
| ASID 使用 | 否 | 未来 | 是 |

---

## 7. 实现检查清单

### 修改文件

- [ ] `boot/riscv64/exception.S`
  - [ ] 入口：检测 SPP，切换到内核页表
  - [ ] 出口：检测 SPP，切换回用户页表

- [ ] `kernel/task/sched.c`
  - [ ] 添加 `g_current_user_satp` 全局变量
  - [ ] 在 `sched_schedule()` 中维护 `g_current_user_satp`

- [ ] `kernel/syscall/syscall.c`
  - [ ] 删除 `g_pmm` hack 代码
  - [ ] （可选）实现 `copy_from_user/copy_to_user`

### 测试场景

- [ ] 用户进程启动（idle → user）
- [ ] 用户进程系统调用（user → kernel → user）
- [ ] 定时器中断（user → kernel → user）
- [ ] 任务切换（user A → user B）
- [ ] 用户进程退出（user → idle）
- [ ] 全局变量访问正确性验证

---

## 8. 总结

**当前状态（Commit 15c8696）：**
- ❌ 异常处理不切换页表（简化设计）
- ⚠️ g_pmm 被损坏，使用 hack 修复
- ✅ 调度器正确切换页表
- ✅ 用户页表包含内核映射

**推荐方案：**
- ✅ 异常入口切换到内核页表
- ✅ 异常出口切换回用户页表
- ✅ 调度器维护 `g_current_user_satp`
- ✅ 彻底解决 PC-relative 寻址问题

**权衡：**
- 性能损失：~2% @ 高负载
- 正确性增益：100%
- 可维护性：显著提升
- 安全性：符合操作系统设计原则

---

**最后更新**: 2026-05-05  
**作者**: Avatar OS Team  
**相关文档**: `RISCV64_PAGE_TABLE_STATUS.md`
