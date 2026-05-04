# RISC-V64 页表机制与 g_pmm 问题分析

## 1. 当前页表架构总结

### 1.1 Sv39 三级页表结构

RISC-V64 使用 Sv39 页表模式（39位虚拟地址空间）：

```
虚拟地址结构（39位）：
  [38:30] VPN[2] - L1 索引（9位，512项）
  [29:21] VPN[1] - L2 索引（9位，512项）
  [20:12] VPN[0] - L3 索引（9位，512项）
  [11:0]  页内偏移（12位，4KB页）

SATP寄存器（64位）：
  [63:60] MODE = 8 (Sv39)
  [59:44] ASID = 0（地址空间标识符，未使用）
  [43:0]  PPN（物理页号，页表基址）
```

### 1.2 内存布局

**内核虚拟内存（高半区）：**
```
KERNEL_VMA          = 0xffffffc000000000
MMIO 高别名          = 0xffffffc000000000 - 0xffffffc03fffffff (1GB)
RAM 高别名（内核区） = 0xffffffc080000000 - 0xffffffc0bfffffff (1GB)
  - 内核代码/数据    = 0xffffffc080200000 - 0xffffffc08031f000
  - g_pmm 变量       = 0xffffffc080230d80 (在 RAM 高别名范围内)
```

**用户虚拟内存（低地址）：**
```
用户代码段 (ET_DYN)  = 0x00010000 - 0x0018ffff
用户堆               = 0x00190000 起
用户mmap区           = 0x50000000 起
用户栈               = 0x6ff00000 - 0x70000000 (1MB, 256页)
```

### 1.3 页表映射策略

**内核页表（PPN = 0x80205）：**
- L1[0x100] (索引256): 映射 MMIO 高别名，1GB 叶子页表项，PTE=0xcf (V|R|W|X)
- L1[0x102] (索引258): 映射 RAM 高别名，1GB 叶子页表项，PTE=0x200000cf (V|R|W|X, U=0)

**用户页表（每个进程独立，例如 PPN = 0x80829）：**
- L1[0x000] - L1[0x001]: 用户低地址空间（代码、堆、栈），按需分配L2/L3页表
- L1[0x100]: **复制自内核** L1[0x100]，映射 MMIO 高别名
- L1[0x102]: **复制自内核** L1[0x102]，映射 RAM 高别名（包含内核代码/数据）

**关键设计：** 用户页表完整复制了内核的高半区映射（L1[0x100], L1[0x102]），使得在用户页表下也能访问内核地址空间。

---

## 2. 页表切换机制

### 2.1 调度器中的页表切换 (sched.c)

调度器在任务切换时切换页表：

```c
// kernel/task/sched.c:117-136
if (next->is_user_process && next->pgd != 0) {
    // 切换到用户页表
    uint64_t pgd_phys = (uint64_t)next->pgd;
    uint64_t ppn = pgd_phys >> 12;
    uint64_t satp = (8ULL << 60) | ppn;  /* MODE=Sv39 */
    csrw satp, satp;
    sfence.vma;
} else if (next->is_user_process == 0 && prev->is_user_process != 0) {
    // 从用户任务切换到内核任务：恢复内核页表
    uint64_t kernel_ppn = 0x80205;
    uint64_t satp = (8ULL << 60) | kernel_ppn;
    csrw satp, satp;
    sfence.vma;
}
```

**切换时机：**
- 用户任务 → 用户任务：直接切换到目标用户页表
- 用户任务 → 内核任务：切换回内核页表
- 内核任务 → 用户任务：切换到用户页表
- 内核任务 → 内核任务：不切换（保持内核页表）

### 2.2 异常处理中的页表策略 (exception.S)

**当前实现（Commit 15c8696）：**

```assembly
# boot/riscv64/exception.S:108-118
# 简化方案：不在exception入口切换页表！
# 原因：
#   1. 用户页表已完整复制内核映射（L1[0x100], L1[0x102]）
#   2. 内核可在用户页表下安全运行
#   3. syscall可同时访问内核和用户空间
#   4. 避免页表切换的复杂性和性能开销

# 仅恢复 GP 寄存器（全局指针）
la    t0, __global_pointer$
mv    gp, t0
```

**关键设计决策：**
- **不切换页表**：内核代码在用户页表下执行
- **仅恢复 GP**：用户态 GP 指向用户数据，内核需要正确的 GP 访问小数据段

**优点：**
- 减少页表切换开销（每次异常免去 2 次 `csrw satp + sfence.vma`）
- 简化实现逻辑
- 系统调用可直接访问用户空间地址（无需 copy_from_user）

**问题：** 这就是 g_pmm 损坏的根本原因（见下节）。

---

## 3. g_pmm 损坏问题分析

### 3.1 问题现象

