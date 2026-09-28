# 内核日志系统（klog）

## 快速上手

```bash
# 默认：级别 info，模块全开
make PLATFORM=qemu-virt-aarch64 run

# 定位中断问题：只要 GIC 的调试日志，别的模块闭嘴
make PLATFORM=qemu-virt-aarch64 run LOG=debug LOG_MODULES=gic

# 发布版：所有日志宏在编译期被抹除
make PLATFORM=qemu-virt-aarch64 kernel LOG=none
```

启动时会打印一行生效配置，排查"为什么我的日志不见了"先看它：

```
[INFO][C0] lib/klog.c:226: [klog] level=3 modules=init,task,driver,uart,timer,mm,fs,net,smp,gic,generic
```

## 两种控制手段

| | 编译期 | 运行期 | 作用 |
|---|---|---|---|
| **级别** | `make LOG=<level>` | `set_log_level()` | 决定 DEBUG/TRACE 的门槛 |
| **模块** | `make LOG_MODULES=<list>` | `log_set_modules_by_name()` | 在 DEBUG/TRACE 上再筛一层 |

### 级别：`LOG=`（编译期）

| 级别 | 值 | 颜色 |
|------|---|------|
| `none` | 0 | — |
| `error` | 1 | 红 |
| `warn` | 2 | 黄 |
| `info` | 3（默认） | 绿 |
| `debug` | 4 | 蓝 |
| `trace` | 5 | 灰 |

**只有 `LOG=none` 是编译期抹除**：宏体整个变成 `do {} while (0)`，参数不求值、格式串也不进 `.rodata`。

其余级别（error/warn/info/debug/trace）只是把 `g_log_level` 的**初值**设成该级别，调用点仍然带着"读 `g_log_level` + 比较 + 分支"——实测默认 info 构建下每个 `KLOG_DEBUG` 调用点都是：

```
ldr w0, [g_log_level]; cmp w0, #0x3; b.hi <skip>
```

两个直接推论：

- 运行期 `set_log_level(LOG_LEVEL_TRACE)` 能把 info 构建里的 TRACE 日志**打开**（代码一直都在）；
- `KLOG_ERROR` 没有任何级别检查，运行期**关不掉**（要关只能 `LOG=none`）。

> 早先文档写的"`LOG=error` 时其他日志会被编译器优化掉"是错的 —— 那是把 `LOG=none` 的行为安到了别的级别上。

### 模块：`LOG_MODULES=`（编译期白名单）

```bash
make ... LOG=debug LOG_MODULES=uart,gic      # 只放行这两个模块的 DEBUG/TRACE
make ... LOG=debug LOG_MODULES=all           # 全部（等于不传）
make ... LOG=debug LOG_MODULES=none          # 全不放行
```

模块掩码只对 `KLOG_DEBUG` / `KLOG_TRACE` / `KLOG_MODULE_*` 起作用（ERROR/WARN/INFO 不受模块限制）。

**白名单语义**：`LOG_MODULES=` 一经传入，掩码就是白名单——
- 打了模块标签的日志（`KLOG_MODULE_DEBUG(LOG_MODULE_GIC, ...)` 及 `KLOG_UART()` 这类简写）按位放行；
- **未打标签**的 `KLOG_DEBUG` / `KLOG_TRACE` 归入 `LOG_MODULE_GENERIC`，不在名单里就不输出；
- 不传 `LOG_MODULES=` 时掩码是全 1，因此旧行为完全不变。

实测（`LOG=debug LOG_MODULES=gic`）：8 条 GIC 模块日志输出、普通 DEBUG 一条不出；把 `generic` 加进名单后普通 DEBUG 恢复 11 条。

模块名拼错会在启动时报错并点名（同时掩码为 0，即什么都不输出）：

```
[ERROR][C0] lib/klog.c:220: [klog] LOG_MODULES 有 1 个无法识别的模块名："gci"
```

### 运行期等价 API

```c
#include "klog.h"

/* 按名字设置（与编译期 LOG_MODULES= 同一套名字） */
log_set_modules_by_name("uart,gic");     /* 返回无法识别的名字个数 */

/* 按位操作 */
log_set_modules(LOG_MODULE_UART | LOG_MODULE_GIC);
log_add_module(LOG_MODULE_TIMER);
log_remove_module(LOG_MODULE_FS);

/* 查询：把当前启用的模块名写进缓冲区 */
char names[128];
log_modules_to_string(names, sizeof(names));

/* 级别 */
set_log_level(LOG_LEVEL_DEBUG);
get_log_level();
```

