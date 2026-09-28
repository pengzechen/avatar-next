# Avatar OS - AI Context

Claude 会自动读取这个文件来了解项目上下文。

## 项目概述

**Avatar OS** 是一个 64 位操作系统内核项目，专注于跨架构支持。

**核心特性**：
- **架构**：AArch64 (ARM 64-bit)、RISC-V 64-bit、x86_64
- **环境**：Freestanding（无标准库）
- **设计**：Linux 内核风格，架构抽象层
- **工具链**：musl 交叉编译

## 快速开始

### 第一步：拉 submodule

`lwext4` 和 `lwIP` 是 **git submodule**，而 `git clone` 默认**不拉**它们。
少了它们构建会死在这样一行：

```
include/vfs.h:14:10: fatal error: ext4.h: No such file or directory
```

这行指向的是**我们自己的**头文件，和真正的原因（submodule 是空的）看不出关系。
`make` 现在会在编译任何东西之前先拦一道并告诉你怎么办，但**建议直接这样 clone**：

```bash
git clone --recurse-submodules <url>
```

已经 clone 过了（或者 submodule 目录是空的）：

```bash
make submodules        # = git submodule update --init --recursive --force
```

> `--force` 不是摆设。有一种状态是"`.git` 指针文件在、`git submodule status` 也不带
> `-` 前缀、`git submodule update --init` 还 exit 0 —— 但工作树是空的（`.git/modules`
> 里有对象，index 里却是 staged 删除）"。这时**不带 `--force` 的 update 会静默地
> 什么都不做**，只有 `--force` 能把它拽回来。判据只能是看文件在不在，不能信 git 的
> 状态字段。

### 编译命令

```bash
# 基本编译
make PLATFORM=qemu-virt-aarch64
make PLATFORM=qemu-virt-riscv64
make PLATFORM=qemu-virt-x86_64

# 调试版本（日志+断言）
make PLATFORM=qemu-virt-aarch64 LOG=debug

# 发布版本（零日志开销）
make PLATFORM=qemu-virt-aarch64 LOG=none

# 关掉帧指针（backtrace 会退化成栈扫描，见 docs/basic/BACKTRACE.md）
make PLATFORM=qemu-virt-aarch64 FP=0

# 清理
make PLATFORM=qemu-virt-aarch64 clean

# 帮助
make help
```

> ⚠️ **构建不跟踪头文件依赖**：`-MMD` 生成的 `.d` 没有被 `-include`
> （见 `Makefile` 第 1188 行注释）。因此**改过任何 `.h`、或切换 `SMP=` / `LOG=` /
> 其它 CFLAGS 之后，必须先 `make PLATFORM=<p> clean` 再全量重编**，否则会用到
> 布局/配置不一致的旧 `.o`，产生看起来像运行期内存踩踏的"幽灵 bug"。
> 详见 `docs/bugfix/X86_64_SMP_SYSCALL_STACK_BUGFIX.md` 附录一。
>
> ⚠️ **变体标志（`GUEST_LINUX=` / `VMM_TEST=` / `STRING_TEST=` …）同理，而且更坑**：
> `make ... rootfs` / `make ... run-fs` 都**会连带重编内核**，用的却是**这一次
> 命令里给的变体**。所以「编好 `GUEST_LINUX=1` 的内核 → 再 `make ... rootfs`」
> 会把内核悄悄换成非 guest 变体（症状：guest 一个字符都不输出，宿主 busybox
> 直接跑起来）。**正确顺序是同变体：`make PLATFORM=<p> GUEST_LINUX=1 rootfs`
> 之后再 `... GUEST_LINUX=1 kernel`**；起 QEMU 前用
> `strings build/<p>/kernel_*.bin | grep -c 'GUEST_LINUX mode'`（应为 1）验一下产物。

### ⚠️ `test-guest-linux` 必须依赖 `$(ROOTFS_IMG)`

三个架构的 guest 镜像（`/guests/rv64/*`、`/guests/linux/*`、`/guests/x86_64/*`）
都是**打进 rootfs 镜像**、再由 guest_loader 从镜像里读出来的。少了这个依赖，
镜像不存在，QEMU 只报一句

```
-device loader,file=.../rootfs-<arch>.img: Cannot load specified image
```

**看起来像 guest 起不来，其实是文件压根没生成**，很容易查错方向
（见 `Makefile` 里 `test-guest-linux` 上方的注释）。

### 工具链