**症状：**
```c
// kernel/syscall/syscall.c:507-522
extern pmm_t *g_pmm;  // 全局指针变量，应指向 &pmm

// 进入 syscall 后，g_pmm 被损坏：
g_pmm == 0x18fd44  // 变成了用户空间地址（syscall的a0参数）！
&g_pmm == 0xffffffc080230d80  // g_pmm 变量的地址是正确的内核地址
```

**触发条件：**
1. 用户进程执行 `ecall` 进入 S-mode
2. 异常处理器 **不切换页表**，satp 仍指向用户页表
3. 内核代码访问全局变量 `g_pmm`

### 3.2 根本原因：PC-relative 寻址

**RISC-V 全局变量访问使用 PC-relative 寻址：**

```assembly
# 汇编伪代码：访问 g_pmm
# 编译器生成（使用 -mcmodel=medany）：
auipc   a0, %pcrel_hi(g_pmm)      # a0 = PC + offset_hi
ld      a0, %pcrel_lo(1b)(a0)     # a0 = *(a0 + offset_lo)
```

**问题分析：**

1. **在内核页表下执行：**
   ```
   PC = 0xffffffc080209e36  （内核高地址）
   offset = 相对偏移量
   计算：a0 = PC + offset = 0xffffffc080230d80  ✓ 正确！
   读取：g_pmm = *(0xffffffc080230d80) = 0xffffffc08028c7d8  ✓ 正确！
   ```

2. **在用户页表下执行：**
   ```
   PC = 0xffffffc080209e36  （仍是内核高地址，因为 sepc 指向内核代码）
   offset = 相对偏移量（编译时计算）
   
   但是！用户页表的 L1[0x102] 虽然复制了内核映射：
   - 虚拟地址 0xffffffc080230d80 → 物理地址 0x80230d80 ✓ 映射正确
   
   问题在于：PC-relative 计算假设 PC 附近的偏移量是固定的，
   但在不同页表上下文中，如果发生了 TLB miss 或缓存不一致，
   可能导致读取到错误的值。
   
   更可能的原因：编译器优化 + 寄存器污染
   ```

**实际原因（推测）：**

g_pmm 的损坏值 `0x18fd44` 恰好是第一个 syscall 的 `a0` 参数（`SET_TID_ADDR` 的参数）。这表明：

1. **寄存器污染**：用户态的寄存器值（a0）污染了内核的全局指针
2. **编译器优化**：编译器可能将 `g_pmm` 缓存在某个寄存器中，而该寄存器未被正确保存/恢复
3. **GP 寻址问题**：虽然恢复了 GP，但 GP-relative 访问仍可能出错

### 3.3 当前 Hack 方案

```c
// kernel/syscall/syscall.c:507-522
#ifdef ARCH_RISCV64
extern pmm_t *g_pmm;
extern pmm_t pmm;

if ((uintptr_t)g_pmm < 0xffffffc000000000 || g_pmm == NULL) {
    /* 通过 &g_pmm 重新读取 g_pmm 的值（强制从内存加载） */
    volatile pmm_t **g_pmm_addr = &g_pmm;
    pmm_t *g_pmm_reread = *g_pmm_addr;
    
    KLOG_ERROR("[syscall] FATAL: g_pmm=%p, re-read via &g_pmm=%p\n", 
               g_pmm, g_pmm_reread);
    
    g_pmm = &pmm;  /* 强制恢复正确值 */
}
#endif
```

**工作原理：**
1. 检查 `g_pmm` 是否被损坏（值不在内核地址空间）
2. 使用 `volatile` 强制从内存重新加载
3. 如果仍然错误，强制设置为正确值 `&pmm`

**缺点：**
- 每次 syscall 都要检查，性能损失
- 治标不治本，根本问题未解决
- 只针对 `g_pmm` 一个变量，其他全局变量可能也有同样问题

---

## 4. 正确解决方案

### 4.1 方案 A：异常入口切换到内核页表（推荐）

**实现：**

```assembly
# boot/riscv64/exception.S 修改
csrr  t0, sstatus
sd    t0, 35*8(sp)

# 检查 SPP：如果从 U 态陷入，切换到内核页表
csrr  t1, sstatus
andi  t1, t1, SSTATUS_SPP   # SPP = 0 表示从 U 态
bnez  t1, 9f                # 从 S 态陷入，跳过

# 从 U 态陷入：切换到内核页表
li    t0, 0x80205           # PPN: 内核 L1 页表
li    t1, 0x8000000000000000  # MODE=Sv39
or    t0, t0, t1
csrw  satp, t0
sfence.vma
9:

# 恢复 GP
la    t0, __global_pointer$
mv    gp, t0

# ... 调用 C 处理函数 ...

# 返回时：如果返回 U 态，切换回用户页表
ld    t0, 35*8(sp)
csrw  sstatus, t0

andi  t1, t0, SSTATUS_SPP
bnez  t1, 3f

# 返回 U 态：切换回用户页表
la    t0, g_current_user_satp  # 需要新增全局变量保存当前用户 satp
ld    t1, 0(t0)
beqz  t1, 3f
csrw  satp, t1
sfence.vma
3:

# 恢复寄存器并返回
```

