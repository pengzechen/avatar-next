# Avatar OS 中断控制机制对比：AArch64 vs RISC-V

**文档版本**: 1.1  
**日期**: 2026-06-18  
**目的**: 详细对比 AArch64 和 RISC-V 架构在 Avatar OS 内核中的中断控制策略

---

## 一、核心设计原则

### 关键约定

中断策略按 **“EL1（S-mode）里运行的是谁”** 区分，而不是简单的“内核态一律关中断”：

| 运行实体 | 所在 EL | 中断状态 | 说明 |
|----------|---------|----------|------|
| **EL0 任务（用户进程）的用户侧** | EL0 / U-mode | **开** | 用户代码可被 timer 抢占 |
| **EL0 任务陷入内核侧**（系统调用 / 缺页 / 异常处理） | EL1 / S-mode | **关** | 进入异常时硬件自动关，处理全程保持关 |
| **内核线程（独立内核任务，如 net-poll / idle）** | EL1 / S-mode | **开** | 全程在 EL1，需可被 timer 抢占，否则会饿死其他任务 |

**一句话**：用户进程“代表它跑内核代码”的那段（syscall/异常）关中断；而**独立的内核线程开中断**。二者都在 EL1，区别在于“EL1 里跑的是某个 EL0 任务的内核侧，还是一个独立内核线程”。

> 历史说明：早期文档（v1.0）写作“内核态（EL1）始终关闭”。这只描述了“EL0 任务陷入内核侧”的情形，**遗漏了独立内核线程需要开中断**这一类。v1.1 起按上表的实体区分重新表述。

**关中断的理由（针对 EL0 任务的内核侧 / 临界区）**：
1. **寄存器保护**：内核代码使用临时寄存器（t0-t6/x9-x15），中断打断会破坏这些值
2. **栈安全**：中断嵌套可能导致栈溢出
3. **原子性**：内核操作（如链表插入、页表切换）需要原子完成
4. **可预测性**：执行路径确定，便于调试和性能分析

**内核线程仍可被抢占的原因**：内核线程长期存在、可能长时间运行，若全程关中断会独占 CPU。它通过异常返回路径上的 `sched_check_and_yield` 被 timer 抢占；其自身的临界区用 `arch_irq_save/restore` 显式保护。

### 两类“首次进入”的分野（AArch64）

新任务的栈帧是伪造的，首次被调度时 `arch_task_switch` 的 `ret` 落到一个 trampoline。落到哪个、是否开中断，正是这套策略的体现：

| Trampoline | 服务对象 | 终点 | 开中断方式 |
|------------|----------|------|------------|
| `task_trampoline`（C，task.c） | **内核线程** | `entry()` 在 EL1 运行 | 入口处显式 `arch_irq_enable()`（**必需**） |
| `task_trampoline_user`（asm，switch.S） | **EL0 任务** | `eret` 下到 EL0 | **不显式开**，由 `arch_switch_to_user` 的 `SPSR=0x340` 经 `eret` 恢复 |
| `arch_fork_resume_user`（asm，switch.S） | **EL0 任务**（fork 子进程） | `eret` 下到 EL0 | **不显式开**，由 frame 中保存的用户态 `spsr` 经 `eret` 恢复 |

**为什么 `task_trampoline` 必须显式开中断**：新任务首次运行不经过 `sched_schedule()` 末尾的 `arch_irq_restore()`——前驱在 `arch_irq_save()` 里替它关了中断，却没有任何人替它开。内核线程若不在此显式开，将全程关中断、永远无法被 timer 抢占。

**为什么 EL0 任务的 trampoline 不显式开中断**：它运行在 EL1 内核侧（启动 EL0 任务的准备阶段），按约定此时应关中断。中断只应在 `eret` 跨入 EL0 的**同一瞬间**由硬件根据 `SPSR` 打开。若在 `eret` 之前手动 `daifclr`，会在仍处于 EL1 时留下一个 IRQ 窗口——既违反约定，又可能让这段半成品的“进 EL0 路径”被抢占。

---

## 二、AArch64 实现（参考标准）

### 2.1 中断控制寄存器