- AArch64: `aarch64-linux-musl-gcc`
- RISC-V: `riscv64-linux-musl-gcc`
- x86_64: `gcc`

## 项目结构

```
avatar/
├── Makefile              # 构建系统
├── CLAUDE.md             # 本文件（AI 上下文）
├── PROJECT_REFERENCE.md  # 详细参考文档
├── include/              # 头文件
│   ├── aarch64/         # AArch64 实现
│   ├── riscv64/         # RISC-V 实现
│   ├── x86_64/          # x86_64 实现
│   ├── types.h          # 基础类型
│   ├── arg.h            # 可变参数
│   ├── arch.h           # 架构检测
│   ├── barrier.h        # 内存屏障
│   ├── cache.h          # 缓存操作
│   ├── spinlock.h       # 自旋锁
│   ├── string.h         # 字符串操作
│   ├── klog.h           # 内核日志
│   ├── list.h           # 双向链表
│   ├── mmio.h           # 内存映射 I/O
│   └── assert.h         # 断言系统
├── lib/                  # 库实现
├── kernel/               # 内核代码
├── tests/                # 测试代码
└── docs/                 # 详细文档
```

## 详细文档索引

**Claude，当实现相关功能时，请先阅读对应的文档：**

### 核心系统

- **内存屏障**: `docs/basic/BARRIER.md` - barrier.h。**改设备寄存器 / 页表 / TLB 顺序相关代码前必读**：
  `barrier_data` 是 dmb 强度（只保证顺序），"写完必须确实到达"的场景要用
  `barrier_sync`（dsb 强度），换错会**削弱**同步
- **缓存操作**: `docs/basic/CACHE.md` - cache.h DMA 和 MMIO 缓存管理
- **自旋锁**: `docs/basic/SPINLOCK.md` - spinlock.h。中断状态存**调用点的局部变量**
  （`spin_lock_irqsave(&l, &flags)`），不要存进锁对象 —— `irq_flags` 那个字段
  在 SMP 下会被别的 CPU 覆盖

### 调试和日志

- **内核日志**: `docs/basic/KLOG.md` - klog.h 日志系统、模块控制，以及
  **「该用哪个级别」的规范**（新增/改日志等级前必读）。含：唯一判据、
  三条硬规则（ISR 不得未采样打日志 / INFO 不得进循环体 / 逐字段 dump 一律 DEBUG）、
  必须留在 INFO 的行清单、`_ONCE`/`_SAMPLE` 限流宏。
  > 改 `include/klog.h` 之后**必须 `make clean`** —— 头文件 mtime 不被跟踪，
  > 半新半旧的宏混编出来的日志「看起来完全合理」，但行数和采样都是错的。
- **断言系统**: `docs/basic/ASSERT.md` - assert.h 运行时和编译时断言
- **调用栈回溯**: `docs/basic/BACKTRACE.md` - panic / 内核态异常时打印带函数名的
  调用栈。`kernel/debug/backtrace.c` + `tools/gen_kallsyms.py`。
  改 `.kallsyms` 的段序、`FP=` 开关、或三个 `link.ld` 的段布局前必读 ——
  符号表是**两趟链接**生成的，段序一挪就会出现"镜像正常、但函数名整体错位"
  的假调用栈；构建里的 `gen_kallsyms.py --verify` 专门挡这个，失败了别绕过。
  此外三个架构的**帧布局不同**（RISC-V 的偏移是负的，且叶子函数不保存 `ra`）。
  想亲眼看效果：`make PLATFORM=<p> test-panic` —— 在 ELF 解析路径上故意 panic 一次
- **系统调用追踪**: `docs/basic/KLOG.md`「系统调用追踪」一节。用户态程序崩了先看这里 ——
  `kernel/syscall/trace.c` 有一个**常开**的每 CPU 环形缓冲（512 条/CPU），
  SIGSEGV 时自动 dump 该 pid 最近的 syscall，也可以随时 `cat /proc/syscalls`。
  **不需要 LOG=debug、不需要重编**。要实时盯就 `LOG=debug LOG_MODULES=syscall`。

### 数据结构和工具

- **双向链表**: `docs/basic/LIST_API.md` - list.h Linux 风格链表
- **字符串操作**: `docs/basic/STRING.md` - string.h 字符串和内存操作
- **MMIO**: `docs/basic/MMIO.md` - mmio.h 内存映射 I/O

### 架构特定