## 日志宏

```c
#include "klog.h"

/* 所有宏都**不**自动补换行，调用方自己写 "\n" */
KLOG_ERROR("Critical error: %s\n", msg);      /* 总是显示（LOG=none 除外）*/
KLOG_WARN("value out of range: %d\n", v);
KLOG_INFO("System initialized, %d MB\n", mem_size);
KLOG_DEBUG("Allocated %p size=%d\n", ptr, size);   /* 受 LOG_MODULES 白名单约束 */
KLOG_TRACE("entry: %s\n", __func__);               /* 同上 */

/* 模块日志：只在 DEBUG/TRACE 且该模块位放行时输出 */
KLOG_MODULE_DEBUG(LOG_MODULE_GIC, "GIC: PMR=0x%x\n", pmr);
KLOG_MODULE_TRACE(LOG_MODULE_TASK, "switch %d -> %d\n", from, to);
KLOG_GIC("GIC: PMR=0x%x\n", pmr);                   /* == KLOG_MODULE_DEBUG(LOG_MODULE_GIC, ...) */

/* 一次性 / 抽样：见下节「限流与一次性日志」 */
KLOG_WARN_ONCE("[syscall] open('%s') not implemented\n", path);
KLOG_WARN_SAMPLE("[PLIC] unhandled irq=%u hit#%u\n", irq, _klog_seq_);
```

`KLOG_DEBUG` 与 `KLOG_MODULE_DEBUG` 的区别只有一个：后者额外带模块位，因此能被 `LOG_MODULES=` 单独选中。
`KLOG_INIT/TASK/DRIVER/UART/TIMER/MM/FS/NET/SMP/GIC` 都是 `KLOG_MODULE_DEBUG` 的单行简写。

### 内置模块

| 名字（`LOG_MODULES=` 里用） | 宏 | 说明 |
|---|---|---|
| `init` | `LOG_MODULE_INIT` | 初始化 |
| `task` | `LOG_MODULE_TASK` | 任务/调度 |
| `driver` | `LOG_MODULE_DRIVER` | 驱动 |
| `uart` | `LOG_MODULE_UART` | UART（`logger_*` 里 `logger_debug` 不带位） |
| `timer` | `LOG_MODULE_TIMER` | 定时器 |
| `mm` | `LOG_MODULE_MM` | 内存管理 |
| `fs` | `LOG_MODULE_FS` | 文件系统 |
| `net` | `LOG_MODULE_NET` | 网络（virtio-net 的每包日志挂在这里） |
| `smp` | `LOG_MODULE_SMP` | 多核 |
| `gic` | `LOG_MODULE_GIC` | GIC（`KLOG_GIC` / `logger_gic_debug`） |
| `generic` | `LOG_MODULE_GENERIC` | 未打标签的 DEBUG/TRACE |
| `syscall` | `LOG_MODULE_SYSCALL` | 系统调用逐次追踪（`KLOG_SYSCALL`）。见下节「系统调用追踪」 |

加新模块要同时改两处：`include/klog.h` 的位定义、`lib/klog.c` 的名字表。

### 限流与一次性日志

给**高频且持续**的场合用（ISR、每包、每目录项）。每个级别都有 `_ONCE` / `_SAMPLE` 两个变体，
模块版是 `KLOG_MODULE_DEBUG_ONCE/SAMPLE(module, ...)`：

| 宏 | 语义 |
|---|---|
| `KLOG_*_ONCE` | 该调用点**一辈子只说一次**。用于"缺哪个系统调用""未知系统调用号"这类——它们是真问题，但会被高频调用刷屏 |
| `KLOG_*_SAMPLE` | 前 `KLOG_SAMPLE_FIRST`(8) 次全打，之后只打 2 的幂，超过 `KLOG_SAMPLE_CAP`(4096) 彻底静音 |

**关键性质：单个调用点的输出有上界。** `FIRST=8 / CAP=4096` 时最多 `8 + 9 = 17` 行，
无论事件实际发生多少次 —— 中断风暴下这是唯一能保住串口的性质。
CAP 之后想知道"现在到底多少次了"，看调用点自己维护的计数器
（参照 `driver/uart/uart_pl011.c` 的 `tx_dropped` / `pl011_tx_dropped()`）。

