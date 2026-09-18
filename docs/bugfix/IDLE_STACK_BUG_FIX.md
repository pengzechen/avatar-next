# Idle 任务栈溢出 Bug 修复记录

**日期**: 2026-05-04  
**影响架构**: RISC-V 64-bit, AArch64  
**严重性**: 高（可导致系统崩溃）

---

## 问题描述

### 症状

**RISC-V 64**：
- 用户进程退出后，系统进入 idle 循环
- 等待几分钟后出现 Store/AMO page fault：
  ```
  [ERROR] Unhandled exception: scause=0xf sepc=0xffffffc08020fcc4 stval=0xffffffc0c0000000
  ```
- `sepc` 指向 `trap_vector` 中的 `sd` 指令
- `stval` 是无效的栈地址（0xffffffc0c0000000）

**AArch64**：
- 相同的潜在问题，但更隐蔽
- 日志显示 idle 会停留在已退出任务的栈上
- 暂未触发崩溃，但随时可能发生

### 根本原因

#### RISC-V 64

1. **idle 任务的 sp 初始化为 0**
   - `task_init()` 中：`g_idle_task.sp = 0;`
   - 期望在首次切换时由 `arch_task_switch` 自动填入

2. **sched.c 传递 sp=-1 标记**
   - 切换到 idle 时：`switch_sp = (uintptr_t)-1;`
   - 意图是告诉 `arch_task_switch` 采取特殊处理

3. **arch_task_switch 不支持 sp=-1**
   - RISC-V 的 `switch.S` 没有检查 `-1`
   - 直接执行 `mv sp, a1`，将 sp 设置为 `-1` (0xffffffffffffffff)
   - 恢复寄存器时：`addi sp, sp, 104` → sp 变成奇怪的值

4. **下次中断触发 page fault**
   - 定时器中断在无效 sp 上保存异常帧
   - `trap_vector` 的 `sd` 指令写入无效地址
   - 触发 Store/AMO page fault

#### AArch64

1. **使用 sp=-1 标记**
   - `sched.c` 在切换到 idle 时传递 `switch_sp = -1`
   - AArch64 的 `switch.S` 有 `.Lswitch_to_idle` 标签处理此情况

2. **.Lswitch_to_idle 没有设置栈指针**
   ```asm
   .Lswitch_to_idle:
       msr     daifclr, #2        /* 开启中断 */
   .Lidle_loop:
       wfi                         /* 等待中断 */
       b       .Lidle_loop
   ```
   - **没有加载 idle.sp！**
   - sp 仍指向 prev 任务的栈（已退出任务的栈）

3. **潜在的栈溢出**
   - prev 的栈可能已被标记为 DEAD，但物理内存还在
   - 如果新任务复用该栈槽，内存会被覆盖
   - 定时器中断在这个"幽灵栈"上保存异常帧
   - 随着中断累积，最终会覆盖关键数据或触发崩溃

---

## 修复方案

### 统一解决方案（推荐）

**为 idle 任务分配专用栈，移除 sp=-1 特殊标记**

#### 1. 创建 idle 专用栈

`kernel/task/task.c`:
```c
/* ── idle 任务（boot 执行上下文）────────────────────────── */
static task_t g_idle_task;

/* ── idle 专用栈（防止 boot 栈在频繁中断下溢出）────────────── */
static uint8_t g_idle_stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
```

#### 2. 初始化 idle.sp 为栈顶

`kernel/task/task.c` 的 `task_init()`:
```c
/*
 * 重要：idle.sp 必须指向实际的栈顶，因为：
 *   1. 任务退出后切换回 idle 时，arch_task_switch 会加载 idle.sp
 *   2. 如果 sp=0 或无效值，下次中断会在无效地址保存寄存器，触发 page fault
 */
g_idle_task.sp         = (uintptr_t)(g_idle_stack + TASK_STACK_SIZE);
g_idle_task.state      = TASK_RUNNING;
g_idle_task.id         = g_task_id_cnt++;
g_idle_task.priority   = 255;
g_idle_task.stack_base = g_idle_stack;  // 不再是 NULL
```

#### 3. 在安全时机切换到 idle 栈

`kernel/task/task.c`:
```c
/*
 * task_switch_to_idle_stack - 切换到 idle 专用栈
 * 
 * 必须在 task_init() 返回后、进入 idle 循环前调用。
 * 在 task_init() 内部切换会导致返回地址丢失。
 */
void task_switch_to_idle_stack(void)
{
    uintptr_t new_sp = (uintptr_t)(g_idle_stack + TASK_STACK_SIZE);
#if ARCH_RISCV64
    __asm__ volatile("mv sp, %0" :: "r"(new_sp) : "memory");
#elif ARCH_AARCH64
    __asm__ volatile("mov sp, %0" :: "r"(new_sp) : "memory");
#elif ARCH_X86_64
    __asm__ volatile("mov %0, %%rsp" :: "r"(new_sp) : "memory");
#endif

    KLOG_INFO("[task] switched to idle stack at 0x%lx\n", (unsigned long)new_sp);
}
```