- **AArch64**: `docs/arch/aarch64/NEON_USAGE.md` - NEON 优化
- **AArch64 FP/SIMD 上下文**: `docs/arch/aarch64/FP_SIMD_CONTEXT.md` -
  改 `trap_frame_t` 布局、`boot/aarch64/exception.S`、`kernel/task/aarch64/switch.S`
  或 `kernel/task/switch.h` 里三处伪帧前必读（含向量表 128 字节槽位预算的坑）
- **x86_64 SYSCALL / per-CPU 栈**: `docs/bugfix/X86_64_SMP_SYSCALL_STACK_BUGFIX.md` -
  改 `cpu_t` 布局或 SYSCALL 入口前必读（含 `%gs:` 偏移的 static_assert 约束与构建陷阱）
- **x86_64 时间基准 / TSC 标定**: `docs/bugfix/X86_64_TIMEBASE_TSC_CALIBRATION_FIX.md` -
  动 `driver/irq/lapic.c` 的频率标定前必读（PIT 只能用 0x43/0x42，**不要用 port 0x61**）

### 中断与上下文切换

**遇到任何中断 / 抢占 / 上下文切换问题，先读这三篇文档，不要凭空推断。**

- **中断屏蔽接口**: `docs/basic/INTERRUPT_MASKING.md` - C 代码统一用
  `include/<arch>/exception_impl.h` 的 `arch_irq_*`（由 exception.h 暴露），
  **不许再写内联汇编**；`.S` 除外

- **中断开关策略**: `docs/INTERRUPT_CONTROL_COMPARISON.md` - AArch64/RISC-V 中断开关的核心约定。关键：中断策略按“EL1 里运行的是谁”区分——EL0 任务的内核侧（syscall/异常）关中断，独立内核线程（含 idle）开中断、可被抢占。含 `task_trampoline` vs `task_trampoline_user` 的分野与代码检查清单。
- **延迟调度机制**: `docs/INTERRUPT_CONTEXT_SWITCH.md` - 为什么不能在 ISR 内切换任务，而是只置 `need_resched`、在异常返回路径（`sched_check_and_yield`）才切换。

### 虚拟化（VMM / guest）

- **RISC-V guest Linux**: `docs/vmm/RISCV64_GUEST_LINUX.md` - H-extension 下跑
  guest Linux 的整个链路（G-stage / sret 进出 / vPLIC / 16550A / SBI / DTB 布局），
  以及 7 条**移植时真踩过**的坑。**动 `kernel/vmm/riscv64/*`、
  `include/riscv64/hext.h`、`include/guest_loader.h` 之前必读** ——
  其中第 1 条（binutils 把 hypervisor CSR 符号名静默映射到 VS 级编号，
  编译全绿、运行期才炸）尤其反直觉。该文档 §7 还登记了一个**尚未定性的
  间歇性 guest 用户态 SIGSEGV**。
  **§8 是「同内核多 VM」**（每 VM 一份 G-stage + 按需分页，与 aarch64 同构）。
  动 G-stage 之前必读那里的小节 8.4 —— 第 1 条（**RISC-V 的 PTE 存的是
  PPN，`pte & ~0xFFF` 拿到的是 `pa>>2`**，与 ARM 的 LPAE 正相反）会让宿主
  从错误的物理页取指、并满屏 `PMM: invalid free address`，两个症状都不指向
  "翻译错了"。