```
DAIF 寄存器（Debug, Asynchronous, IRQ, FIQ 屏蔽位）:
  [9] D - Debug 异常屏蔽
  [8] A - SError（异步中止）屏蔽  
  [7] I - IRQ 屏蔽
  [6] F - FIQ 屏蔽

SPSR_EL1（保存的处理器状态寄存器）:
  [9:6] DAIF - 异常返回时恢复的屏蔽位
  [3:0] M[3:0] - 异常等级和栈选择
    0x0 = EL0t（用户态，SP_EL0）
    0x5 = EL1h（内核态，SP_EL1）
```

### 2.2 关键代码路径

#### exception_init() - 异常初始化
```c
// boot/aarch64/exception.c
void exception_init(void) {
    extern void vectors(void);
    WRITE_VBAR_EL1((uint64_t)vectors);
    
    // 注意：没有使能 DAIF.I。中断由各路径按需开启：
    //  - 内核线程：task_trampoline / task_idle_loop 入口处 arch_irq_enable()
    //  - EL0 任务：arch_switch_to_user 的 SPSR=0x340 经 eret 打开
    KLOG_INFO("AArch64 exception init: vbar_el1=0x%lx\n", 
              (uint64_t)vectors);
}
```

#### task_trampoline() - 内核线程首次运行（EL1，需开中断）
```c
// kernel/task/task.c
void task_trampoline(void) {
    /* 内核线程全程在 EL1 运行，需开中断以可被 timer 抢占。
     * 首次运行不经过 sched_schedule 末尾的 arch_irq_restore，
     * 因此必须在此显式开中断（前驱已 arch_irq_save 关中断）。 */
    arch_irq_enable();

    task_t *cur = task_current();
    cur->entry(cur->arg);   // 在 EL1 运行，可被抢占
    task_exit();
}
```

#### task_trampoline_user() - EL0 任务首次运行（不在此开中断）
```asm
// kernel/task/aarch64/switch.S
.global task_trampoline_user
task_trampoline_user:
    /* 不在此开中断：仍处于 EL1 内核侧，按约定关中断。
     * 中断由 arch_switch_to_user 的 SPSR=0x340 经 eret 原子打开，
     * 避免在 eret 之前于 EL1 留下 IRQ 窗口。 */
    mov     x0, x19          // x0 = user_entry
    mov     x1, x20          // x1 = user_sp
    mov     x2, sp           // x2 = kernel_sp
    b       arch_switch_to_user
```

#### arch_switch_to_user() - 进入用户态
```asm
// kernel/task/aarch64/switch.S
.global arch_switch_to_user
arch_switch_to_user:
    /* ── 设置用户栈指针（SP_EL0）─────────────────────────── */
    msr     sp_el0, x1

    /* ── 设置异常返回地址到用户入口点───────────────────── */
    msr     elr_el1, x0

    /* ── 设置 SPSR 到 EL0t（用户态，IRQ使能）──────────────── */
    mov     x9, #0x340           // EL0t, DAIF.I=0 (IRQ未屏蔽)
    msr     spsr_el1, x9

    /* ── 跳转到用户态─────────────────────────────────────── */
    eret                         // 硬件自动恢复 SPSR -> DAIF（此刻才开中断）
```

**SPSR_EL1 = 0x340 解析**:
- `[9:6] = 0011` : D=0, A=0, I=0, F=1 → IRQ 使能
- `[3:0] = 0x0` : EL0t（用户态）

#### 异常入口 - vectors
```asm
// boot/aarch64/exception.S
vectors:
    // ... 异常向量表 ...

.Lsave_context:
    /* 硬件自动：SPSR_EL1 <- DAIF | M[3:0]
                 ELR_EL1  <- PC
                 DAIF.I   <- 1 (进入 EL1 时自动屏蔽 IRQ) */
    
    // 保存 x0-x30, sp, elr, spsr
    SAVE_REGS
    // 调用 C 处理函数
    bl      handle_exception
    // 恢复寄存器
    RESTORE_REGS
    eret                   // 硬件自动恢复 SPSR -> DAIF
```

**硬件行为**：
- **进入异常时**：`DAIF.I` 自动置 1（关闭 IRQ）
- **eret 返回时**：`SPSR_EL1[9:6]` → `DAIF[9:6]`（恢复中断状态）

---

## 三、RISC-V 实现（需对齐 AArch64）

### 3.1 中断控制寄存器

```
sstatus 寄存器:
  [8] SPP  - 进入 S-mode 前的特权级（0=U, 1=S）
  [5] SPIE - 进入异常前的 SIE 值
  [1] SIE  - S-mode 中断全局开关（0=关, 1=开）

sret 指令行为:
  1. PC <- sepc
  2. 特权级 <- SPP (0→U-mode, 1→S-mode)
  3. SIE <- SPIE（恢复中断状态）
  4. SPIE <- 1
  5. SPP <- 0
```