`kernel/main.c`:
```c
/* 初始化任务子系统 */
task_init();

/* 切换到 idle 专用栈（防止 boot 栈在频繁中断下溢出） */
task_switch_to_idle_stack();
```

#### 4. 移除 sp=-1 特殊标记

`kernel/task/sched.c`:
```c
uintptr_t switch_sp = next->sp;

/* 注意：之前 AArch64 使用 sp=-1 标记切换到 idle，但这导致 
 * .Lswitch_to_idle 没有设置栈指针，造成潜在的栈溢出问题。
 * 现在所有架构统一：idle 有专用栈，正常返回即可。*/

// 删除以下代码：
// #if ARCH_AARCH64
//     if (next == g_idle) {
//         switch_sp = (uintptr_t)-1;
//     }
// #endif
```

---

## 修复验证

### 测试方法

创建短生命周期任务，触发栈槽复用：

```c
static void short_lived_task(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    KLOG_INFO("[short_task_%u] running and exiting immediately\n", id);
    
    /* 在栈上分配较多数据，增加栈使用 */
    volatile char stack_filler[1024];
    for (int i = 0; i < 1024; i++) {
        stack_filler[i] = (char)(id + i);
    }
    
    task_exit();
}

static void stack_overflow_test(void *arg)
{
    /* 创建 50 个短生命周期任务 */
    for (uint32_t i = 0; i < 50; i++) {
        task_create("short_task", short_lived_task, 
                    (void*)(uintptr_t)i, 5);
        /* 让出 CPU */
        for (int j = 0; j < 10; j++) {
            task_yield();
        }
    }
    
    task_exit();  /* 系统进入 idle */
}
```

### 预期结果

**修复前**：
- RISC-V：几分钟后触发 Store page fault
- AArch64：日志显示 "CPU will stay on prev's stack"

**修复后**：
- 系统正常进入 idle
- 日志显示 idle 使用专用栈地址
- 长时间运行无崩溃

---

## 教训总结

### 1. **架构抽象的一致性**

问题根源是不同架构采用了不同的 idle 处理策略：
- AArch64 有 `.Lswitch_to_idle` 特殊路径（但未正确实现）
- RISC-V 没有对应实现，导致 sp=-1 被直接使用

**教训**：跨架构特性必须在所有架构上正确实现或统一移除。

### 2. **栈管理的严格性**

- **每个执行上下文必须有有效的栈**，包括 idle 任务
- **不要依赖 "boot 栈够大" 的假设**，频繁中断会累积消耗
- **栈切换时机很关键**，必须在安全点（无返回地址依赖时）

### 3. **特殊标记的危险性**

使用 `-1` 等魔数作为特殊标记容易引入 bug：
- 调用方和实现方必须完全理解语义
- 不同架构可能有不同解释
- 更好的方案：显式的状态标志或专用函数

### 4. **日志驱动调试**

这个 bug 的发现过程：
1. 现象：随机崩溃在 trap_vector
2. 分析：stval 显示无效栈地址
3. 假设：idle 栈有问题
4. 验证：添加日志显示 sp 不匹配
5. 修复：分配专用栈并正确初始化

**教训**：关键路径（如任务切换）应有足够的日志支持调试。

### 5. **早期测试的重要性**

AArch64 的 bug 一直潜伏，因为：
- busybox 测试时栈还没被复用
- 短任务测试才暴露问题

**教训**：设计压力测试（如本次的短生命周期任务测试）覆盖边界情况。

---

## 相关文件

**修改的文件**：
- `kernel/task/task.c` - 添加 idle 专用栈，初始化 idle.sp
- `kernel/task/task.h` - 添加 `task_switch_to_idle_stack()` 声明
- `kernel/task/sched.c` - 移除 sp=-1 特殊标记
- `kernel/main.c` - 调用 `task_switch_to_idle_stack()`

**不需要修改的文件**：
- `kernel/task/aarch64/switch.S` - `.Lswitch_to_idle` 代码保留但不再使用
- `kernel/task/riscv64/switch.S` - 无需修改
- `boot/riscv64/exception.S` - 无需修改（trap_vector 正常工作）

---

## 参考

- [SPINLOCK.md](SPINLOCK.md) - 内存屏障和中断安全
- [INTERRUPT_CONTEXT_SWITCH.md](INTERRUPT_CONTEXT_SWITCH.md) - 中断上下文切换
- [用户进程退出切换到idle的异常帧问题修复.md](用户进程退出切换到idle的异常帧问题修复.md) - 相关历史问题

---

**状态**: ✅ 已修复  
**测试**: ✅ RISC-V 64 通过，✅ AArch64 通过  
**回归测试**: busybox 正常运行
