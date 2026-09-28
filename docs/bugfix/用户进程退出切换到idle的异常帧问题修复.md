# 用户进程退出时切换到 idle 的异常帧问题修复

## 问题描述

### 现象
当用户进程（如 `/bin/ls`）通过系统调用退出时，内核切换到 idle 任务后发生 EL1 异常：

```
[ERROR] [el1_sync] EL1 exception: EC=0x0, ESR=0x2000000, FAR=0x0
[ERROR] [el1_sync] ELR=0x118420, SP_EL0=0x6ffffe10, SPSR=0x600003c5
```

**关键信息**：
- **ELR=0x118420**：这是一个**用户空间地址**（< 0x40000000）
- **当前任务**：idle (id=0)
- **SP_EL0=0x6ffffe10**：用户栈指针
- **SPSR=0x600003c5**：准备返回 EL0 的状态

### 影响范围
- 所有通过系统调用退出的用户进程
- 导致系统崩溃，无法继续运行

## 问题根因分析

### 调用链追踪

当用户进程 `ls` 调用 `exit()` 系统调用退出时：

1. **异常发生**，硬件保存异常帧到 ls 的内核栈：
   ```
   handle_el0_sync_exception → syscall_handler → sys_exit → task_exit → sched_schedule
   ```

2. **`sched_schedule` 调用 `arch_task_switch`** 切换到 idle

3. **问题**：切换到 idle 后，**调用链仍然存在**！

4. **后续中断发生**时：
   - 中断处理在 idle 的栈上保存新的异常帧
   - 中断返回时，`RESTORE_REGS` 恢复的是 **ls 的异常帧**（保存在 ls 的内核栈上）
   - ls 的异常帧包含 **用户地址 ELR=0x118420**

5. **执行 `eret`** 跳转到用户地址 0x118420，但此时：
   - TTBR0 已经是内核页表（切换到 idle 时设置）
   - 用户地址 0x118420 在内核页表中**没有映射**
   - **触发异常**！

### 核心问题

**调用链未被切断**：用户进程退出时的系统调用异常帧保存在其内核栈上，切换到 idle 后，这个异常帧仍然存在。后续中断返回时会错误地恢复这个过时的异常帧，导致跳转到用户地址而崩溃。

## 解决方案

### 设计思路

在切换到 idle 任务时：
1. **不返回到调用者**（不执行 `ret`）
2. **直接进入 idle 循环**
3. **切断调用链**，防止后续中断返回路径尝试恢复过时的异常帧

### 实现细节

#### 1. C 代码层（kernel/task/sched.c）

在 `sched_schedule()` 中检测到切换到 idle 时，传递特殊标记：

```c
uintptr_t switch_sp = next->sp;
if (next == g_idle) {
    switch_sp = (uintptr_t)-1;  /* 特殊标记：切换到 idle */
}
arch_task_switch(&prev->sp, switch_sp, &prev->pgd, next->pgd);
```

**关键点**：
- 通过 `next == g_idle` 判断是否切换到 idle
- 使用 `-1` 作为特殊标记（不可能是有效的栈地址）
- 正常任务切换时传递真实的 `next->sp`

#### 2. 汇编代码层（kernel/task/aarch64/switch.S）

在 `arch_task_switch()` 中检测特殊标记：

```asm
/* 检查是否切换到 idle 任务 */
mov     x9, #-1
cmp     x1, x9
b.eq    .Lswitch_to_idle

/* 正常任务切换：恢复寄存器并返回 */
mov     sp, x1
ldp     x29, x30, [sp, #80]
...
ret

.Lswitch_to_idle:
    /* 切换到 idle：不返回，直接进入 idle 循环 */
    msr     daifclr, #2        /* 开启中断 */

.Lidle_loop:
    wfi                     /* 等待中断 */
    b       .Lidle_loop
```

**关键点**：
- 检测 `next_sp == -1`
- 如果是，**不恢复寄存器**（x19-x30）
- **不执行 `ret`**（不返回到 `sched_schedule`）
- 直接开启中断并进入 `wfi` 循环
- 这样切断了调用链，ls 的异常帧不会被后续中断返回恢复

## 修改的文件

### 1. kernel/task/sched.c
**位置**：第 114-135 行

**改动**：添加特殊标记逻辑和详细注释

```c
/*
 * 特殊处理：如果切换到 idle，传递一个特殊标记（next->sp == (uintptr_t)-1）
 * 让 arch_task_switch 知道不要返回，而是直接进入 idle 循环。
 * 这样可以切断调用链，防止从用户进程退出时的异常帧被后续中断返回恢复。
 */
uintptr_t switch_sp = next->sp;
if (next == g_idle) {
    switch_sp = (uintptr_t)-1;  /* 特殊标记：切换到 idle */
}
arch_task_switch(&prev->sp, switch_sp, &prev->pgd, next->pgd);
```

### 2. kernel/task/aarch64/switch.S
**位置**：第 56-96 行

**改动**：
- 添加 idle 切换检测逻辑
- 实现 `.Lswitch_to_idle` 分支
- 优化注释，添加详细说明

```asm
/* ── 检查是否切换到 idle 任务──────────────────────────── */
/* next_sp == -1 表示切换到 idle（由 sched_schedule 设置） */
mov     x9, #-1
cmp     x1, x9
b.eq    .Lswitch_to_idle

/* ── 正常任务切换：加载下一任务的 SP 并恢复寄存器 ──────── */
mov     sp, x1
ldp     x29, x30, [sp, #80]
...
ret

/* ── 切换到 idle 任务──────────────────────────────────── */
.Lswitch_to_idle:
    /*
     * 切换到 idle 时不返回调用者，直接进入 idle 循环。
     *
     * 原因：用户进程通过系统调用退出时，异常帧保存在其内核栈上
     * （包含用户地址 ELR）。如果正常返回到调用者，后续中断返回路径
     * 会尝试恢复这个过时的异常帧，导致跳转到用户地址而崩溃。
     *
     * 解决：不执行 ret，直接进入 idle 循环，切断调用链。
     */
    msr     daifclr, #2        /* 开启中断 */

.Lidle_loop:
    wfi                         /* 等待中断 */
    b       .Lidle_loop
```