**需要新增：**
```c
// kernel/task/sched.c
#if ARCH_RISCV64
uint64_t g_current_user_satp = 0;  // 保存当前用户任务的 satp
#endif

// 在 sched_schedule() 中更新：
if (next->is_user_process && next->pgd != 0) {
    uint64_t pgd_phys = (uint64_t)next->pgd;
    uint64_t ppn = pgd_phys >> 12;
    g_current_user_satp = (8ULL << 60) | ppn;
    // ... csrw satp ...
} else {
    g_current_user_satp = 0;
}
```

**优点：**
- 彻底解决 PC-relative 寻址问题
- 符合标准内核设计模式（Linux/xv6 都这样做）
- 所有全局变量访问都正确

**缺点：**
- 每次异常多 2 次页表切换（性能开销约 100-200 周期）
- 系统调用无法直接访问用户空间，需要实现 copy_from_user/copy_to_user

### 4.2 方案 B：禁用 GP-relative 优化

**实现：**
```makefile
# Makefile
CFLAGS += -fno-small-data -fno-common -fno-pie -fno-pic
```

强制所有全局变量使用绝对地址访问而非 GP-relative。

**优点：**
- 无需修改异常处理流程
- 简单直接

**缺点：**
- 性能下降（所有全局访问都变慢）
- 无法从根本上解决 PC-relative 问题
- 可能影响其他优化

### 4.3 方案 C：copy_from_user/copy_to_user（配合方案 A）

如果采用方案 A，需要实现用户空间访问机制：

```c
// 临时切换到用户页表进行访问
int copy_from_user(void *dst, const void *user_src, size_t len)
{
    uint64_t old_satp;
    __asm__ volatile("csrr %0, satp" : "=r"(old_satp));
    
    // 切换到用户页表
    extern uint64_t g_current_user_satp;
    if (g_current_user_satp != 0) {
        __asm__ volatile("csrw satp, %0" : : "r"(g_current_user_satp));
        __asm__ volatile("sfence.vma");
    }
    
    // 复制数据
    memcpy(dst, user_src, len);
    
    // 切换回内核页表
    __asm__ volatile("csrw satp, %0" : : "r"(old_satp));
    __asm__ volatile("sfence.vma");
    
    return 0;
}
```

---

## 5. 性能对比

| 方案 | 每次异常开销 | 全局访问开销 | syscall 用户访问 |
|------|-------------|-------------|-----------------|
| 当前（不切换） | 0 | 正常 + Hack检查 | 直接访问（快） |
| 方案 A（切换） | ~200 周期 | 正常 | copy_from_user（慢） |
| 方案 B（禁用优化） | 0 | 稍慢 | 直接访问（快） |

**推荐：** 方案 A 是最正确的实现，符合操作系统安全和隔离原则。

---

## 6. 实现计划

### 阶段 1：验证问题
- [x] 确认 g_pmm 损坏模式
- [x] 验证用户页表包含内核映射
- [x] 确认 PC-relative 寻址机制

### 阶段 2：实现方案 A
- [ ] 添加 `g_current_user_satp` 全局变量
- [ ] 修改 `exception.S` 入口：U→S 时切换到内核页表
- [ ] 修改 `exception.S` 出口：S→U 时切换回用户页表
- [ ] 更新 `sched_schedule()` 维护 `g_current_user_satp`

### 阶段 3：实现 copy_from_user
- [ ] 实现 `copy_from_user()` 函数
- [ ] 实现 `copy_to_user()` 函数
- [ ] 修改所有访问用户指针的 syscall（read/write/etc）

### 阶段 4：清理 Hack
- [ ] 删除 `syscall_handler()` 中的 g_pmm 检查
- [ ] 清理调试日志

---

## 7. 参考资料

- **RISC-V 特权架构手册**: Volume II Chapter 4 (Sv39 Page-Based Virtual Memory)
- **Linux RISC-V 实现**: `arch/riscv/kernel/entry.S` - 标准的页表切换实现
- **xv6-riscv**: `kernel/trap.c` - 教学操作系统的异常处理
- **Avatar OS 文档**: 
  - `docs/INTERRUPT_CONTROL_COMPARISON.md` - 中断控制对比
  - `docs/USER_PROCESS_STATUS.md` - 用户进程状态
  - 本文档

---

**最后更新**: 2026-05-05  
**状态**: g_pmm 问题已识别，Hack 方案可临时工作，正确方案待实现  
**Commit**: 15c8696 ("feat: riscv 能进入busybox了。但是目前是hack的。g_pmm 的值会被改，导致问题。")