`_SAMPLE` 宏的格式参数里可以引用 `_klog_seq_`（当前命中序号），
但它**只在宏的参数表内有效**。

**为什么不做基于时钟的窗口限流**：`g_system_ticks` 在 `timer_init()` 之前恒为 0，
而 `timer_init()` 晚于 pmm/fs/vmm 初始化。窗口限流会认为"所有事件都发生在 tick 0"，
打印一次后吞掉整个前 timer 启动阶段 —— 恰好是最需要日志的那一段。所以只做计数采样。

**两条放置约束**：

1. **不要放在被多个 TU include 的头文件的函数里** —— 每个 TU 各拿一份 `static` 计数器，
   `_ONCE` 会退化成"每 TU 一次"，`_SAMPLE` 的预算成倍。
2. riscv64 的 `.bss.boot` 不在 `__bss_start/__bss_end` 内、**不被清零**
   （`boot/riscv64/link.ld`），所以这些宏不得用于 `.text.boot`/`.bss.boot` 里的代码。

状态是**块作用域 `static`**，落在 `.bss`，靠启动时的 `clear_bss` 保证初值 0；
SMP 安全靠 `__atomic_*`（RELAXED），不碰 `g_klog_lock`。

## 该用哪个级别

> 这一节是**规范**，不是建议。新增日志、或改一条日志的等级之前，先读它。

### 唯一判据

> **这条信息，在一次正常且成功的启动里，值不值得占用人类一行注意力？**

- 值得 → `INFO`（如果这次运行其实并不正常，继续往上走 `WARN` / `ERROR`）
- 不值得，但排障的时候会想要 → `DEBUG`
- 不值得，而且按对象/按迭代反复发生 → `DEBUG` + 模块标签，或 `TRACE`

### 各级别规则

| 级别 | 规则 | 说明 |
|---|---|---|
| **ERROR** | 子系统**无法完成**被要求的事，或数据/正确性已丢失 | 见下方"鉴别句" |
| **WARN** | 系统继续跑了，但**降级或非预期**：走了回退、拒绝了东西、能力缺失、吸收了潜在 bug | 可采样处 |
| **INFO** | **状态迁移**或**生效配置**，每次迁移 / 每次启动 / 每个用户可见事件**至多一行** | 见下方两个取消资格 |
| **DEBUG** | 排障时想要的一切非状态迁移信息 | **降级 INFO 的默认归宿**；应与模块标签同用 |
| **TRACE** | 量级由输入无界驱动，或只受 CPU 速度限制 | 逐符号重定位、逐级页表遍历 |

**ERROR 的鉴别句**：*健康系统会不会打出这行？会 → 就不是 ERROR。*
最容易被误标成 ERROR 的四类：成功读到的结构体特性位、预期内的 `ENOSYS`、
例行轮询超时、平台能力陈述（"这个平台没接这个设备"）。

**INFO 的两个硬性取消资格**，命中任意一条就不是 INFO：

1. **频率由输入驱动** —— 逐包、逐系统调用、逐目录项、逐页、逐 IRQ、逐循环迭代；
2. **没有状态改变** —— "即将做 X"、"文件大小是 N"、寄存器快照、banner 拆成好几行。

### 三条硬规则

1. **ISR 里不得出现未采样的任何等级日志。** ISR 打日志等于在关中断下忙等 UART，
   而逐中断日志是自己造中断风暴。确需观测就用 `_SAMPLE` 宏（见下节）。
2. **INFO 及以上不得出现在循环体内**，除非行程数是小的编译期常量。
   `for (i = 0; i < count; i++) KLOG_INFO(...)` **永远是 bug** ——
   即使当前 `count` 通常等于 1，下一个调用者会传 4096。
3. **逐字段结构体 dump / 寄存器快照 / 多行 banner** → 降到 DEBUG，或合并成一行 INFO。

### 已经定过分歧的场合

| 场合 | 判定 | 依据 |
|---|---|---|
| 成功读到的超级块特性位 | `INFO` | 一次性、生效配置 |
| *不支持的*特性位掩码 | 非零 → `WARN`；为零 → `DEBUG` | 非零意味着在静默忽略磁盘语义 |
| 未实现的系统调用返回失败（`ENOSYS`） | `WARN` + `_ONCE` | 返回失败是**正确行为**；"缺哪些"正是 bring-up 想要的清单 |
| 未知的系统调用号 | `ERROR` + `_ONCE` | 连分类都没有；busybox 探测会刷屏 |
| panic 路径上的日志 | `ERROR` | 打完就 `platform_panic()` |
| "没接这个设备 / 这个平台不支持" | `WARN` 或 `INFO` | 平台事实，不是错误 |

