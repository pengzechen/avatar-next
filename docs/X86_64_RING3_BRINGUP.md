# x86_64 用户态（Ring 3）Bring-Up 文档

> 记录从零实现 x86_64 Ring3 用户态支持的完整过程，包含所有踩坑点和修复方案。
>
> 最终产出：两个用户态进程（`user_test`、`hello`）在 QEMU 上稳定交替运行，SYSCALL/SYSRET 正常工作。

---

## 目录

1. [背景与目标](#1-背景与目标)
2. [前置知识：x86_64 用户态基础设施](#2-前置知识x86_64-用户态基础设施)
3. [GDT 布局](#3-gdt-布局)
4. [TSS 配置](#4-tss-配置)
5. [SYSCALL/SYSRET 配置](#5-syscallsysret-配置)
6. [页表（Ring3 映射）](#6-页表ring3-映射)
7. [进入 Ring3：IRETQ 流程](#7-进入-ring3iretq-流程)
8. [踩坑记录（7 个关键 Bug）](#8-踩坑记录7-个关键-bug)
9. [关键设计原则总结](#9-关键设计原则总结)
10. [涉及文件列表](#10-涉及文件列表)

---

## 1. 背景与目标

项目在 aarch64 / riscv64 上已有完整的用户态支持（多任务调度、SYSCALL 处理、用户页表）。
本次工作目标是为 **x86_64** 补全同等能力：

- 4 级页表（PML4 → PDPT → PD → PT）用户段映射
- TSS + GDT 正确初始化，支持 Ring0/Ring3 权限切换
- `IRETQ` 进入 Ring3，`SYSCALL`/`SYSRET` 返回内核
- 两个用户进程稳定运行并切换

---

## 2. 前置知识：x86_64 用户态基础设施

x86_64 进入 Ring3 需要以下硬件结构全部就绪：

| 结构 | 作用 |
|------|------|
| GDT | 描述符表，包含内核/用户代码段、数据段、TSS 描述符 |
| TSS | 保存 Ring0 栈指针（RSP0），CPU 特权级切换时自动加载 |
| IDT | 中断/异常处理入口，异常返回路径决定是否切换任务 |
| EFER.SCE | 启用 `SYSCALL`/`SYSRET` 指令（默认关闭！） |
| MSR\_STAR | 配置 `SYSCALL`/`SYSRET` 使用的段选择子 |
| MSR\_LSTAR | `SYSCALL` 跳转目标 RIP（64-bit 内核入口） |
| MSR\_SFMASK | `SYSCALL` 时自动清除的 RFLAGS 位 |
| 页表 | 用户段（PTE\_USER）映射代码和栈 |

---

## 3. GDT 布局

```
索引   偏移   描述
 0     0x00   Null 描述符
 1     0x08   Code32  DPL=0（遗留，boot 阶段用）
 2     0x10   Code64  DPL=0（内核代码）
 3     0x18   Data64  DPL=0（内核数据）
 4     0x20   Data64  DPL=3（用户数据，SS）
 5     0x28   Code64  DPL=3（用户代码，CS）
 6     0x30   TSS 描述符低 8 字节
 7     0x38   TSS 描述符高 8 字节（64-bit TSS 占两个槽）
```

### SYSCALL/SYSRET 与段选择子的关系

`MSR_STAR[63:48]` 配置用户段基址，CPU 规定：
- SYSRET 后 CS = `STAR[63:48] + 16`，SS = `STAR[63:48] + 8`
- 因此 STAR 高 16 位应设为用户数据段索引前一个，即 `0x18`

实际选择子（RPL=3）：
- 用户 CS = `0x28 | 3 = 0x2B`
- 用户 SS = `0x20 | 3 = 0x23`

SYSCALL 时（进入内核）：
- CS = `STAR[47:32]`，即 `0x10`（内核代码段）
- SS = `STAR[47:32] + 8`，即 `0x18`（内核数据段）

---

## 4. TSS 配置

### TSS 的核心作用

当发生特权级切换（Ring3 → Ring0）时，CPU 从当前 TR（Task Register）指向的 TSS 中读取 `RSP0`，作为内核栈顶。

### 关键实现细节

```c
// tss.c

// ⚠️ 顺序至关重要：内核栈必须在 TSS 之前定义
// 否则 TSS.RSP0 = &g_tss，异常处理会覆盖 TSS 本身！
static uint8_t g_kernel_stack[16384];   // 先定义栈
static tss_t   g_tss;                   // 再定义 TSS

uint64_t g_x86_tss_rsp0;  // 导出给汇编使用，随任务切换更新
```

### TSS GDT 描述符

64-bit TSS 描述符占 **16 字节**（两个 GDT 槽）：

```c
// ⚠️ base 必须是 uint64_t，不能截断！
static void tss_set_gdt_entry(uint64_t base, uint32_t limit) {
    // 低 8 字节：标准段描述符格式
    // 高 8 字节：base[63:32]
    // 截断为 uint32_t 会导致高地址 TSS 加载失败 → Triple Fault
}
```

### 每次任务切换更新 RSP0

```c
// sched.c 中，切换到用户任务时：
g_x86_tss_rsp0 = (uint64_t)next_task->kernel_stack_top;
```

---

## 5. SYSCALL/SYSRET 配置

### MSR 配置一览

```c
// exception.c / boot.S

// 启用 SYSCALL 指令（EFER 第 0 位）
uint64_t efer = rdmsr(MSR_EFER);
wrmsr(MSR_EFER, efer | EFER_SCE);

// 配置段选择子
// [47:32] = 内核 CS base = 0x10
// [63:48] = 用户 CS base - 16 = 0x18
wrmsr(MSR_STAR, (0x10ULL << 32) | (0x18ULL << 48));

// SYSCALL 入口
wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

// 进入内核时清除 IF（禁止中断）和 DF
wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF);
```

### syscall_wrapper.S 核心逻辑

```asm
syscall_entry:
    # ⚠️ SYSCALL 不会自动切换栈！必须手动切换到内核栈
    # 错误做法：使用静态共享栈（多任务会互相覆盖）
    # 正确做法：使用 TSS.RSP0（每个任务独立的内核栈）
    movq g_x86_tss_rsp0(%rip), %rsp

    # 保存用户态 RCX（返回 RIP）和 R11（返回 RFLAGS）
    push %rcx
    push %r11
    # ... 调用 C 层 syscall_handler
    pop %r11
    pop %rcx
    sysretq
```

---

## 6. 页表（Ring3 映射）

### 映射权限

用户页表映射需要在 PTE 中设置 `PTE_USER`（bit 2），否则 Ring3 访问会触发 Page Fault #14（EC 含 U/S=0 但 CPL=3）。

中间级页表（PML4E、PDPTE、PDE）同样需要 `PTE_USER`，否则 MMU 遍历时会拒绝用户访问：

```c
// mm_vm.h
#define x86_make_table_pte(pa)  ((pa) | PTE_PRESENT | PTE_WRITABLE | PTE_USER)
//                                                              ^^^^^^^^^^^
//                                     所有中间级必须带 USER，否则叶子级 USER 无效
```

### 用户内存布局（QEMU x86_64）

```
0x010000  用户代码（.text.user，从 ELF 加载）
0x01ffff  用户栈顶（向下增长，栈底约 0x1ffff0）
```

---

## 7. 进入 Ring3：IRETQ 流程

`task_trampoline_user`（switch.S）通过 `iretq` 首次进入 Ring3：

```asm
task_trampoline_user:
    # 构造 Ring3 异常返回帧（从高到低入栈）
    push $0x23          # SS  = 用户数据段选择子（DPL=3）
    push user_rsp       # RSP = 用户栈顶
    push $0x202         # RFLAGS = IF=1（允许中断）
    push $0x2B          # CS  = 用户代码段选择子（DPL=3）
    push user_rip       # RIP = 用户程序入口

    iretq               # CPU 验证段描述符 + 切换到 Ring3
```

`iretq` 时 CPU 会：
1. 从栈弹出 RIP、CS、RFLAGS
2. 检测 CS.RPL > CPL → 发生特权级切换
3. 继续从栈弹出 RSP、SS
4. 切换到 Ring3，跳转到 RIP

---

## 8. 踩坑记录（7 个关键 Bug）

---

### Bug 1：Triple Fault / QEMU 立即重启

**现象**：执行 `iretq` 后 QEMU 立即重置，无任何输出。

**根本原因 A：TSS 描述符 base 被截断为 uint32_t**

```c
// 错误代码（tss.c 旧版）
void tss_set_gdt_entry(uint32_t base, ...) {  // ← 截断了高 32 位！
```

内核运行在高半部（`0xffff800000xxxxxx`），TSS 地址高位被丢弃，CPU 加载了错误的 TSS，Ring3 切换失败 → Triple Fault。

**修复**：

```c
void tss_set_gdt_entry(uint64_t base, uint32_t limit) {
    // 正确设置高 8 字节的 base[63:32]
}
```

**根本原因 B：变量定义顺序错误**

```c
// 错误顺序
static tss_t   g_tss;           // TSS 先定义
static uint8_t g_kernel_stack[16384];  // 栈在后面

// g_tss.rsp0 = &g_tss（栈顶指向 TSS 自身！）
// 异常处理压栈 → 覆盖 TSS → 崩溃
```

**修复**：调换定义顺序，让 `g_kernel_stack` 在前，`g_tss` 在后，使 `rsp0 = g_kernel_stack + 16384`。

**根本原因 C：切换页表后 GDTR 仍指向低地址**

内核切换到高半部 CR3 后，bootloader 设置的低地址映射消失，但 GDTR 仍指向低地址 GDT，`iretq` 加载段描述符时触发 #GP → Triple Fault。

**修复**：在 `boot64_virt`（高半部虚拟地址入口点）重新加载 GDTR：

```asm
boot64_virt:
    lgdt [boot_gdt_desc64]   # 使用高半部虚拟地址的 GDT 描述符
```

---

### Bug 2：#UD（无效操作码）在 RIP=0x10015

**现象**：用户程序第一条 `syscall` 指令触发 #UD（中断向量 6），CPU 不认识该指令。

**根本原因**：`EFER.SCE`（bit 0）未启用，`syscall` 在 64-bit 模式下默认无效。

**修复**：在 `boot.S` 配置 EFER 时加上 SCE：

```asm
rdmsr           ; 读 MSR_EFER (0xC0000080)
or  eax, 0x901  ; LME(bit8) | NXE(bit11) | SCE(bit0) ← 加上 SCE
wrmsr
```

同时在 `exception_init()` 中也显式设置（双保险）。

---

### Bug 3：Page Fault #14 EC=0x15（指令 fetch 失败）

**现象**：用户程序执行时触发 Page Fault，EC 的 I（Instruction fetch）位为 1，PRESENT 位为 1（页存在但权限不足）。

**根本原因**：中间级页表（PML4E/PDPTE/PDE）缺少 `PTE_USER` 标志，导致 MMU 拒绝 Ring3 指令 fetch。

**修复**：`x86_make_table_pte` 在所有中间级加上 `PTE_USER`。

---

### Bug 4：第二个用户进程从未被创建

**现象**：日志中只能看到 `user_test` 进程，`hello` 进程的创建日志缺失。

**根本原因**：`timer_set_tick_cb(sched_tick)` 在 `main()` 中过早调用，定时器 ISR 在 `kernel_main` 完成第一个进程创建后立即触发调度，`main()` 没有机会创建第二个进程。

**修复**：将 `timer_set_tick_cb(sched_tick)` 移到 **两个** `process_create()` 调用之后：

```c
// kernel/main.c
process_create("user_test", ...);
process_create("hello", ...);
timer_set_tick_cb(sched_tick);   // ← 最后再开启抢占
```

---

### Bug 5：输出出现乱码字符 `?`

**现象**：用户程序输出末尾有多余字符，偶发乱码。

**根本原因**：用户汇编中字符串长度硬编码错误：

```asm
# 字符串 "[user_test] #\n" 实际 15 字节，但写了 14
mov rsi, msg_len   # 错误的长度
```

**修复**：逐一核对所有字符串长度常量。

---

### Bug 6：CPU 异常 #14（Page Fault）发生在 RIP=0x86（内核地址）

**现象**：用户进程触发异常后，内核自身崩溃，RIP=0x86 不是任何合法内核函数。

**根本原因**：`handle_exception()` C 函数内直接调用 `task_exit()` 触发上下文切换，违反了"**C 异常处理函数中不能切换任务**"规则（见 `INTERRUPT_CONTEXT_SWITCH.md`）。

在 C 函数返回前切换任务会导致 `iretq` 返回到错误的寄存器状态（已被新任务污染）。

**修复**：在 C 层只**标记**任务状态，在 exception.S 的 `iretq` 前调用调度器：

```c
// exception.c handle_exception()
if (is_user_exception) {
    current_task->state = TASK_DEAD;
    sched_tick();   // 标记需要切换，但不在 C 中实际切换
    return;
}
```

```asm
; exception.S（iretq 之前）
call sched_check_and_yield   ; 在汇编层安全切换
iretq
```

---

### Bug 7：随机崩溃，RIP=0x86，两个任务的 task->sp 收敛到同一地址

**现象**：系统运行几十次 syscall 后崩溃，GDB 显示两个任务的 `sp` 字段均指向 `0xffff80000029d288`，是某个共享地址。

**根本原因**：`syscall_wrapper.S` 使用了**静态共享内核栈**：

```asm
# 旧代码（syscall_wrapper.S）
.bss
syscall_stack:  .space 8192
syscall_stack_top:

syscall_entry:
    leaq syscall_stack_top(%rip), %rsp   # 所有任务共用同一个栈！
```

流程：
1. 任务 A 进入 syscall，RSP = `syscall_stack_top`
2. A 调用 `sys_yield()` → 调度器把 A 挂起，保存 `task_a->sp = syscall_stack_top - 0x??`
3. 任务 B 进入 syscall，RSP = 同一个 `syscall_stack_top`
4. 调度器保存 `task_b->sp` = 相同区域
5. 恢复任务 A：跳到 B 写入的地址 → 跳到垃圾地址 0x86 → 崩溃

**修复**：改为使用每个任务独立的 TSS 内核栈：

```asm
# 新代码（syscall_wrapper.S）
syscall_entry:
    movq g_x86_tss_rsp0(%rip), %rsp   # 使用 TSS.RSP0（每任务独立）
```

并在任务切换时同步更新：

```c
// sched.c
g_x86_tss_rsp0 = (uint64_t)next_task->kernel_stack_top;
```

---

## 9. 关键设计原则总结

### 原则 1：C 异常处理函数中不能触发上下文切换

> 参见 `docs/INTERRUPT_CONTEXT_SWITCH.md`

中断/异常的 C 处理函数返回后，汇编会用 `iretq` 恢复寄存器状态。如果在 C 函数内切换任务，寄存器帧已失效，`iretq` 会跳到错误位置。

正确模式：C 函数设置标志 → 汇编在 `iretq` 前调用调度器。

### 原则 2：SYSCALL 入口必须使用每任务独立的内核栈

`SYSCALL` 指令不切换栈。如果多个任务共用一个 syscall 内核栈，任务在 syscall 中途被调度出去后，另一个任务会覆盖栈内容。

必须在 syscall 入口立即切换到当前任务的专属内核栈（通过 TSS.RSP0 间接获取）。

### 原则 3：TSS 描述符 base 必须是完整 64 位

高半部内核地址的高 32 位不为零，GDT TSS 描述符的 base 字段必须完整填写，否则 CPU 加载错误的 TSS 地址。

### 原则 4：所有中间级页表必须设置 PTE_USER

MMU 遍历 4 级页表时，**每一级**都要检查 U/S 位。只有叶子页 PTE 设置 USER 是不够的，中间级（PML4E/PDPTE/PDE）同样需要。

### 原则 5：抢占开启时机

定时器回调（`sched_tick`）注册时机过早会导致内核初始化被中断。建议：
- 创建所有必要的内核/用户任务后
- 再注册定时器抢占回调

### 原则 6：EFER.SCE 必须显式开启

x86_64 的 `SYSCALL` 指令默认不可用，必须在 `MSR_EFER` 中设置 `SCE`（bit 0）。IA32e 模式下长模式生效（LME/LMA）不会自动启用 SCE。

---

## 10. 涉及文件列表

| 文件 | 改动说明 |
|------|---------|
| `boot/x86_64/boot.S` | 添加 TSS GDT 槽；高半部重载 GDTR；启用 EFER.SCE |
| `boot/x86_64/tss.c` | 修复变量定义顺序；修复 base 截断；导出 `g_x86_tss_rsp0` |
| `boot/x86_64/tss.h` | 声明 `extern uint64_t g_x86_tss_rsp0` |
| `boot/x86_64/syscall_wrapper.S` | 替换静态共享栈为 `g_x86_tss_rsp0` 动态内核栈 |
| `boot/x86_64/exception.c` | 用户异常改为标记 + 延迟调度；启用 EFER.SCE；配置 SYSCALL MSR |
| `include/x86_64/exception.h` | 修正 `trap_frame_t` 字段顺序（与 exception.S push 顺序一致）|
| `include/x86_64/mm_vm.h` | 新增页表管理：`x86_walk_pt`、`mm_vm_map_pages`、`x86_perm_to_flags` |
| `kernel/task/x86_64/switch.S` | 实现 `task_trampoline_user`（iretq 进入 Ring3）|
| `kernel/task/sched.c` | 切换用户任务时更新 TSS.RSP0；调整 `sched_check_and_yield` 策略 |
| `kernel/main.c` | 将 `timer_set_tick_cb` 移到所有进程创建之后 |
| `apps/x86_64/user_test.S` | Ring3 测试程序（write + yield 循环）|
| `apps/x86_64/hello.S` | Ring3 第二个测试程序，与 user_test 交替运行 |

---

## 附录：最终验证输出（摘录）

```
[user_test] Hello from Ring3!
[hello] Hello from user hello!
[user_test] Hello from Ring3!
[hello] Hello from user hello!
...（稳定交替，无崩溃）
```

SYSCALL 调用次数达到 200+ 以上仍无异常，验证多任务 Ring3 支持完全稳定。

---

*文档版本：1.0 | 日期：2026-05-05 | 架构：x86_64 | 平台：QEMU*