- **x86_64 guest Linux（已跑通到 shell）**: `docs/vmm/X86_GUEST_LINUX.md` ——
  VMX/EPT + vLAPIC + IO-APIC/PIT/PIC 桩 + PIO 串口，guest Linux 6.2.15
  已能启动到**交互式 busybox shell**（`make PLATFORM=qemu-virt-x86_64
  LOG=warn SMP=1 test-guest-linux`，stdin 直通 guest 串口）。
  该文档 §9 是「最后一公里」的 6 个 bug —— **每个都症状在 guest 侧、
  根因在 VMM 侧，而且表现都像另一个问题**（MMIO 解码拿 guest RIP 当物理地址、
  CPUID 0x15/0x16 报 0 掉进 PIT 死循环、STI 影子挡住中断注入、
  `MSR_FS_BASE` 漏写 `GUEST_BASE_FS` 导致 userspace 段错误…）。
  改 `kernel/vmm/x86_64/*` 前必读。
  **§11 是「同内核多 VM」**（每 VM 一份 EPT + 按需分页，与 aarch64/riscv 同构）。
  动 EPT 或 `guest_loader` 的宿主侧写内存路径之前必读那里的小节 11.3 ——
  两条**都不报错、只是数据错位**的坑：`guest_loader_gpa_ptr()` 在新映射那条路上
  漏了页内偏移（页对齐顺序装载时看不出来，MP 表这种非页对齐的一次性写入就中招），
  以及别对它的返回值做跨页指针算术（identity 时代成立，按需分页之后不成立）。
  **回归门禁（三条路各一个，别只跑一个）**：
  - **直启 + SMP=1**：`tools/boot_regress.sh [次数]` —— 连续启动 N 次、每次都要出
    `~ #`（§9.9 那个间歇性卡死就是它抓出来的，修复后 500/500 通过）。
  - **helper + 多核**：`tools/vmm_helper_regress.sh <arch> [次数] [smp] [--restart]` ——
    `run-net` 起宿主 shell 再敲 `/bin/vmm-run`；`--restart` 额外验证「Ctrl+] 停掉
    之后还能再启动」。**SMP>1 下的坑几乎都只在这条路上出现**（VM setup 跑在 helper
    的核上、vCPU 钉在另一颗核上），根因清单见 `docs/bugfix/SMP_HELPER_MODE_BUGFIX.md`
    与 `docs/vmm/X86_GUEST_LINUX.md` §9.10~§9.12。
  - **多 VM**：`tools/vmm_multivm_regress.sh <arch> [smp]` —— `vmm-run` 起 vm1 →
    `Ctrl+[`（0x1b）detach → `vmm-run -n` 新建 vm2，要求两个 guest 各自跑到
    `uname`、宿主侧留下两个 `vcpu0 task created`。改 VM 池 / 设备 per-VM 化 /
    stage-2（G-stage）之后必须跑这条 —— 单 VM 的门禁**测不出**这类回归。
