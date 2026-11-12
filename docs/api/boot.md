# boot — C API

[← 返回总览](index.md) · 25 个函数 · 说明覆盖率 100.0%

<a id="mod-boot-aarch64-exception.c"></a>

## boot/aarch64/exception.c

`boot/aarch64/exception.c` · 4 个函数 · 4 个有说明 · 127 行

<a id="fn-boot-aarch64-exception.c-21"></a>

### `handle_sync_exception`

```c
void handle_sync_exception(uint64_t *stack_pointer)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 ESR/FAR 打印 EL1 同步异常详情（含 DFSC 解码），以异常帧回溯调用栈，最后执行关机。

<details>
<summary>函数内备注（1 条）</summary>

**第 36 行**

> 打调用栈, 回答&quot;是谁调过来的&quot;。

> 用异常帧而不是当前栈：现在已经在异常处理程序里, 当前栈是处理程序
> 自己的, 看不到出错的那条路径。
> elr     = 出错的那条指令
> r[29]   = x29, 帧指针
> sp      = SAVE_REGS 之后 SP 被下移了 TRAP_FRAME_SIZE, 所以出错时的
> SP 就是帧地址加上这个大小

> 这里保持 platform_shutdown() 而不是换成 platform_panic()：本函数
> 原先就是关机, 改成死循环会改变对外行为(依赖&quot;QEMU 自行退出&quot;的脚本
> 会挂住)。先打栈, 再关机, 既保住现场又不动语义。

</details>

**位置**：`boot/aarch64/exception.c:21`

<a id="fn-boot-aarch64-exception.c-57"></a>

### `handle_el0_sync_exception`

```c
void handle_el0_sync_exception(uint64_t *stack_pointer)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

EC=0x15 转交 syscall_handler；用户取指/数据中止则打印现场、导出该进程 syscall 轨迹并投递 SIGSEGV，其余异常记录后关机。

<details>
<summary>函数内备注（6 条）</summary>

**第 66 行**

> EC == 0x15: SVC 指令(系统调用)

**第 68 行**

> 调用系统调用处理函数, 传入完整 trap_frame

**第 73 行**

> EC == 0x20: Instruction Abort from EL0
> EC == 0x24: Data Abort from EL0
> → 投递 SIGSEGV 给用户进程

**第 81 行**

> 崩溃现场：把这个 pid 最近走过的 syscall 打出来。
> 环形缓冲是常开的, 所以这里不需要事先开任何开关。

**第 91 行**

> 其他异常类型

**第 96 行**

> 停机

</details>

**位置**：`boot/aarch64/exception.c:57`

<a id="fn-boot-aarch64-exception.c-100"></a>

### `handle_irq_exception`

```c
void handle_irq_exception(uint64_t *stack_pointer)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先把当前 CPU 的 irq_depth 加一，调用 irq_dispatch_frame 分发中断，返回后再减一。

<details>
<summary>函数内备注（1 条）</summary>

**第 106 行**

> ack → handler → EOI/complete 全部由中断控制器后端完成
> (GICv2/GICv3 的 IAR/EOIR 时序差异不再泄漏到异常入口)。

</details>

**位置**：`boot/aarch64/exception.c:100`

<a id="fn-boot-aarch64-exception.c-115"></a>

### `invalid_exception`

```c
void invalid_exception(uint64_t *stack_pointer, uint64_t kind, uint64_t source)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印异常 kind 与 source，以异常帧为起点回溯调用栈，然后 platform_panic 停机。

<details>
<summary>函数内备注（2 条）</summary>

**第 117 行**

> 调用点(向量表里的 .Lvector_other 宏)是 SAVE_REGS 之后 `mov x0, sp`,
> 所以这里拿到的是完整的 trap frame, 可以当调用栈的起点用。

**第 122 行**

> 打完下一行就 panic —— panic 路径上的日志永远是 ERROR

</details>

**位置**：`boot/aarch64/exception.c:115`

<a id="mod-boot-riscv64-exception.c"></a>

## boot/riscv64/exception.c — RISC-V 64-bit S-mode 异常/中断 C 分发器

`boot/riscv64/exception.c` · 7 个函数 · 7 个有说明 · 253 行

提供：
exception_init()   — 设置 stvec, 配置异常入口
irq_install()      — 注册 scause 对应的中断处理函数
handle_exception() — 从 exception.S 调用, 分发陷阱