### 3.2 关键代码路径

#### exception_init() - 异常初始化（修复后）
```c
// boot/riscv64/exception.c
void exception_init(void) {
    extern void trap_vector(void);
    WRITE_STVEC((uint64_t)trap_vector);

    /*
     * 关键修复：不在此处使能 sstatus.SIE！
     * 内核态（S-mode）始终保持 SIE=0，防止定时器中断
     * 在内核代码中随机触发，破坏 t0/t1 等临时寄存器。
     * 定时器中断只在用户态（U-mode）生效：sret 通过 
     * SPIE→SIE 自动使能。
     */

    CSR_SET(sstatus, 1UL << 18);   // 仅使能 SUM（用户内存访问）

    KLOG_INFO("RISC-V exception init: stvec=0x%lx, SUM enabled "
              "(SIE kept 0 in kernel)\n", (uint64_t)trap_vector);
}
```

**对比 AArch64**: 与 AArch64 不调用 `msr daifclr, #2` 的逻辑一致。

#### arch_switch_to_user() - 进入用户态（修复后）
```asm
// kernel/task/riscv64/switch.S
.global arch_switch_to_user
arch_switch_to_user:
    // ... 切换页表、调试日志 ...

    /* 保存内核栈到 sscratch，供下一次用户陷入使用 */
    csrw    sscratch, a2

    /* 设置返回地址到用户入口 */
    csrw    sepc, a0

    /* 设置 sstatus：SPP=0 (返回 U 模式)，SPIE=1，SIE=0
     * 对齐 AArch64 设计：内核态 SIE=0，sret 通过 SPIE→SIE 
     * 在用户态使能中断。不能在 sret 前设 SIE=1，否则
     * 定时器可在 S 态窗口触发，破坏 t0/t1。 */
    csrr    t0, sstatus
    li      t1, ~0x102          /* 清 SPP (bit 8) 和 SIE (bit 1) */
    and     t0, t0, t1
    ori     t0, t0, 0x20        /* 仅设 SPIE (bit 5)，sret 后 SIE=SPIE=1 */
    csrw    sstatus, t0

    /* 用户栈 */
    mv      sp, a1

    /* 同步 I-cache */
    fence.i

    sret                         /* 硬件自动：SIE <- SPIE = 1 */
```

**旧代码错误**:
```asm
// ❌ 错误：同时设置 SIE=1 和 SPIE=1
ori     t0, t0, 0x22        /* set SPIE (bit 5) + SIE (bit 1) */
```
这会导致 `csrw sstatus, t0` 到 `sret` 之间的窗口期内，内核态 SIE=1，定时器中断可触发！

#### 异常入口 - trap_vector（修复后）
```asm
// boot/riscv64/exception.S
trap_vector:
    /*
     * 使用 sscratch=0 约定判断来源模式（与 Linux RISC-V 内核一致）：
     *   sscratch=0   : 从 S 态（内核）陷入
     *   sscratch≠0   : 从 U 态（用户）陷入，值为内核栈指针
     *
     * 关键修复：在保存 x5(t0)/x6(t1) 之前就完成判断和栈切换，
     * 避免旧代码 csrr/andi 污染 t0 的问题。
     */
    csrrw  t0, sscratch, t0
    bnez   t0, .Lfrom_user

.Lfrom_kernel:
    /* S → S: t0=0（旧 sscratch），sscratch=原始 t0 */
    csrrw  t0, sscratch, x0      /* t0=原始t0, sscratch=0 */
    addi   sp, sp, -TRAP_FRAME_SIZE
    sd     x5,  5*8(sp)          /* 原始 t0（正确！） */
    sd     x6,  6*8(sp)          /* 原始 t1（未被修改，正确！） */
    addi   t1, sp, TRAP_FRAME_SIZE
    sd     t1,  2*8(sp)          /* 原始 sp */
    j      .Lsave_rest

.Lfrom_user:
    /* U → S: t0=kernel_sp（旧 sscratch），sscratch=原始 t0 */
    mv     t1, sp                /* t1 = user_sp */
    mv     sp, t0                /* sp = kernel_sp */
    addi   sp, sp, -TRAP_FRAME_SIZE
    csrrw  t0, sscratch, x0      /* t0=用户原始t0, sscratch=0 */
    sd     x5,  5*8(sp)          /* 用户原始 t0（正确！） */
    sd     x6,  6*8(sp)          /* 用户原始 t1（未被修改，正确！） */
    sd     t1,  2*8(sp)          /* user_sp */

.Lsave_rest:
    // 保存其余寄存器（跳过 x2/x5/x6，已保存）
    sd    x0,   0*8(sp)
    sd    x1,   1*8(sp)
    // ... x3, x4, x7-x31 ...
```