- **两种运行模式、`/dev/vmm` 协议、Ctrl+] / Ctrl+[ 语义**: `docs/vmm/GUEST_CONSOLE.md`
- **裸跑 guest 作基线对照**: `docs/vmm/GUEST_NATIVE_QEMU.md`

**使用方式**: 用 Read tool 读取文档，了解 API 和最佳实践后再实现。

## 已实现模块

### 基础设施
- ✅ 类型系统 (types.h) - 整数、指针、对齐宏
- ✅ 可变参数 (arg.h) - va_list 支持
- ✅ 架构检测 (arch.h) - 编译时架构识别

### 内存和同步
- ✅ 内存屏障 (barrier.h) - 编译器/CPU 屏障
- ✅ 缓存操作 (cache.h) - clean/invalidate
- ✅ 自旋锁 (spinlock.h) - 包含 IRQ 变体

### 工具库
- ✅ 字符串操作 (string.h) - strlen, strcmp, memcpy 等
- ✅ 双向链表 (list.h) - Linux 内核风格
- ✅ 内存映射 I/O (mmio.h) - 带屏障的设备访问

### 调试支持
- ✅ 内核日志 (klog.h) - 级别和模块控制
- ✅ 断言系统 (assert.h) - 运行时和编译时

## 关键设计原则

### 1. 内联优先
**所有头文件函数必须是 `static inline`**，避免链接错误。

```c
// ✅ 正确
static inline void operation(void) { }

// ❌ 错误
extern void operation(void);
```

### 2. 架构抽象模式
统一接口 + 架构特化实现：

```c
/* module.h */
#include "arch.h"
static inline void operation(void);

#if defined(__aarch64__)
    #include "aarch64/module_impl.h"
#elif defined(__x86_64__)
    #include "x86_64/module_impl.h"
#elif defined(__riscv)
    #include "riscv64/module_impl.h"
#endif
```

### 3. 类型安全
使用 `__typeof__` 实现类型安全的宏：

```c
#define MIN(a, b) __extension__ ({            \
    __typeof__(a) _a = (a);                   \
    __typeof__(b) _b = (b);                   \
    _a < _b ? _a : _b;                        \
})
```

### 4. 集成日志
使用 klog 进行错误和调试输出。**等级怎么选见 `docs/basic/KLOG.md`「该用哪个级别」**，
一句话判据：*这条信息，在一次正常且成功的启动里，值不值得占用人类一行注意力？*

```c
/* 值得 -> INFO；排障才要的细节 -> DEBUG + 模块标签 */
KLOG_ERROR("Critical error: %s\n", msg);
KLOG_INFO("sensor up: %d Hz\n", rate);
KLOG_DEBUG("Value: %d\n", value);
KLOG_UART("UART init\n");                       /* == KLOG_MODULE_DEBUG(LOG_MODULE_UART, ...) */

/* 高频且持续（ISR / 每包 / 每目录项）—— 必须限流，不许裸 KLOG_* */
KLOG_WARN_SAMPLE("[plic] unhandled irq=%u hit#%u\n", irq, _klog_seq_);
KLOG_WARN_ONCE("[syscall] open('%s') not implemented\n", path);
```

> 所有宏都**不**自动补 `\n`；漏写会和下一行粘在一起。

### 5. 内存安全
同步和 I/O 操作集成内存屏障：

```c
// MMIO 自动包含屏障
mmio_write32(addr, value);  // 包含释放屏障

// Spinlock 使用 acquire/release
spin_lock(&lock);  // 获取屏障
spin_unlock(&lock);  // 释放屏障
```

## 给 Claude 的重要提示

### 实现新功能时

1. **先阅读文档** - 用 Read tool 读取 `docs/` 中的相关文档
2. **遵循模式** - 查看现有模块的实现方式
3. **三个架构** - 必须支持 AArch64、RISC-V、x86_64
4. **使用 klog** - 集成日志输出
5. **测试编译** - 测试所有三个架构

### 常见错误避免

```c
// ❌ 不要这样做
extern void helper(void);  // 多重定义错误

// ✅ 应该这样
static inline void helper(void) { }

// ❌ 不要在头文件中实现复杂函数
// ✅ 复杂函数放在 lib/ 中，声明用 extern
```

### 架构特定实现

参考现有实现：
- `include/aarch64/*_impl.h` - AArch64 内联汇编
- `include/riscv64/*_impl.h` - RISC-V 内联汇编
- `include/x86_64/*_impl.h` - x86_64 内联汇编

### 平台层依赖

需要平台层提供的函数：

```c
// UART 输出（klog 使用）
void uart_putchar(char c);
void uart_putstr(const char *str);

// Panic 处理（assert 使用）
void platform_panic(void);
```

## 未来工作方向

可能需要实现的模块（优先级未排序）：

- 内存管理（MMU、页表、分配器）
- 中断处理（IDT、IRQ 控制器）
- 任务调度（进程、线程）
- 设备驱动框架
- 系统调用接口
- 文件系统
- 网络栈

## 相关资源

- **详细参考**: `PROJECT_REFERENCE.md` - 完整的技术文档
- **项目文档**: `docs/` - 各模块详细说明

---

**版本**: 1.0
**更新**: 2025-01-02
**项目**: Avatar OS


**规则**：今后所有跨平台 C 代码中的架构判断，一律 `#include "arch.h"` 后使用 `#if ARCH_X86_64` / `#if ARCH_AARCH64` / `#if ARCH_RISCV64`。

**规则（用户指针）**：syscall 里拿到来自 `regs[]` 的指针，**一律经
`copy_to_user_bytes()` / `copy_from_user_bytes()` / `copy_string_from_user()` 访问，
禁止裸解引用**（如 `*(int *)regs[1] = ...`、`memset((void *)regs[1], ...)`）。
这些接口内部用 `user_range_ok()` 校验；裸解引用时用户传一个非法地址
（例如 `(void*)-1`）就会让内核态写到坏地址 → #PF → 异常处理的 `while(1) hlt`
→ **整机挂死**。这是 LTP 实测出来的：
`kernel/syscall/fs/ioctl.c`、`kernel/syscall/fs/pty.c`、`kernel/syscall/core/proc_lifecycle.c`
（`wait_handler`）、`kernel/syscall/syscall.c`（`getrlimit`/`getrusage`）都踩过一遍。
新增 syscall 时请顺带检查这一条 —— 详见 `tests/ltp/testcases.list` 文件头。

已进行更改。


启动带文件系统内核
# 1. 编译内核
make PLATFORM=qemu-virt-x86_64 kernel

# 2. 创建 ext4 rootfs 镜像（需要 Host 安装 e2fsprogs）
make PLATFORM=qemu-virt-x86_64 rootfs

# 3. 向镜像写入内容（可选）
sudo mount -o loop build/rootfs.img /mnt/tmp
sudo mkdir -p /mnt/tmp/etc
sudo echo "Avatar OS" | sudo tee /mnt/tmp/etc/hostname
sudo umount /mnt/tmp

# 4. 启动 QEMU（自动用 -device loader 加载镜像到对应物理地址）
make PLATFORM=qemu-virt-x86_64 run-fs