### 必须留在 INFO 的行

下列行有**仓库外的依赖**（文档配方、脚本、人眼判据），降级会**静默**破坏它们：

| 行 | 位置 | 依赖方 |
|---|---|---|
| `[vmmdev] boot: starting guest` | `kernel/fs/pseudofs/vmm_dev.c` | `docs/vmm/GUEST_NATIVE_QEMU.md` 用 `grep` 轮询它测启动耗时 |
| `[klog] level=%u modules=%s` | `lib/klog.c` | 本文档开头"日志不见了先看这行" |
| `=== STRING TEST: PASS (N cases) ===` | `tests/string_test.c` | `make test-string` 的成功判据（见 `docs/basic/STRING.md`） |
| VMM 测例 transcript | `tests/vmm_test.c`、`kernel/vmm/{aarch64/el2_run.c,x86_64/vmx.c,riscv64/hext_run.c}` | `readme.md` 用 `LOG=info` 跑 `test-vmm`，这些行就是通过证据 |
| `=== Launching busybox shell ===` 等启动进度标记 | `kernel/main.c` | "到底起来没有"的事实信号 |

> VMM 那几行是**最容易改错的地方**：它们不能降级，只能改成采样 ——
> 原来的 `iter % N == 0` 节流保留 INFO 语义，但把输出量压成有界。

## 系统调用追踪（strace 式）

用户态程序崩了、想按 syscall 调用顺序定位时用这个，**不要**靠 `LOG=debug` 逐条打日志。
逐条打有三个致命短板：要全量重编、直写 UART 刷过去就没了、崩在 syscall 中间时只剩一个
配不上对的孤立入口行。

`kernel/syscall/trace.c` 提供两条路：

| 手段 | 用途 | 需要重编吗 |
|---|---|---|
| **环形缓冲**（常开） | 崩溃后回看"刚才走了哪些 syscall" | **不需要** |
| `KLOG_SYSCALL` 流式输出 | 实时盯着看 | 需要（`LOG=debug LOG_MODULES=syscall`）|

### 环形缓冲

每 CPU 一个 512 条的环，**每次 syscall 都记**（入口写参数、出口回填返回值），
不碰 UART、不加锁 —— syscall 路径本来就关中断且不迁移，各 CPU 只写自己的环。
一条 syscall 只占一个槽位，所以 dump 出来是一行一条，和 strace 一致。

**为什么必须常开**：崩溃是事后才知道的，没法"补录"。关掉省下的那点开销，
换来的是崩的时候什么都没有。

**什么时候能看到它**：

1. **进程异常死亡时自动 dump** —— SIGSEGV 等致命信号打出该 pid 最近的
   `SYSCALL_TRACE_DUMP_MAX`（默认 32）条。零操作，崩溃现场直接出现在屏幕上。
2. **随时手动看** —— `cat /proc/syscalls`，最多 128 条，不受等级影响。

输出长这样（路径是内联存在记录里的，所以直接看得见文件名）：

```
[WARN] [SYSCALL] pid=5 崩溃前的最近 3 条系统调用：
[WARN] [SYSCALL] pid=5 t=5680ms set_tid_address (0x220e0, 0x1, 0x22060) = 0x5 (5)
[WARN] [SYSCALL] pid=5 t=5680ms openat "/etc/hostname" (0xffffffffffffff9c, 0x110b8, 0x20000) = 0xfffffffffffffffe (-2)
[WARN] [SYSCALL] pid=5 t=5680ms write (0x1, 0x110c8, 0xd) = 0xd (13)
```

几个用起来要知道的点：

- **`= ? (没有返回)`**：execve / exit 这类不会返回的 syscall 会一直保持哨兵值。
  这是**有用信息**，不是记录丢了 —— 它告诉你"走到这里就没再回来"。
- **只记前 3 个参数**。够看清 `openat` 的 dirfd/flags、`read` 的 fd/count、`brk` 的地址；
  更靠后的参数要自己对着用户态代码看。