**旧代码错误**:
```asm
// ❌ 错误：先读 sstatus 判断 SPP，污染了 t0
csrr  t0, sstatus
andi  t0, t0, SSTATUS_SPP    // t0 被修改！
bnez  t0, 1f

// 之后保存的 x5(t0) 是被污染的值
sd    x5,   5*8(sp)          // ❌ 保存的是错误值
```

#### 异常返回路径
```asm
// boot/riscv64/exception.S
    // ... 恢复寄存器 ...

    /*
     * 根据返回目的地更新 sscratch：
     *   返回 U 态（SPP=0）: sscratch = 内核栈顶（下次 U→S 陷阱使用）
     *   返回 S 态（SPP=1）: sscratch = 0（维持"内核态=0"约定）
     */
    andi  t1, t0, SSTATUS_SPP
    bnez  t1, 3f
    /* 返回 U 态：设置 sscratch = kernel_sp */
    addi  t1, sp, TRAP_FRAME_SIZE
    csrw  sscratch, t1
    j     4f
3:
    /* 返回 S 态：sscratch 清零 */
    csrw  sscratch, x0
4:
    // 恢复寄存器
    ld    x2,   2*8(sp)
    sret                   // SIE <- SPIE（自动恢复中断状态）
```

**硬件行为对比**：

| 架构    | 进入异常时                | 异常返回时                |
|---------|---------------------------|---------------------------|
| AArch64 | `DAIF.I` 自动置 1（硬件） | `SPSR[9:6]` → `DAIF[9:6]` |
| RISC-V  | `SIE` 不变（需软件维持 0）| `SPIE` → `SIE`            |

**关键差异**：AArch64 硬件自动在进入 EL1 时屏蔽 IRQ，RISC-V 需要软件保证 S-mode 的 SIE=0。

---

## 四、错误案例分析

### 4.1 错误场景：busybox 崩溃
```
[INFO] kernel/task/task.c:141: [user-entry] trampoline: entry=0x18c22 
       usp=0x6fffff28 ksp=0xffffffc08024c240 upgd=0x80828000 
       satp=0x8000000000080205 sstatus=0x8000000200046020 
       sie=0x20 stvec=0xffffffc08020fb80
[ERROR] boot/riscv64/exception.c:96: Store PF: pc=0xffffffc080206ee4 
        va=0x2f3a6e69622f7a
```

**崩溃地址分析**：
- `pc=0xffffffc080206ee4` → 内核代码（`list_insert_last`）
- `va=0x2f3a6e69622f7a` → ASCII 字符串 "/z:bin/"（PATH 环境变量）

**根因推测**：
1. **旧代码**：`ori t0, t0, 0x22` 在 sret 前设置 SIE=1
2. **窗口期中断**：定时器在 `csrw sstatus` 到 `sret` 之间触发
3. **S→S 陷阱**：旧 `trap_vector` 用 `csrr t0, sstatus` 判断 SPP，污染 t0
4. **t0 值损坏**：中断返回后，t0 不再是正确的指针，导致 `sd` 指令访问非法地址

**修复验证**：
- ✅ 修复 1：`exception_init()` 不调用 `CSR_SET(sstatus, SSTATUS_SIE)`
- ✅ 修复 2：`arch_switch_to_user()` 只设 SPIE，不设 SIE
- ✅ 修复 3：`trap_vector` 用 `sscratch=0` 约定，在判断前不污染 t0/t1

### 4.2 loop_test 程序能运行的原因

loop_test.S 是极简程序：
```asm
_start:
    li a0, 1           // fd = stdout
    la a1, msg         // buf = "AAAA\n"
    li a2, 5           // len = 5
    li a7, 64          // syscall write
    ecall
    j _start           // 无限循环
```

**为什么能运行**：
- 系统调用路径极短，t0/t1 未被大量使用
- 无复杂内核操作（如链表插入、页表分配）
- 运气好，定时器未在关键窗口触发