### 3. boot/aarch64/exception.c
**位置**：第 21-35 行

**改动**：清理调试代码，恢复为简洁的异常报告

```c
void handle_sync_exception(uint64_t *stack_pointer)
{
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    uint64_t esr = READ_ESR_EL1();
    uint64_t far = READ_FAR_EL1();
    uint32_t ec  = (esr >> 26) & 0x3F;
    uint32_t dfsc = esr & 0x3F;

    KLOG_ERROR("[el1_sync] EL1 exception: EC=0x%x, ESR=0x%llx, FAR=0x%llx\n",
               ec, esr, far);
    KLOG_ERROR("[el1_sync] ELR=0x%llx, SP_EL0=0x%llx, SPSR=0x%llx\n",
               el1_ctx->elr, el1_ctx->usp, el1_ctx->spsr);
    KLOG_ERROR("[el1_sync] DFSC=0x%x (translation=%d perm=%d)\n",
               dfsc, (dfsc & 0x3C) == 0x04, (dfsc & 0x3C) == 0x0C);

    (void)ec;

    platform_shutdown();
}
```

## 技术细节

### AArch64 异常处理机制

1. **异常发生时**：硬件自动保存：
   - ELR_EL1：异常返回地址
   - SPSR_EL1：保存的程序状态
   - SP_EL0：用户栈指针（如果从 EL0 来）
   - 其他寄存器到内核栈

2. **异常返回（eret）**：
   - 从 ELR_EL1 恢复执行地址
   - 从 SPSR_EL1 恢复处理器状态
   - 从 SP_EL0 恢复用户栈指针

### 为什么会跳转到用户地址

1. ls 退出时的异常帧（包含用户地址 ELR）保存在 ls 的内核栈
2. 切换到 idle 后，TTBR0_EL1 设置为内核页表
3. 后续中断返回时，`RESTORE_REGS` 从 ls 的内核栈恢复异常帧
4. 执行 `eret` 时：
   - ELR = 0x118420（用户地址，ls 的代码段）
   - TTBR0 = 内核页表（没有用户地址映射）
   - **地址翻译失败 → 异常**

### 为什么不能简单地修复异常帧

1. **异常帧属于 ls**，保存在 ls 的内核栈
2. **切换到 idle 后**，已经无法访问 ls 的内核栈
3. **调用链仍然存在**：中断返回会沿着调用链回到 `handle_el0_sync_exception`
4. **必须切断调用链**，让切换到 idle 后不再返回

## 测试验证

### 测试场景
```bash
make ARCH=aarch64 clean
make ARCH=aarch64 kernel
make ARCH=aarch64 run
```

### 预期结果
1. ✅ busybox 成功启动并运行
2. ✅ `/bin/ls` 成功执行
3. ✅ ls 退出后成功切换到 idle
4. ✅ 系统进入 idle 循环，打印 `pick_next: queue empty, returning idle`
5. ✅ **没有 EL1 异常**

### 实际结果
```
[DEBUG] [sched] switch: prev='/bin/ls' (id=4) -> next='idle' (id=0, is_user=0)
[DEBUG] kernel/task/sched.c:77: [sched] pick_next: queue empty, returning idle
[DEBUG] kernel/task/sched.c:77: [sched] pick_next: queue empty, returning idle
...
```

系统成功进入 idle 循环，没有崩溃！

## 经验总结

### 关键教训

1. **调用链的生命周期**：任务切换时，调用链仍然存在于栈上
2. **异常帧的生命周期**：异常帧保存在发生异常的任务的栈上，切换任务后仍然存在
3. **中断返回路径**：中断返回会沿着调用链恢复上下文，可能恢复过时的异常帧
4. **页表切换时机**：页表切换后，之前任务的地址映射可能不再有效

### 设计原则

1. **明确状态转换**：任务退出时应该明确切断所有返回路径
2. **防止悬空指针**：不要让中断返回路径尝试访问已退出任务的资源
3. **简化特殊情况**：idle 任务不需要正常的返回路径，可以直接进入循环
4. **文档化关键决策**：在代码中添加详细注释说明特殊处理的原因

## 相关概念

### AArch64 异常级别
- **EL0**：用户态（User Mode）
- **EL1**：内核态（Kernel Mode）
- **EL2**：虚拟化（Hypervisor Mode）
- **EL3**：监控（Secure Monitor）

### 系统调用流程
1. 用户态执行 `svc` 指令
2. 硬件切换到 EL1
3. 保存异常帧到内核栈
4. 调用系统调用处理函数
5. 执行 `eret` 返回用户态

### 任务切换流程
1. 保存当前任务寄存器
2. 切换页表（如果需要）
3. 切换栈指针
4. 恢复新任务寄存器
5. 返回（`ret`）到新任务

## 参考资料

- **ARM Architecture Reference Manual**：异常处理和上下文切换
- **kernel/task/task.c**：任务生命周期管理
- **kernel/task/sched.c**：调度器实现
- **boot/aarch64/exception.S**：异常处理汇编代码

---

**文档版本**：1.0  
**创建日期**：2026-05-03  
**作者**：Avatar OS 开发团队  
**问题修复状态**：✅ 已解决