- **路径型 syscall 的路径**（openat/newfstatat/readlinkat/execve/…）内联存 32 字节，
  经 `copy_string_from_user()` 抓取 —— 来自 `regs[]` 的用户指针**禁止裸解引用**
  （用户传非法地址会让内核态读到坏地址 → #PF → 整机挂死）。
- 内存开销：512 条 × 88B × 8 CPU ≈ **360KB** 的 `.bss`。嵌入式目标可在平台配置里
  把 `SYSCALL_TRACE_DEPTH` 调小。

### 流式输出（实时）

```bash
make PLATFORM=... run LOG=debug LOG_MODULES=syscall
```

只放行 syscall 模块的 DEBUG —— 也就是全部系统调用的逐次记录，别的模块闭嘴。
这条路不常开（要重编），但适合"看着它跑"的场景。它和环形缓冲共用同一份
syscall 名字表。

## 输出格式

```
[INFO][C0] kernel/main.c:200: === Avatar OS Kernel aarch64 build ... ===
[ERROR][C0] lib/klog.c:219: [klog] LOG_MODULES 有 1 个无法识别的模块名："gci"
[DEBUG][C0] [MOD] driver/irq/gicv2.c:116: GIC: Detected 288 IRQ lines for virtualization
```

`[级别][C<cpuid>] 文件:行号: 消息`，模块日志多一个 `[MOD]` 标记。级别带 ANSI 颜色。

## 注意事项

1. **单行上限 511 字节**：`kvprintf` 用 512 字节栈缓冲，超长行会被**截断**（`my_vsnprintf` 按 C99 语义返回"本该写多长"，`kvprintf` 会 clamp 到缓冲长度）。
2. **换行**：所有宏都不自动补 `"\n"`，忘记写就会和下一行连在一起。
3. **性能**：`DEBUG`/`TRACE` 调用点在 info 构建下仍在（一次全局读 + 比较 + 分支），只是不执行；要彻底去掉用 `LOG=none`。
4. **线程/中断安全**：klog 自己用 IRQ-safe 自旋锁按**整条消息**加锁（格式化在锁外做），所以 ISR 里打日志是安全的。
5. **断言不受日志级别影响**：`assert()` 失败时用 `kprintf` 直接输出（不是 `KLOG_ERROR`），因此 `LOG=none` 构建下**也会**打印 —— 以前它会被静默掉，只剩一个没有任何输出的 panic。
6. **不要在被 klog 依赖的路径里打日志**：UART 驱动的"发送缓冲满、重试超时"分支里有 `logger_warn`，而它是在 klog 持锁逐字符输出的过程中被调用的 —— 一旦该分支可达就是同核自死锁。目前 UART 的缓冲/中断发送路径整体未启用（`uart_initialized` 永远是 false，初始化赋值被注释掉了），所以不可达；**启用那条路径前必须先解决这个重入**。同理，PL011 的 TX 缓冲锁用的是非 IRQ-safe 的 `spin_lock`。
7. **`LOG_MODULES=` 消费得太晚，挡不住最早的几行**：`klog_init()` 由 `kernel/main.c` 调用，而 x86_64 / riscv64 在它**之前**就有架构初始化行打出（x86 的 `Initializing IDT + LAPIC...`、riscv 的 `Initializing exception handler...`）。`log_set_modules_by_name()` 是**整体赋值**（白名单而非合并），所以那几行是在默认全 1 掩码下输出的，不受 `LOG_MODULES=` 约束。目前已知并接受；要修得把级别/掩码初始化提前到 `platform_init()` 之前。
8. **改了 `include/klog.h` 之后必须 `make clean`**：Makefile 的 `_CFG_SIG` 只跟踪 `GIC=/SMP=/LOG=/LOG_MODULES=`，**不跟踪头文件 mtime**（`-MMD` 的 `.d` 从未被 `-include`）。后果不是编译失败，而是部分 TU 用旧宏、部分用新宏 —— 输出看起来完全合理，只是行数和采样都是错的。验证这类改动时要按行数比对，所以这个坑特别容易骗过验收。

## 测试

`tests/klog_test.c` 里有级别/模块/动态切换的用例，但 `run_klog_tests()` **目前没有任何调用者**（和之前 `tests/string_test.c` 一样是死代码）。要跑起来需要在 `kernel_main` 里显式调用；字符串那套自检已经用 `make ... test-string` 接通了（见 `docs/basic/STRING.md`），klog 这套还没接。