<a id="fn-boot-riscv64-exception.c-34"></a>

### `irq_install`

```c
void irq_install(int cause, irq_handler_t h)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

检查 cause 落在数组范围内后，把处理函数登记到对应中断表项。

**位置**：`boot/riscv64/exception.c:34`

<a id="fn-boot-riscv64-exception.c-47"></a>

### `riscv_dispatch_host_irq`

```c
void riscv_dispatch_host_irq(uint64_t cause, void *frame)
```

riscv_dispatch_host_irq — 给 VMM 的 guest 陷阱向量复用宿主中断分派

见 include/riscv64/exception.h 的说明。等价于 handle_exception 里
`interrupt_handlers[irq](frame_ptr)` 那一行, 只是不经过 trap_frame
的保存/恢复(现场已在 hext_trap_vector 里保存)。

**位置**：`boot/riscv64/exception.c:47`

<a id="fn-boot-riscv64-exception.c-63"></a>

### `rv_dump_trap_regs`

```c
static void rv_dump_trap_regs(trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

借助寄存器名字表，以每行四个的格式用 ERROR 级日志打印陷阱帧中的 32 个整数寄存器。

**位置**：`boot/riscv64/exception.c:63`

<a id="fn-boot-riscv64-exception.c-82"></a>

### `riscv_kernel_interrupt_enable`

```c
void riscv_kernel_interrupt_enable(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

置位 sstatus.SIE 以允许内核态中断。

**位置**：`boot/riscv64/exception.c:82`

<a id="fn-boot-riscv64-exception.c-87"></a>

### `riscv_kernel_interrupt_disable`

```c
void riscv_kernel_interrupt_disable(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清除 sstatus.SIE 以关闭内核态中断。

**位置**：`boot/riscv64/exception.c:87`

<a id="fn-boot-riscv64-exception.c-92"></a>

### `exception_init`

```c
void exception_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 stvec 指向直接模式的 trap_vector 并置位 sstatus.SUM 以允许访问用户页，保持 SIE 关闭并打印初始化信息。

<details>
<summary>函数内备注（5 条）</summary>

**第 94 行**

> trap_vector 在 exception.S 中定义(.align 4, 直接模式)

**第 97 行**

> 写入 stvec, bits[1:0] = 00 表示 Direct 模式

**第 100 行**

> 注意：不在此处使能 sstatus.SIE(内核态中断开关)。
> trap_vector 已能保存 S-&gt;S 的完整现场；这里仍保持 SIE=0,
> 由调度器和 timer 初始化完成后的显式开关决定何时允许内核态收中断。

**第 106 行**

> 使能 sstatus.SUM(Supervisor User Memory access)。
> 置 1 后 S 模式可直接读写带 PTE_U 的用户页面,
> 使 syscall 处理函数能直接解引用用户空间指针(如 write buf)。

**第 111 行（行尾）**

> bit 18 = SUM

</details>

**位置**：`boot/riscv64/exception.c:92`

<a id="fn-boot-riscv64-exception.c-119"></a>

### `handle_exception`

```c
void handle_exception(void *frame_ptr)
```

── handle_exception ───────────────────────────────────────────── *
由 exception.S 的 trap_vector 调用, 参数为 trap_frame_t *

<details>
<summary>函数内备注（18 条）</summary>

**第 125 行**

> 更新调试计数器

**第 130 行**

> 调试：打印异常基本信息

**第 132 行**

> KLOG_ERROR(&quot;[exception] sync: code=%llu pc=0x%lx stval=0x%lx sstatus=0x%lx\n&quot;,
> code, frame-&gt;sepc, frame-&gt;stval, frame-&gt;sstatus);

**第 139 行**

> ── 中断路径 ────────────────────────────────────────────

**第 146 行**

> ISR 上下文 + 逐中断一行, 必须采样

**第 147 行**

> KLOG_WARN_SAMPLE(

**第 148 行**

> &quot;[riscv irq] non-timer IRQ #%llu cause=%llu mode=%c sepc=0x%lx sstatus=0x%lx stval=0x%lx\n&quot;,

**第 149 行**

> (unsigned long long)++g_rv_non_timer_irq_count, irq, (frame-&gt;sstatus &amp; SSTATUS_SPP) ? &#x27;S&#x27; : &#x27;U&#x27;,

**第 150 行**

> frame-&gt;sepc, frame-&gt;sstatus, frame-&gt;stval);

**第 154 行**

> 内核态收到中断的计数。曾在这里打一行确认, 太吵 —— 需要时把下面
> 计数配合 KLOG_DEBUG 打开即可。

**第 167 行**

> ── 同步异常路径 ──────────────────────────────────────

**第 169 行**

> RISC-V ecall: sepc 指向 ecall 本身, 需手动前进到下一条

**第 172 行**

> 现场已完整保存在 frame 里，可以打开 SIE：长 syscall
> （文件系统/等待输入等）不再把整颗核的 IRQ 延迟拖到毫秒级。
> timer 中断只置 need_resched；用户进程的内核侧不会在任意
> C 代码点被抢占（trap_preempt_kernel_side 对
> is_user_process 返回 false）。返回 trap_vector 前恢复
> SIE=0，signal/调度检查和 sret 路径保持原有约定。

**第 189 行**

> 用户态 page fault → 投递 SIGSEGV 而不是 shutdown

**第 197 行**

> 崩溃现场：环形缓冲常开, 这里直接把该 pid 最近的 syscall 打出来

**第 205 行**

> 其他异常：打印简单信息后挂起

**第 222 行**

> 注意：这里必须是 0x600。写符号名 hstatus 会被汇编成
> 0x200(vsstatus), 印出来的 SPV/SPVP 就是别的寄存器的位 ——
> 详见 include/riscv64/hext.h 顶部的警告。

**第 236 行**

> 打调用栈, 回答&quot;是谁调过来的&quot;。

> 用异常帧而不是当前栈：现在已经在异常处理程序里, 当前栈是处理程序
> 自己的, 看不到出错的那条路径。
> sepc    = 出错的那条指令
> x[8]    = s0/fp, 帧指针
> x[2]    = sp, 出错时的栈指针(进了陷阱之后原值保存在帧里)

> 这里保持 platform_shutdown() 而不是换成 platform_panic()：本函数
> 原先就是关机, 改成死循环会改变对外行为。先打栈, 再关机, 既保住
> 现场又不动语义。

</details>

**位置**：`boot/riscv64/exception.c:119`

<a id="mod-boot-x86_64-exception.c"></a>

## boot/x86_64/exception.c — x86_64 IDT 初始化与中断分发

`boot/x86_64/exception.c` · 8 个函数 · 8 个有说明 · 309 行

提供：
exception_init()    — 建立 IDT, 加载 IDTR, 初始化 LAPIC, sti
handle_exception()  — 由 exception.S 调用；IRQ 向量交给
driver/irq 的 LAPIC 后端分发
idt_set_gate()      — 填写单个 IDT 门描述符

<a id="fn-boot-x86_64-exception.c-72"></a>

### `wrmsr`

```c
static inline void wrmsr(uint32_t msr, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 64 位值拆成低高两个 32 位，用 wrmsr 指令写入指定 MSR。

**位置**：`boot/x86_64/exception.c:72`

<a id="fn-boot-x86_64-exception.c-79"></a>

### `rdmsr`

```c
static inline uint64_t rdmsr(uint32_t msr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 rdmsr 指令读取指定 MSR 的低高 32 位并拼成 64 位返回。

**位置**：`boot/x86_64/exception.c:79`

<a id="fn-boot-x86_64-exception.c-88"></a>

### `idt_set_gate`

```c
void idt_set_gate(uint8_t vec, void (*handler)(void), uint16_t sel, uint8_t attr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把处理函数地址拆入 idt[vec] 的低中高偏移字段，并设置段选择子与属性，ist 与保留字段清零。

**位置**：`boot/x86_64/exception.c:88`

<a id="fn-boot-x86_64-exception.c-102"></a>

### `exception_init`

```c
void exception_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

安装 IDT 与 SYSCALL MSR，屏蔽 8259A 主从 PIC，初始化 LAPIC 后开中断并打印完成日志。

<details>
<summary>函数内备注（5 条）</summary>

**第 110 行**

> 屏蔽 8259A PIC(legacy), 我们使用 LAPIC。
> 默认情况下 BIOS/QEMU 将 IRQ0 映射到 INT 8(Double Fault 向量),
> 如果不屏蔽, sti 后 PIC timer 会触发 #DF。

**第 115 行（行尾）**

> master PIC — 屏蔽所有 IRQ

**第 116 行（行尾）**

> slave  PIC — 屏蔽所有 IRQ

**第 118 行**

> 初始化 LAPIC(软件使能 + SVR)

**第 121 行**

> 使能 CPU 中断

</details>

**位置**：`boot/x86_64/exception.c:102`

<a id="fn-boot-x86_64-exception.c-127"></a>

### `exception_init_secondary`

```c
void exception_init_secondary(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

为从核安装 IDT 与 SYSCALL MSR，并初始化本核 LAPIC。

**位置**：`boot/x86_64/exception.c:127`

<a id="fn-boot-x86_64-exception.c-134"></a>

### `exception_setup_idt`

```c
static void exception_setup_idt(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

为 0..47 号向量及 resched IPI、LAPIC spurious 安装中断门，最后设置并加载 IDTR。

<details>
<summary>函数内备注（4 条）</summary>

**第 136 行**

> 0-31：CPU 异常；32：LAPIC timer；33-47：外部 IRQ 向量
> （ISA IRQ n -&gt; IOAPIC GSI n -&gt; IDT 向量 0x20+n）。
> 32-47 的 stub 早已存在，只是以前没有把门装进 IDT。

**第 146 行**

> 安装向量 0xF0(跨核 resched IPI)

**第 149 行**

> 安装向量 255(LAPIC Spurious)

**第 152 行**

> 加载 IDTR

</details>

**位置**：`boot/x86_64/exception.c:134`

<a id="fn-boot-x86_64-exception.c-158"></a>

### `exception_setup_syscall`

```c
static void exception_setup_syscall(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

配置 STAR 段选择子、LSTAR 指向 syscall_entry、SFMASK 清除 IF 位，并打印入口地址。

<details>
<summary>函数内备注（6 条）</summary>

**第 160 行**

> 配置 SYSCALL/SYSRET MSR 寄存器(用户态支持)

**第 163 行（行尾）**

> syscall_wrapper.S

**第 165 行**

> MSR_STAR: 配置段选择子
> [31:0]   保留
> [47:32]  SYSCALL 内核 CS/SS (CS=值, SS=值+8)
> [63:48]  SYSRET 用户 CS/SS (CS=值+16, SS=值+8)

**第 173 行**

> MSR_LSTAR: SYSCALL 入口地址

**第 176 行**

> MSR_SFMASK: SYSCALL 时清除的 RFLAGS 位(关中断：IF=0x200)

**第 177 行（行尾）**

> 清除 IF(中断标志)

</details>

**位置**：`boot/x86_64/exception.c:158`

<a id="fn-boot-x86_64-exception.c-184"></a>

### `handle_exception`

```c
void handle_exception(void *frame_ptr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

中断交 irq_dispatch_frame；用户异常打印现场并按类型投递信号或置 DEAD 后请求重调度；内核异常回溯后 panic。

<details>
<summary>函数内备注（11 条）</summary>

**第 194 行**

> LAPIC 后端负责按向量查表、调用 handler 并写 EOI

**第 204 行**

> 对所有用户异常打印寄存器

**第 212 行**

> 打印 RIP 处的指令字节(用于所有异常)

**第 236 行**

> 用户态 page fault / GPF / 非法指令 → 投递信号而不是直接杀死

**第 252 行**

> 崩溃现场：环形缓冲常开, 这里直接把该 pid 最近的 syscall 打出来

**第 260 行**

> 关键：不要在异常 C 处理函数内直接 task_exit()/sched_schedule()。
> 调度应在 exception.S 的返回路径(sched_check_and_yield)统一执行,
> 避免破坏异常返回现场。

**第 270 行**

> 请求在异常返回路径执行重调度

**第 275 行**

> 未注册的 CPU 异常：打印详细信息

**第 286 行**

> Page Fault: 打印 CR2(出错地址)

**第 295 行**

> 用异常帧(不是处理程序自己的栈)打调用栈, 然后走统一的
> platform_panic() 停机 —— 它比裸 while(1) hlt 多做两件事：
> 关中断, 并跳过自己那次重复的 backtrace。

> 以前这里是 `while(1) hlt`：整机挂死在现场, 看不到&quot;是谁调过来的&quot;。

**第 305 行**

> 其余未注册中断静默忽略(包括 spurious)

</details>

**位置**：`boot/x86_64/exception.c:184`

<a id="mod-boot-x86_64-tss.c"></a>

## boot/x86_64/tss.c — x86_64 Task State Segment (TSS) 初始化

`boot/x86_64/tss.c` · 6 个函数 · 6 个有说明 · 149 行

TSS 在 x86_64 中用于：
1. 特权级切换时加载内核栈(RSP0/RSP1/RSP2)
2. 中断栈表(IST)用于关键异常处理

本实现设置基本的 TSS, 提供 RSP0(Ring 0 栈指针)。

<a id="fn-boot-x86_64-tss.c-48"></a>

### `tss_set_gdt_entry`

```c
static void tss_set_gdt_entry(uint64_t *gdt, uint64_t base, uint32_t limit)
```

SYSCALL 入口(boot/x86_64/syscall_wrapper.S)不经过 TSS, 直接读
cpu_t::kernel_rsp0。这里保证它与本核 TSS.RSP0 始终一致。

<details>
<summary>函数内备注（8 条）</summary>

**第 52 行**

> 低 8 字节：TSS 描述符(Type=0x9: Available TSS)

**第 53 行（行尾）**

> Limit 15:0

**第 54 行（行尾）**

> Base 15:0

**第 55 行（行尾）**

> Base 23:16

**第 56 行（行尾）**

> Type=9, P=1, DPL=0

**第 57 行（行尾）**

> Limit 19:16, G=0

**第 58 行（行尾）**

> Base 31:24

**第 60 行**

> 高 8 字节：Base 63:32 + 保留

</details>

**位置**：`boot/x86_64/tss.c:48`

<a id="fn-boot-x86_64-tss.c-65"></a>

### `x86_tss_init`

```c
void x86_tss_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 0 号 CPU 与默认内核栈为参数调用 x86_tss_init_cpu。

**位置**：`boot/x86_64/tss.c:65`

<a id="fn-boot-x86_64-tss.c-70"></a>

### `x86_tss_init_cpu`

```c
void x86_tss_init_cpu(uint32_t cpu_id, uint64_t rsp0)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 CPU 清零 TSS、设置 RSP0（缺省取内核栈顶）并禁用 IO 位图，在 GDT 写入描述符后 ltr 加载并打印日志。

<details>
<summary>函数内备注（6 条）</summary>

**第 77 行**

> 清零 TSS

**第 80 行**

> 设置 RSP0(Ring 0 栈顶)

**第 87 行**

> I/O 位图基址设为超出 TSS 末尾(禁用 I/O 权限检查)

**第 94 行**

> 获取当前 GDT 基址

**第 105 行**

> 在 GDT 中添加本 CPU 的 TSS 描述符(16 字节, 占两个 slot)

**第 108 行**

> 加载 TR(Task Register)指向本 CPU 的 TSS selector

</details>

**位置**：`boot/x86_64/tss.c:70`

<a id="fn-boot-x86_64-tss.c-121"></a>

### `x86_tss_sel_of_cpu`

```c
uint16_t x86_tss_sel_of_cpu(uint32_t cpu_id)
```

── 取本核 TSS 的选择子/基址 ────────────────────────────────────────

cpu_id 从 per-CPU 指针(%gs / TPIDR)读, 不从 TR 反推 —— 调用者
(VMCS 宿主区)恰恰是在 TR 可能已经不对的时候需要它, 用 TR 反推会自我
复制错误。选择子/基址的编码与 x86_tss_init_cpu() 里 ltr 的那一套必须一致。

**位置**：`boot/x86_64/tss.c:121`

<a id="fn-boot-x86_64-tss.c-128"></a>

### `x86_tss_current`

```c
void x86_tss_current(uint16_t *sel, uint64_t *base)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

输出当前 CPU 的 TSS 选择子与 TSS 基址，传入空指针则跳过对应输出。

**位置**：`boot/x86_64/tss.c:128`

<a id="fn-boot-x86_64-tss.c-141"></a>

### `x86_tss_set_rsp0`

```c
void x86_tss_set_rsp0(uint64_t rsp0)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

更新当前 CPU 的 TSS RSP0，并同步 per-CPU 的 kernel_rsp0 镜像供 SYSCALL 入口换栈。

<details>
<summary>函数内备注（1 条）</summary>

**第 147 行**

> 同步 per-CPU 镜像：SYSCALL 入口从这里换栈, 各核互不影响。

</details>

**位置**：`boot/x86_64/tss.c:141`