**busybox 为什么必现**：
- 代码量大（5MB），内核路径复杂
- ELF 加载、页表映射、重定位过程频繁使用 t0/t1
- 执行时间长，定时器中断必定在窗口期触发

---

## 五、最佳实践总结

### 5.1 通用原则
1. **内核态禁止中断**：简化状态管理，避免寄存器污染
2. **用户态使能中断**：响应定时器，实现抢占式调度
3. **异常入口立即关中断**：保护现场保存过程
4. **异常返回恢复中断状态**：根据目标特权级自动切换

### 5.2 RISC-V 特有注意事项
1. **sscratch=0 约定**：内核态 0，用户态非 0（存内核栈指针）
2. **SIE 软件维护**：不像 AArch64 有硬件自动屏蔽，需主动保持 0
3. **SPIE 机制**：sret 通过 SPIE→SIE 切换中断状态，勿手动设 SIE
4. **t0/t1 保护**：异常入口第一时间保存，避免判断逻辑污染

### 5.3 调试技巧
1. **对比 loop_test vs busybox**：简单程序能跑不代表复杂程序没问题
2. **检查 sstatus 值**：SIE 位应始终为 0（内核态）
3. **GDB 断点**：在 `csrw sstatus` 设置，检查 t0/t1 值是否正常
4. **中断计数器**：统计 S→S 陷阱次数，不应有（说明内核态中断开了）

---

## 六、代码检查清单

### AArch64（参考标准）
- [ ] `exception_init()` 不调用 `msr daifclr, #2`
- [ ] **内核线程**：`task_trampoline()`（C）入口调用 `arch_irq_enable()`（开中断，可被抢占）
- [ ] **EL0 任务**：`task_trampoline_user()`（asm）**不**包含 `msr daifclr, #2`——中断留给 `eret`
- [ ] **EL0 任务**：`arch_fork_resume_user()`（asm）**不**包含 `msr daifclr, #2`——中断留给 `eret`
- [ ] `arch_switch_to_user()` 设置 `SPSR_EL1 = 0x340`（DAIF.I=0），由 `eret` 在跨入 EL0 瞬间开中断
- [ ] 异常向量表进入时硬件自动设置 `DAIF.I=1`；返回时 `eret` 按栈上 `SPSR` 忠实恢复
- [ ] `idle` / secondary idle 入口（`task_idle_loop` / `cpu_secondary_bootstrap`）调用 `arch_irq_enable()`

### RISC-V（需对齐）
- [ ] `exception_init()` 不调用 `CSR_SET(sstatus, SSTATUS_SIE)`
- [ ] `arch_switch_to_user()` 清除 SIE，仅设 SPIE
- [ ] `trap_vector` 使用 `sscratch=0` 约定判断模式
- [ ] `trap_vector` 在保存 x5/x6 前完成判断，不污染 t0/t1
- [ ] 异常返回前根据 SPP 设置 sscratch（U 态非 0，S 态 0）

---

## 七、参考资料

### RISC-V 规范
- **RISC-V Privileged Specification v1.12**
  - 第 4.1.1 节：sstatus 寄存器
  - 第 4.1.7 节：异常处理流程
  - 第 4.2.1 节：sret 指令行为

### Linux RISC-V 内核
- `arch/riscv/kernel/entry.S` - 使用 sscratch=0 约定
- `arch/riscv/kernel/head.S` - 初始化时关闭 SIE

### AArch64 参考
- **ARM Architecture Reference Manual ARMv8**
  - 第 D1.16 节：异常处理
  - 第 D1.17 节：DAIF 中断屏蔽

---

## 八、版本历史

| 版本 | 日期       | 变更内容                               |
|------|------------|----------------------------------------|
| 1.0  | 2026-05-05 | 初始版本，完整对比 AArch64 和 RISC-V   |
| 1.1  | 2026-06-18 | 修正核心约定：中断策略按“EL1 里运行的实体”区分——EL0 任务的内核侧关中断，独立内核线程开中断（可被抢占）。澄清 `task_trampoline`（内核线程，显式开中断）与 `task_trampoline_user`/`arch_fork_resume_user`（EL0 任务，不显式开、留给 eret）的分野；删除后两者中冗余且有害的 `daifclr`。 |

---

**维护者**: Avatar OS Team  
**联系**: 通过 Git Issues 反馈问题
