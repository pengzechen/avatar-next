# kernel — C API

[← 返回总览](index.md) · 1008 个函数 · 说明覆盖率 95.0%

<a id="mod-kernel-debug-backtrace.c"></a>

## kernel/debug/backtrace.c — 内核调用栈回溯

`kernel/debug/backtrace.c` · 27 个函数 · 27 个有说明 · 754 行

实现在这里, 用法和设计取舍见 debug/backtrace.h 与 docs/basic/BACKTRACE.md。
下面只补三个实现上的选择：

1. 帧指针链优先, 栈扫描兜底。帧指针链准确, 代价低, 但只要有一环不
可靠(汇编写的异常存根不建帧, FP=0 的构建, 栈被踩烂)就整条断掉。
所以链走不通时改用栈扫描 —— 会有误报, 但比"什么都没有"强。

2. 每一步都验证, 而不是走到底再验证。栈被踩烂时帧指针可能指向任意
内存, 照着读就是读野地址。所以每一步都要求：地址是内核地址, 8 字节
对齐, 落在当前栈区间内, 返回地址落在 .text 里, 且帧指针严格单调递增
(栈向下增长)。任何一条不满足就停, 宁可少报几帧。

3. panic 路径绕开 klog 的锁。kvprintf() 取 g_klog_lock, 而 panic 完全
可能发生在别的 CPU 正持锁的时候 —— 那样就在锁上自旋到死, 一个字都打
不出来。见 backtrace_panic_enter()。

<a id="fn-kernel-debug-backtrace.c-104"></a>

### `bt_get_sp`

```c
static uintptr_t bt_get_sp(void)
```

bt_get_sp - 读当前栈指针

为什么不直接用 __builtin_frame_address(0)：那是帧指针, 不是栈指针。
FP=1 时它比真正的 sp 高出一整个帧；FP=0 时它压根不是帧指针(rbp 已经被
当通用寄存器用了), 拿去当扫描起点会偏出去。

读 sp 没有现成的 wrapper(exception_impl.h 里那些是管中断屏蔽的),
就地内联汇编 —— 和 boot/x86_64/exception.c 读 CR2/CR3 是同一个做法。

**位置**：`kernel/debug/backtrace.c:104`

<a id="fn-kernel-debug-backtrace.c-132"></a>

### `backtrace_panic_enter`

```c
void backtrace_panic_enter(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把全局 panic 标志置真，使后续回溯改走无锁 UART 且只打印一次。

**位置**：`kernel/debug/backtrace.c:132`

<a id="fn-kernel-debug-backtrace.c-140"></a>

### `bt_puts`

```c
static void bt_puts(const char *s)
```

整段调用栈拼成一条消息再发：走 kprintf 时是一条消息一次加锁, 多核下
这段输出不会和别的日志交错。

**位置**：`kernel/debug/backtrace.c:140`

<a id="fn-kernel-debug-backtrace.c-151"></a>

### `backtrace_lookup`

```c
const char *backtrace_lookup(uintptr_t addr, uintptr_t *offset)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 kallsyms 地址表二分查找不大于 addr 的最近符号，回填偏移并返回符号名；空表或过小返回 NULL。

<details>
<summary>函数内备注（2 条）</summary>

**第 158 行**

> 空表(stage-1 的桩)直接落空；短路保证不会读 addrs[0]

**第 162 行**

> 二分：找满足 addrs[i] &lt;= addr 的最大 i

</details>

**位置**：`kernel/debug/backtrace.c:151`

<a id="fn-kernel-debug-backtrace.c-183"></a>

### `bt_is_text`

```c
static bool bt_is_text(uintptr_t a)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断地址是否落在内核代码段 [_stext, _etext) 内。

**位置**：`kernel/debug/backtrace.c:183`

<a id="fn-kernel-debug-backtrace.c-188"></a>

### `bt_fill`

```c
static void bt_fill(bt_entry_t *e, uintptr_t addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写入回溯项的地址，并用 backtrace_lookup 查出符号名与偏移。

**位置**：`kernel/debug/backtrace.c:188`

<a id="fn-kernel-debug-backtrace.c-228"></a>

### `bt_read_frame`

```c
static bool bt_read_frame(uintptr_t fp, uintptr_t *prev_fp, uintptr_t *ra)
```

bt_read_frame - 从帧记录 fp 取出「上一帧的 fp」和「返回地址」

返回 true 表示 ra 有效；false 表示这是个叶子帧, 本帧没把返回地址
存到栈上(只有 riscv64 会这样, 见下)。

注意： 三个架构的帧布局不一样, 这是本文件最容易写错的一处。实测 GCC
-O2 -fno-omit-frame-pointer 生成：

x86_64 rbp            aarch64 x29            riscv64 s0(x8)
push %rbp             stp x29,x30,[sp,-16]!  addi sp,sp,-N
mov  %rsp,%rbp        mov x29,sp             sd   ra,(N-8)(sp)
sd   s0,(N-16)(sp)
addi s0,sp,N
[fp+0] = 上帧 fp      [x29+0] = 上帧 x29     [s0-16] = 上帧 s0
[fp+8] = 返回地址     [x29+8] = 返回地址     [s0-8]  = 返回地址

RISC-V 的偏移是负的 —— s0 指向帧顶而不是帧底。照抄 x86 的
[fp]/[fp+8] 会读到帧内的局部变量, 症状是调用栈里冒出一堆随机函数。

RISC-V 还有个额外的坑：叶子函数不保存 ra(压根没调用过谁, ra 一直
活着, 没必要存), 于是 GCC 把它省下来的槽位给了 s0：

bt_temp_b(非叶子): addi sp,sp,-16; sd s0,0(sp); sd ra,8(sp); addi s0,sp,16
bt_temp_c(叶子)  : addi sp,sp,-16; sd s0,8(sp);               addi s0,sp,16
^^^^^^^ 只有 s0, 没有 ra

也就是说 [s0-8] 里装的到底是 ra 还是 s0, 得看这个函数是不是叶子。
判据是"它是不是代码地址"。叶子帧的返回地址栈上根本没有 —— 只活在 ra
寄存器里, 异常路径可以从 trap frame 的 x[1] 捞回来(见
backtrace_dump_fault 的 ra 参数)。

<details>
<summary>函数内备注（1 条）</summary>

**第 239 行（行尾）**

> 叶子帧：[s0-8] 里是上一帧的 s0

</details>

**位置**：`kernel/debug/backtrace.c:228`

<a id="fn-kernel-debug-backtrace.c-274"></a>

### `bt_stack_bounds`

```c
static void bt_stack_bounds(uintptr_t sp, uintptr_t *lo, uintptr_t *hi)
```

bt_stack_bounds - 当前内核栈的 [lo, hi)

拿到栈区间才能判断一个帧指针是不是"落在栈里", 这是防读野地址的
主要手段。

注意： 用任务栈之前必须先用 sp 验一下。task_init() 里 sched_init() 一执行,
task_current() 就返回 idle 了, 但 CPU 此时还跑在 boot 栈上, 要等
task_switch_to_idle_stack() 才真正切过去。那段窗口里如果照用 idle 的
栈边界, 当前栈上的所有帧都会被判成"不在栈里", 一个都走不出来。

三个分支依次是：任务的栈(且 sp 确实在里面)→ 启动栈 → 都不认识时以 sp
为中心开一个窗口(至少让首帧自身能过检查, 剩下的靠单调性和 .text 检查兜)。

**位置**：`kernel/debug/backtrace.c:274`

<a id="fn-kernel-debug-backtrace.c-305"></a>

### `bt_walk_fp`

```c
static int bt_walk_fp(uintptr_t fp, uintptr_t sp, uintptr_t seed_ra, bt_entry_t *out, int max)
```

bt_walk_fp - 顺着帧指针链向上走

fp 是起始帧的帧指针, 返回的第一帧是 fp 所属函数的调用者里的位置
([fp+8]/[fp-8] 存的是返回地址, 也就是 call 的下一条指令)。

<details>
<summary>函数内备注（4 条）</summary>

**第 314 行**

> 每一步都验证, 不信任链上的任何一环：
> 栈被踩烂时 fp 可能指向任意内存, 照着读就是读野地址。

> 注意： 边界余量按架构给(BT_FRAME_PAD_*), 不能一律写成 fp &lt; lo+16 ——
> 首帧的 fp 往往就等于调用者给的 sp, 多减 16 会让第一帧直接被
> 拒掉, 整条帧指针链一次都走不通(实测踩过：所有调用栈都退化成
> 栈扫描)。

**第 332 行（行尾）**

> 不是返回地址：链到头了, 或者这帧是假的

**第 335 行**

> 叶子帧(只有 riscv64 会这样, 见 bt_read_frame)：本帧没把返回
> 地址存到栈上, 它只活在 ra 寄存器里。第一帧还能靠异常路径递进来
> 的 ra 补上；再往上就没有来源了 —— 跳过这一层的 PC, 但继续
> 沿帧链走, 不能因为少一格就把后面的调用者全丢掉。

**第 344 行（行尾）**

> 栈向下增长, 往上走必须严格递增

</details>

**位置**：`kernel/debug/backtrace.c:305`

<a id="fn-kernel-debug-backtrace.c-365"></a>

### `bt_scan_stack`

```c
static int bt_scan_stack(uintptr_t sp, uintptr_t lo, uintptr_t hi, bt_entry_t *out, int n, int max)
```

bt_scan_stack - 栈扫描兜底

从 sp 往栈顶逐字扫描, 把落在 .text 里的值当作返回地址。帧指针链走不通
时才用(汇编存根, FP=0 的构建, 栈被踩烂)。

代价是会误报：局部变量, 被调用者保存的旧 PC, 任何碰巧等于代码地址
的常量都会被当成一帧。所以它只是补充, 不替代帧指针链。

<details>
<summary>函数内备注（1 条）</summary>

**第 377 行（行尾）**

> 同一个值的多份副本, 压掉

</details>

**位置**：`kernel/debug/backtrace.c:365`

<a id="fn-kernel-debug-backtrace.c-408"></a>

### `bt_collect`

```c
static int bt_collect(uintptr_t pc, uintptr_t fp, uintptr_t sp, uintptr_t ra, int max)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先记 pc 帧，再沿帧指针链展开；链不足两帧时改用栈扫描并从已有帧之后追加。

<details>
<summary>函数内备注（2 条）</summary>

**第 420 行**

> 出故障的那条指令算第 0 帧。不在 .text 里也照报 —— 跳到野地址正是
> 最需要看清的情况, 此时 name 为 NULL, 只显示地址。

**第 429 行**

> 帧指针链只走出一帧(通常只有 pc 那一帧)说明它没起作用 —— 换成栈
> 扫描。走通了就不用扫描：扫描的误报会污染一份本来干净的调用栈。

> 注意： 扫描要从 keep 开始追加, 不能从 0 开始重写。异常路径下 out[0]
> 是那条出错的指令(pc), 是整份调用栈里最该看见的一帧；从 0 重写会
> 把它吃掉, 症状是调用栈看着挺正常, 唯独少了出错的那一条
> (riscv64 实测：sepc 明明打出来了, backtrace 的 #0 却是它的调用者)。

</details>

**位置**：`kernel/debug/backtrace.c:408`

<a id="fn-kernel-debug-backtrace.c-455"></a>

### `bt_copy_out`

```c
static int bt_copy_out(bt_entry_t *out, int max, int n)
```

把 g_bt_scratch 里的结果交给调用者。
只在 bt_collect 返回之后才写 out —— 见 g_bt_scratch 的说明：
扫描期间往栈上的 out 写会自反馈。

**位置**：`kernel/debug/backtrace.c:455`

<a id="fn-kernel-debug-backtrace.c-468"></a>

### `backtrace_collect`

```c
__attribute__((noinline)) int backtrace_collect(bt_entry_t *out, int max)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取本函数帧地址，从当前栈回溯并把结果拷贝到调用者缓冲区。

<details>
<summary>函数内备注（2 条）</summary>

**第 470 行**

> 帧指针必须在这里取(而不是被内联出去的某个 helper 里)：取到的是
> 本函数的帧记录, 往上走一步正好落到调用者那一帧, 于是 out[0] 是
> 调用者的位置 —— 不会把 backtrace 自己的内部帧混进调用者的栈里。
> noinline 是给&quot;有人哪天开了 LTO&quot;兜底的。

**第 478 行**

> ra 传 0：从&quot;当前栈&quot;开始时, 起始帧一定是 backtrace 自己(它调用过
> 东西, 不是叶子), 帧记录里必然存着返回地址, 用不上这个提示。

</details>

**位置**：`kernel/debug/backtrace.c:468`

<a id="fn-kernel-debug-backtrace.c-484"></a>

### `backtrace_collect_from`

```c
int backtrace_collect_from(uintptr_t pc, uintptr_t fp, uintptr_t sp, uintptr_t ra, bt_entry_t *out, int max)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按调用者给定的 pc/fp/sp/ra 采集回溯并拷贝到输出缓冲区。

**位置**：`kernel/debug/backtrace.c:484`

<a id="fn-kernel-debug-backtrace.c-495"></a>

### `bt_appendf`

```c
static int bt_appendf(char *buf, int size, int pos, const char *fmt, ...)
```

追加一段格式化文本, 返回新的 pos。
my_vsnprintf 是 C99 语义(截断时返回"本该多长"), 所以必须自己 clamp ——
否则 pos 会一路越界, 最后写穿缓冲区。

**位置**：`kernel/debug/backtrace.c:495`

<a id="fn-kernel-debug-backtrace.c-515"></a>

### `bt_render_entry`

```c
static int bt_render_entry(char *buf, int size, int pos, int idx, const bt_entry_t *e)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

格式化一条回溯项：有符号名时输出地址、符号与偏移，否则标 <no symbol>。

**位置**：`kernel/debug/backtrace.c:515`

<a id="fn-kernel-debug-backtrace.c-524"></a>

### `bt_render_common`

```c
static int bt_render_common(char *buf, int size, uintptr_t pc, uintptr_t fp, uintptr_t sp, uintptr_t ra, const char *banner)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

采集并渲染完整回溯文本（标题、扫描提示、逐帧），缓冲不足时在尾部标 truncated，返回写入长度。

<details>
<summary>函数内备注（3 条）</summary>

**第 529 行**

> 下限 32 是给下面&quot;截断标记&quot;直接写尾部留余量(size-24 不能下溢)

**第 533 行**

> 直接读 g_bt_scratch, 不再往本函数栈上开一份 e[] —— 那正是扫描
> 自反馈的源头, 见 g_bt_scratch 的说明。

**第 550 行**

> 截断必须明确标出来 —— 否则看起来像&quot;调用栈只有这么深&quot;。
> 直接覆盖尾部 24 字节(size&gt;=32 已保证不下溢)。

</details>

**位置**：`kernel/debug/backtrace.c:524`

<a id="fn-kernel-debug-backtrace.c-558"></a>

### `bt_banner`

```c
static void bt_banner(char *buf, int size, const char *what)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写 BACKTRACE 标题，含 CPU 号与当前任务名/ID，无任务时标 boot。

<details>
<summary>函数内备注（1 条）</summary>

**第 565 行**

> task_init() 之前的 boot 上下文没有任务可报

</details>

**位置**：`kernel/debug/backtrace.c:558`

<a id="fn-kernel-debug-backtrace.c-576"></a>

### `backtrace_render`

```c
int backtrace_render(char *buf, int size)
```

供 /proc/backtrace 用：渲染"读这个文件的那一刻"当前任务的调用栈。

最上面几帧必然是 backtrace_read → vfs → syscall 入口那一串 —— 那条路径
就是这个任务此刻的内核栈, 不是噪声。Linux 的 /proc/PID/stack 也一样。

**位置**：`kernel/debug/backtrace.c:576`

<a id="fn-kernel-debug-backtrace.c-584"></a>

### `backtrace_print`

```c
void backtrace_print(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

重入或 panic 已打印过则直接返回，否则采集当前栈渲染输出到 klog 并清除 busy 标志。

<details>
<summary>函数内备注（1 条）</summary>

**第 589 行**

> 这两道闸和 backtrace_print_from 里的一样, 缺一不可：
> g_bt_busy       —— 防重入
> g_bt_panic_done —— 异常路径已经用异常帧打过一份正确的栈了,
> 这里再来一份只会是错的：平台 panic 是由异常处理
> 程序调用的, 从 platform_panic 往上走要穿过没有帧
> 指针的异常存根, 捞到的是栈上残留的旧返回地址
> (实测打出了一个跟现场毫无关系的 ramblk_init)。
> 两份栈里错的那份更容易把人带偏, 所以宁可不打。

</details>

**位置**：`kernel/debug/backtrace.c:584`

<a id="fn-kernel-debug-backtrace.c-613"></a>

### `backtrace_print_from`

```c
void backtrace_print_from(uintptr_t pc, uintptr_t fp, uintptr_t sp, uintptr_t ra)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在非重入且 panic 未打印过时渲染带 (fault) 标题的回溯，panic 场景置只打一次标志。

<details>
<summary>函数内备注（1 条）</summary>

**第 618 行**

> 重入和&quot;只打一次&quot;两道闸：
> g_bt_busy       —— dump 本身又崩了(符号表坏了, 栈彻底烂了),
> 别无限递归。
> g_bt_panic_done —— panic 路径只打一次；panic 之后内核已经不可信,
> 第二次的栈基本都是噪声, 还会把第一次刷出屏幕。

</details>

**位置**：`kernel/debug/backtrace.c:613`

<a id="fn-kernel-debug-backtrace.c-642"></a>

### `backtrace_dump_fault`

```c
void backtrace_dump_fault(uintptr_t pc, uintptr_t fp, uintptr_t sp, uintptr_t ra)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先摘掉 klog 全局锁并置 panic 标志，再用异常帧打印一次回溯。

<details>
<summary>函数内备注（2 条）</summary>

**第 644 行（行尾）**

> 先把 klog 的全局锁摘掉, 否则可能卡在锁上

**第 645 行（行尾）**

> 改走无锁 UART, 并只打一次

</details>

**位置**：`kernel/debug/backtrace.c:642`

<a id="fn-kernel-debug-backtrace.c-656"></a>

### `bt_selftest_c`

```c
static __attribute__((noinline)) int bt_selftest_c(bt_entry_t *out, int max)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

自测链最内层：采集回溯，把帧数写入全局 sink 并返回。

**位置**：`kernel/debug/backtrace.c:656`

<a id="fn-kernel-debug-backtrace.c-664"></a>

### `bt_selftest_b`

```c
static __attribute__((noinline)) int bt_selftest_b(bt_entry_t *out, int max)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

自测链中层：调用内层自测函数并记录其返回帧数。

**位置**：`kernel/debug/backtrace.c:664`

<a id="fn-kernel-debug-backtrace.c-672"></a>

### `bt_selftest_a`

```c
static __attribute__((noinline)) int bt_selftest_a(bt_entry_t *out, int max)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

自测链外层：调用中层自测函数并记录帧数。

**位置**：`kernel/debug/backtrace.c:672`

<a id="fn-kernel-debug-backtrace.c-681"></a>

### `bt_name_contains`

```c
static bool bt_name_contains(const char *hay, const char *needle)
```

freestanding 下不依赖 strstr 是否可用(string.h 里没有它)

**位置**：`kernel/debug/backtrace.c:681`

<a id="fn-kernel-debug-backtrace.c-699"></a>

### `backtrace_selftest`

```c
void backtrace_selftest(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

沿自测函数采集回溯并校验前三帧符号名，不匹配时用 kprintf 输出失败原因与排查建议。

<details>
<summary>函数内备注（3 条）</summary>

**第 701 行**

> 期望的链路：backtrace_collect 的调用者开始往上是
> out[0] = bt_selftest_c, out[1] = bt_selftest_b, out[2] = bt_selftest_a
> 用&quot;包含&quot;而不是全等匹配：GCC 可能给 static 函数加上 .constprop.0 /
> .isra.0 后缀。

**第 725 行**

> 正常时 LOG=info 下完全静默, 只在 LOG=debug 下留一份完整的
> 调用栈 —— 它同时是&quot;展开对不对&quot;和&quot;符号化对不对&quot;的现场证据,
> 比一句 ok 有用得多。

**第 743 行**

> 这里必须用 kprintf 而不是 KLOG_ERROR：LOG=none 会把 KLOG_ERROR 编成
> 空语句, 而&quot;调用栈是假的&quot;这件事任何构建配置下都得看得见 —— 假的栈比
> 打不出来更危险, 它会把人带到完全错误的方向去。理由同 include/libc/assert.h。

</details>

**位置**：`kernel/debug/backtrace.c:699`

<a id="mod-kernel-debug-klog_async.c"></a>

## kernel/debug/klog_async.c — 异步内核日志输出

`kernel/debug/klog_async.c` · 4 个函数 · 4 个有说明 · 160 行

背景：klog_write() 原来整段持 spin_lock_irqsave 逐字符写 UART。R3 的
IRQ-off trace 显示这是 x86/riscv 上最大的关中断区间（毫秒级）。

现在：启动 klogd 内核线程后，klog_write 只把消息 memcpy 进环形缓冲
（持 IRQ-off 几微秒），UART 输出由 klogd 在中断打开的状态下完成。
panic 路径和环形缓冲满时仍退回同步输出，保证崩溃现场不丢。

<a id="fn-kernel-debug-klog_async.c-36"></a>

### `klog_async_enqueue`

```c
bool klog_async_enqueue(const char *buf, size_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

关中断加锁把字节写入日志环，空间不足返回 false 交回同步输出，成功时唤醒 klogd。

<details>
<summary>函数内备注（2 条）</summary>

**第 44 行**

> 拿到锁后复查：panic flush 会在置位&quot;异步关闭&quot;之后再来抢这把锁。
> 不复查的话，已经在等锁的生产者会在 flush 认领完范围之后又写进环形
> 缓冲，那批字节就永远没人消费了。

**第 57 行（行尾）**

> 满：退回同步输出

</details>

**位置**：`kernel/debug/klog_async.c:36`

<a id="fn-kernel-debug-klog_async.c-90"></a>

### `klog_async_panic_flush`

```c
void klog_async_panic_flush(void)
```

klog_async_panic_flush - panic 时把已入环的日志同步刷出

背景：klogd 是普通内核线程，进入 panic 后不会再被调度。如果崩溃前最后
几条日志还躺在环形缓冲里，它们永远不会被打印出来 —— 而那段恰恰是最想
看到的现场。这里在 klog_panic_begin() 里主动消费一次。

并发处理：
- 先关异步入口(ready=false)，并在 enqueue 拿锁后复查，保证之后不会再
有生产者往环里写；
- 尽力拿环形锁：拿到就独占认领 tail..head；拿不到说明有核正卡在临界区，
直接按快照裸读。panic 下宁可重复输出，也不能丢日志；
- 认领(把 tail 推到 head)之后释放锁再写 UART，避免持锁做几十 KB 的
慢速串口输出。此时生产者已被 ready=false 挡住，klogd 看到 head==tail
也不会再消费，环内容不会被覆盖。

<details>
<summary>函数内备注（1 条）</summary>

**第 102 行（行尾）**

> 认领整个已入队范围

</details>

**位置**：`kernel/debug/klog_async.c:90`

<a id="fn-kernel-debug-klog_async.c-111"></a>

### `klogd_fn`

```c
static void klogd_fn(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

klogd 循环：环空时置当前任务 BLOCKED 并调度等待，有日志时按 128 字节块取出后在锁外写 UART。

**位置**：`kernel/debug/klog_async.c:111`

<a id="fn-kernel-debug-klog_async.c-146"></a>

### `klog_async_start`

```c
void klog_async_start(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未启动时创建 RT 优先级 1 的 klogd 内核线程，成功后置异步就绪标志。

<details>
<summary>函数内备注（1 条）</summary>

**第 151 行**

> 在此之前（含本行自身的日志）仍然同步写，保证启动期不丢日志。
> klogd 用最低 RT 优先级（FIFO 1）：普通优先级下被唤醒要等 tick，
> 日志输出和 wake_max 都会出现 ~10ms 的假延迟；FIFO 1 高于 NORMAL、
> 低于所有真正的 RT worker，只在有日志时短暂抢占。

</details>

**位置**：`kernel/debug/klog_async.c:146`

<a id="mod-kernel-main.c"></a>

## kernel/main.c — Kernel main entry point

`kernel/main.c` · 5 个函数 · 5 个有说明 · 346 行

Called from architecture-specific boot code

<a id="fn-kernel-main.c-54"></a>

### `test_boot_active`

```c
__attribute__((weak, noinline)) bool test_boot_active(void)
```

默认启动模式：正常 busybox。测试变体（GUEST_LINUX=1 / VMM_TEST=1）链接的
tests/unit-test/{guest_test,vcpu_thread_test}.c 提供同名强定义，链接时自动覆盖这里的 weak 版本。

必须 noinline：weak 函数可被 interpose，但 GCC 在 -O2 下仍可能把同 TU 的
调用内联成常量；内联后就看不到测试变体的强定义了。

**位置**：`kernel/main.c:54`

<a id="fn-kernel-main.c-59"></a>

### `test_boot_skip_net`

```c
__attribute__((weak, noinline)) bool test_boot_skip_net(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

弱符号默认实现，恒返回 false，表示启动时不跳过网络初始化。

**位置**：`kernel/main.c:59`

<a id="fn-kernel-main.c-64"></a>

### `test_boot_run`

```c
__attribute__((weak, noinline)) void test_boot_run(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

弱符号空实现，默认不做任何事，供测试变体覆盖以运行自定义启动流程。

**位置**：`kernel/main.c:64`

<a id="fn-kernel-main.c-67"></a>

### `load_busybox`

```c
static void load_busybox(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先让出 CPU 100 次等文件系统就绪，再以 frame=NULL 原地加载 /busybox 的 sh 进入用户态，失败打日志后退出任务。

<details>
<summary>函数内备注（4 条）</summary>

**第 71 行**

> 等待文件系统初始化

**第 79 行**

> 以 busybox 的 sh applet 进入交互 shell

**第 83 行**

> 调用 ELF 加载器执行 /busybox。frame=NULL：这是内核线程，原地 exec
> 成功后由 arch_enter_user() 直接进入用户态，本任务(以及它的 PID)
> 就是 shell 本身，不再有 wrapper 残留在 ps 里。

**第 96 行**

> 任务完成

</details>

**位置**：`kernel/main.c:67`

<a id="fn-kernel-main.c-101"></a>

### `kernel_main`

```c
void kernel_main(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按序完成平台、UART、异常、日志、PMM、驱动、文件系统、任务与 AP 初始化，最后按测试或普通模式创建启动任务。

<details>
<summary>函数内备注（37 条）</summary>

**第 103 行**

> 平台配置是纯数据(来自 build/&lt;platform&gt;/platform_static.c), 不碰硬件;
> UART 驱动要用它查 MMIO 基址, 所以必须先跑。

**第 109 行**

> UART 必须早于任何 KLOG —— 下面架构初始化里就有日志。此时异常向量/
> 中断控制器可以尚未初始化, 本步只做轮询模式的串口配置。

**第 116 行**

> Keep NEON enabled for AArch64 code paths that require it.

**第 120 行**

> Initialize IDT and LAPIC

**第 123 行**

> TSS(特权级切换用)在 cpu_init_bsp() 之后初始化：TSS.RSP0 要同步进
> cpu_t::kernel_rsp0, 而 cpu_init_bsp() 会把 g_cpus[0] 整个 memset 掉。
> IDT 不使用 IST(ist=0), 所以这里晚一点设置不影响异常处理。

**第 130 行**

> 必须在 fs_init() 之前设置 stvec, 否则 ext4_mount 中的任何
> CPU 异常都会落到 M-mode (OpenSBI), 导致 hart 被重置

**第 137 行**

> 记录 boot 建立的内核根页表(x86_64 CR3 / RISC-V satp)。此刻仍跑在
> 内核地址空间上；之后用户页表构造、调度切回内核任务都依赖它。
> AArch64 的内核映射在 TTBR1, 没有这个抽象, 记录到的是 0。

**第 145 行**

> mmu.S 建立的 1GiB 内核映射必须与 RV_KERNEL_MAP_LIST 完全一致；
> 缺一条就 panic 在启动期, 而不是等 syscall 期间 Store PF。

**第 151 行**

> 应用编译期模块白名单(Makefile 的 LOG_MODULES=)并打印生效配置。
> 必须在 UART 可用之后, 任何模块日志之前。

**第 157 行**

> 调用栈回溯自检：造一条已知的调用链, 验证&quot;展开 + 符号化&quot;整条路是通的。

> 放在这么早(任务系统, 页表都还没起来)是有意的 —— 它只读栈和
> .kallsyms 表, 两者在镜像加载完就都在了。正常时完全静默, 检查失败
> 才用 kprintf 报错(不受 LOG= 影响)。

> 为什么值得每次开机都跑：kallsyms 表是两步链接生成的, 一旦段序被挪动,
> 镜像本身完全正常, 但 panic 打出来的函数名会整体错位 —— 假的调用栈比
> 打不出来更危险, 它会把人带到完全错误的方向。这里当场喊出来。

**第 168 行**

> backtrace_selftest();

**第 170 行**

> 探测/设定缓存行大小(cache API 的 range 操作用它做步长)。
> 以前从来没人调用, 于是所有架构都吃硬编码的 64。

**第 176 行**

> Print welcome message

**第 177 行**

> 一条事实一行 —— 拆成三行没有多出任何信息

**第 180 行**

> ── 字符串/内存函数自检(STRING_TEST=1, 跑完继续启动)────────

**第 182 行（行尾）**

> tests/unit-test/string_test.c

**第 186 行**

> ── 初始化物理内存管理器 ─────────────────────────────────

**第 187 行**

> 链接脚本导出的 __kernel_image_start/end 是虚拟地址：高半区代码
> 对低物理地址的 ABS 符号做 PC 相对寻址会超出 ADRP/auipc/RIP-rel 的
> 范围(实测 aarch64 报 R_AARCH64_ADR_PREL_PG_HI21 truncated to fit)。
> 换算放在这里 —— boot 层知道 KERNEL_VMA, PMM 只收物理地址。

**第 197 行**

> 驱动阶段（一）：中断控制器、时间基准、根块设备。

> IRQ/TIME 不依赖文件系统，提前初始化；STORAGE 必须先于 fs_init()，
> 由 ramblk/sdblk 在 probe 里注册根块设备。

**第 210 行**

> 测试入口：普通启动在这里什么都不跑；只有 PMM_TEST=1 / KMEM_TEST=1
> 等测试变体才会执行对应测试（具体内容在 tests/unit-test/unit_test.c）。

**第 216 行**

> 驱动阶段（二）：DMA/ION/NPU/TPU

**第 219 行**

> ── 初始化任务子系统 ─────────────────────────────────

**第 221 行（行尾）**

> 安装 BSP per-CPU 指针

**第 225 行（行尾）**

> 必须在 cpu_init_bsp() 之后(见上方说明)

**第 230 行**

> 启动早期的 fs_init / ext4 装载等长临界区不污染运行时 RT 基线；
> 此时只有 BSP、没有用户任务/tick 回调，清空统计不会和别的核竞争。

**第 236 行**

> 先把 AP 拉起来, 再创建任何可能被摊核的任务。

> vCPU 任务的亲和性在创建时就算好了((slot + vcpu_id) % g_num_cpus)。
> 从前这一步在模式分支之后, 直启 guest 时 g_num_cpus 还是 1, 于是
> 4 个 vCPU 全被钉在 cpu0 —— guest 侧看是 4 核, 宿主只有一颗核在
> 分时, AP bring-up 和 IPI 都会被拖死。放到任务创建之前, 任何模式
> (直启 guest / busybox / 网络)都能按真实核数摊开。

**第 248 行**

> 放在创建 net-poll / busybox 之前：这些长期就绪任务会把 idle 任务
> 挤到队尾，测试的 boot 上下文（idle）在任务退出后不一定马上拿回 CPU。

**第 257 行**

> 同样必须在创建 net-poll / busybox 之前运行：boot 上下文不是可调度
> 的 task（idle 永不入就绪队列）。一旦 net-poll 这种&quot;永远 ready&quot;的
> NORMAL 任务存在，boot 被 tick 抢占后就不会再被调度回来，demo 的
> 调用点根本到不了。demo 自己临时安装 tick 回调，跑完摘掉。

**第 267 行**

> 驱动阶段（三）：网卡；net_init() 需要驱动先注册 netdev。
> guest 直启和 VMM 自测不需要网络，由测试变体的 boot 对象声明跳过。

**第 279 行**

> 选择启动模式：
> 默认 / 普通 shell      → busybox
> GUEST_LINUX=1          → tests/unit-test/guest_test.c
> VMM_TEST=1             → tests/unit-test/vcpu_thread_test.c

**第 299 行**

> SMP 检查完成, 现在才启动 busybox 交互 shell

**第 308 行**

> SMP 结果打一条 INFO。

> cpu_bring_up_all() 内部逐核的 &quot;cpuN online&quot; 和收尾的
> &quot;SMP bringup done: %u CPU(s) online&quot; 都是 KLOG_SMP(== 模块 DEBUG),
> 于是 LOG=info 下一条都看不见 —— 整个启动日志里就没有&quot;这台机器到底
> 几个核在跑&quot;这个事实, 而它决定了后面所有的并发行为(调度器, per-CPU
> 状态, VMM 的核间竞态, GIC 投递)。实测在 rk3588 上就是这个症状。

> 两个数一起报：CONFIG_SMP_CPUS 是编译期请求的, g_num_cpus 是实际
> 上线的。两者不等(PSCI CPU_ON 失败, 核被固件扣住)是最该一眼看出来的
> 情况, 只报一个的话那种情况会伪装成一切正常。

**第 323 行**

> ── 启用抢占, 进入 idle 循环(所有模式共用)─────────
> 在所有任务创建完成后启用, 避免 tick 打断内核初始化路径。
> 切换到 idle 专用栈(防止 boot 栈在频繁中断下溢出)。

**第 332 行**

> SMP 健康检查：抢占启用后立刻验证所有核 timer 都在 tick。

**第 335 行**

> SMP 线程分发自检(当前默认关闭，测试变体可手工打开)。SMP=1 时也会跑,
> 验证 round-robin 自身不破坏单核。失败仅 KLOG_ERROR, 不 panic, 避免
> 影响后续 busybox / vmm 测试。

**第 339 行**

> extern void smp_thread_test_run(uint32_t, uint32_t, uint32_t);
> smp_thread_test_run(4, 200, 800);

**第 344 行**

> 切到 idle 栈并进入 idle 主循环(永不返回)。

</details>

**位置**：`kernel/main.c:101`

<a id="mod-kernel-mm-aarch64-stage2.c"></a>

## kernel/mm/aarch64/stage2.c — AArch64 Stage-2 MMU 实现(每 VM 一份 + 按需分页)

`kernel/mm/aarch64/stage2.c` · 19 个函数 · 19 个有说明 · 471 行

老的实现是一组全局页表 + identity map + "init 后把非 RAM 清成无效"。
那套只能有一个 VM, 而且要求 guest RAM 预先分配并整片清零。现在是：

- 每个 VM 一个 s2_ctx_t；静态 L1/L2 按 slot 索引(STAGE2_MAX_VMS 份)；
- init 之后表是空的 —— "空表即全 trap", 不再需要 enable_mmio_trap；
- guest RAM 靠缺页驱动分配：没人预映射, guest 第一次访问某页时
VMM 在缺页处理里调 stage2_map_page() 现分配一个宿主物理页；
- 宿主自己要写的几块(内核映像/DTB/initrd/初始栈)在加载期用
stage2_map_range() 显式映射。

页表布局(VTCR SL0=1, T0SZ=32)：
L1: 4 entries × 1 GiB
L2: 4 × 512 entries × 2 MiB —— RAM 窗口内一律是 table descriptor
L3: 每个 2 MiB 块一张, 按需从 PMM 分配

记账不需要额外位图：l3_tbl[i] 非空就说明"该 2 MiB 块有页", 销毁时遍历
每张表的每个有效表项即可把按需页精确还回去。

<a id="fn-kernel-mm-aarch64-stage2.c-66"></a>

### `ipa_idx`

```c
static void ipa_idx(uint64_t ipa, int *i1, int *i2, int *i3)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 IPA 中分别取出 L1、L2、L3 三级页表索引写入输出参数。

**位置**：`kernel/mm/aarch64/stage2.c:66`

<a id="fn-kernel-mm-aarch64-stage2.c-80"></a>

### `ram_blk_index`

```c
static int ram_blk_index(const s2_ctx_t *s2, uint64_t ipa)
```

RAM 窗口内的 2 MiB 块序号(0..S2_MAX_L3_TABLES-1), 窗口外返回 -1。

注意： 这个序号故意与 L2 下标解耦：RAM 窗口起点不保证 1 GiB 对齐
(aarch64 guest 用的是 0x70000000, 落在 L2[1][384] 而不是 L2[0][0]),
直接拿它当 L2 下标会算错。

**位置**：`kernel/mm/aarch64/stage2.c:80`

<a id="fn-kernel-mm-aarch64-stage2.c-95"></a>

### `l3_ensure`

```c
static uint64_t *l3_ensure(s2_ctx_t *s2, uint64_t ipa)
```

确保 ipa 所在的 2 MiB 块有一张 L3 表(没有就从 PMM 现分配)。
返回该表, 失败返回 NULL(PMM 没页)。

**位置**：`kernel/mm/aarch64/stage2.c:95`

<a id="fn-kernel-mm-aarch64-stage2.c-121"></a>

### `stage2_vm_init`

```c
void stage2_vm_init(s2_ctx_t *s2, uint32_t slot, uint32_t vmid, uint64_t ram_base, uint64_t ram_size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 slot 后清空 stage-2 上下文，建立 L1 到 L2 的表结构并计算 VTCR/VTTBR，随后立即激活该 VM。

<details>
<summary>函数内备注（3 条）</summary>

**第 134 行**

> 默认没有 NPU 恒等窗口；需要时由 vmm_arch_vm_init() 显式设定。
> 不清零的话静态表在 VM 复用时会带着上一个 VM 的窗口。

**第 146 行**

> 空表 = 全 trap：没有一条有效映射, 任何 IPA 访问都陷入 EL2

**第 150 行**

> 注意： L1 必须指向 L2 —— 这几条 entry 是&quot;表结构&quot;, 不是&quot;映射&quot;, 所以即使
> 一张空表也要有。漏掉的症状极具误导性：l3_ensure()/stage2_map_page()
> 只碰 L2/L3, 看上去完全正常(第二次访问同一 IPA 能命中已有表项,
> 还回同一个 PA), 但硬件从 L1 就走进死胡同 —— guest 于是在同一条取指
> 上无限重复 fault：`ifetch fault ipa=0x70200000 -&gt; pa=0x40002000` × N。

</details>

**位置**：`kernel/mm/aarch64/stage2.c:121`

<a id="fn-kernel-mm-aarch64-stage2.c-173"></a>

### `stage2_vm_destroy`

```c
void stage2_vm_destroy(s2_ctx_t *s2)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历释放受管 L3 叶页（外部映射跳过）和各 L3 表页，清空两级表并刷 TLB 后清零上下文。

<details>
<summary>函数内备注（3 条）</summary>

**第 190 行**

> NPU DMA identity / device MMIO：页不归 PMM

**第 196 行（行尾）**

> 表页本身

**第 200 行**

> 先关掉映射(清表)再 flush —— 顺序反了会短暂留下指向已释放页的映射

</details>

**位置**：`kernel/mm/aarch64/stage2.c:173`

<a id="fn-kernel-mm-aarch64-stage2.c-214"></a>

### `stage2_activate`

```c
void stage2_activate(const s2_ctx_t *s2)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写入 VTCR_EL2 与 VTTBR_EL2，再置 HCR_EL2 的 VM 位并同步，激活该 VM 的 stage-2 翻译。

**位置**：`kernel/mm/aarch64/stage2.c:214`

<a id="fn-kernel-mm-aarch64-stage2.c-231"></a>

### `stage2_ipa_is_ram`

```c
int stage2_ipa_is_ram(const s2_ctx_t *s2, uint64_t ipa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断 IPA 是否落在该 VM 的 RAM 窗口内。

**位置**：`kernel/mm/aarch64/stage2.c:231`

<a id="fn-kernel-mm-aarch64-stage2.c-236"></a>

### `stage2_set_npu_dma_window`

```c
void stage2_set_npu_dma_window(s2_ctx_t *s2, uint64_t base, uint64_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

记录 NPU DMA 恒等窗口的起止地址，size 为 0 时窗口为空。

**位置**：`kernel/mm/aarch64/stage2.c:236`

<a id="fn-kernel-mm-aarch64-stage2.c-242"></a>

### `stage2_ipa_is_npu_dma`

```c
int stage2_ipa_is_npu_dma(const s2_ctx_t *s2, uint64_t ipa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅在 NPU DMA 窗口非空时判断 IPA 是否落在其范围内。

**位置**：`kernel/mm/aarch64/stage2.c:242`

<a id="fn-kernel-mm-aarch64-stage2.c-265"></a>

### `stage2_map_identity_block`

```c
uint64_t stage2_map_identity_block(s2_ctx_t *s2, uint64_t ipa)
```

stage2_map_identity_block — 把 ipa 所在的 2 MiB 块恒等映射(PA = IPA)。

与 stage2_map_block 的唯一区别：不调 pmm_alloc_pages, PA 直接用 IPA。
这一段的物理内存由宿主 platform.conf 的 reserves 钉住, 所以 IPA 一定是
一段真实存在, 且没人会拿去复用的物理地址。

为什么必须恒等：RK3588 NPU 无 IOMMU, 它只认物理地址。guest 里 librknnrt
拿到的"设备地址"是驱动从 CMA/预留池分配出来的物理地址, NPU 就照着那个
地址去读写 —— 只有 IPA == PA 时, 这个地址在 guest 和宿主两边指的是同一块
内存。(详见 include/arch/aarch64/stage2.h 里 s2_ctx_t.npu_dma_base 的说明。)

属性用 Normal(不是 Device)：这是普通内存, NPU 要用 AXI 正常访问它；
且 guest 侧 CPU 也要读写(拷输入, 读结果), 用 Device 属性会慢得离谱。

已映射的项跳过(幂等)—— 缺页重试, 以及加载期预映射都会走到。
返回本次新装的页数。

<details>
<summary>函数内备注（2 条）</summary>

**第 279 行（行尾）**

> 已经映射过

**第 282 行**

> identity 映射：IPA == PA, 页由 platform reserves 钉住, 不归 PMM

</details>

**位置**：`kernel/mm/aarch64/stage2.c:265`

<a id="fn-kernel-mm-aarch64-stage2.c-289"></a>

### `stage2_map_page`

```c
uint64_t stage2_map_page(s2_ctx_t *s2, uint64_t ipa, int zero)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

确保 L3 表存在后幂等映射一页，未映射时分配物理页并按需清零，返回物理地址或 0。

<details>
<summary>函数内备注（2 条）</summary>

**第 300 行**

> 已映射：幂等(缺页重试, 以及&quot;先 map_range 后 fault&quot;都会走到)

**第 306 行（行尾）**

> 注意： 调用方必须处理, 不能当成功继续

</details>

**位置**：`kernel/mm/aarch64/stage2.c:289`

<a id="fn-kernel-mm-aarch64-stage2.c-330"></a>

### `stage2_map_block`

```c
uint64_t stage2_map_block(s2_ctx_t *s2, uint64_t ipa, int zero)
```

stage2_map_block — 一次把 ipa 所在的整个 2 MiB 块装好(512 个 4 KiB 页)

为什么需要：按需分页下每次缺页都要一次完整的 EL2 往返(保存/恢复 128 个
EL2 系统寄存器 + PMM 分配 + 装表), TCG 下实测约 1.2ms。Linux 启动期间
实测缺页 1447 次 ⇒ 白白多花约 1.7 秒(对照：guest 自报的启动时间只有
1.8 秒, 墙上却要 3.6 秒, 差额就是它)。

而启动阶段的访问是密集的 —— 一次装一整块能把这 1447 次压到几十次。
代价是最多 2 MiB 的过取, 相对启动期的密集触碰可以接受(这正是 plan 里
给 4 KiB 粒度留的"测速后再上"的口子)。

注意：块内 512 页是各自独立分配的, 不要求物理连续 —— 省下的是 EL2
往返次数, 不是分配开销。返回本次装上的页数。

<details>
<summary>函数内备注（2 条）</summary>

**第 344 行（行尾）**

> 已经映射过

**第 348 行（行尾）**

> PMM 没页：装多少算多少

</details>

**位置**：`kernel/mm/aarch64/stage2.c:330`

<a id="fn-kernel-mm-aarch64-stage2.c-359"></a>

### `stage2_map_range`

```c
uint64_t stage2_map_range(s2_ctx_t *s2, uint64_t ipa, uint64_t size, int zero)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐页建立映射，任一页失败即报错并返回已映射页数，全部成功后在末尾统一刷 TLB。

<details>
<summary>函数内备注（1 条）</summary>

**第 375 行**

> 批量映射末尾统一刷一次；中途每页都刷会慢到不可用

</details>

**位置**：`kernel/mm/aarch64/stage2.c:359`

<a id="fn-kernel-mm-aarch64-stage2.c-381"></a>

### `stage2_lookup`

```c
int stage2_lookup(const s2_ctx_t *s2, uint64_t ipa, uint64_t *pa_out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅在 RAM 窗口内查 L2 表项与 L3 叶项，命中时输出带页内偏移的物理地址并返回 1。

<details>
<summary>函数内备注（1 条）</summary>

**第 393 行（行尾）**

> RAM 窗口内只允许 table descriptor

</details>

**位置**：`kernel/mm/aarch64/stage2.c:381`

<a id="fn-kernel-mm-aarch64-stage2.c-407"></a>

### `stage2_map_device_region`

```c
void stage2_map_device_region(s2_ctx_t *s2, uint64_t ipa, uint64_t pa, uint64_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐页建立设备 MMIO 的读写不可执行映射且不置属主位，完成后刷新整个 VM 的 TLB。

<details>
<summary>函数内备注（1 条）</summary>

**第 418 行**

> device MMIO：PA 属于设备, 不归 PMM, 不要置 owner 位

</details>

**位置**：`kernel/mm/aarch64/stage2.c:407`

<a id="fn-kernel-mm-aarch64-stage2.c-426"></a>

### `stage2_tlb_flush_vm`

```c
void stage2_tlb_flush_vm(const s2_ctx_t *s2)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先重写本 VM 的 VTTBR 再执行全 VM 的 TLBI，配合 dsb 与 isb 保证失效生效。

<details>
<summary>函数内备注（1 条）</summary>

**第 428 行**

> 注意： 先重写 VTTBR 再发 TLBI：TLBI 是对&quot;当前 VTTBR 指定的 VM&quot;生效的,
> 而本核可能刚被定时器抢占去跑了另一个 VM 的 vCPU 任务(它一进循环就
> 改 VTTBR_EL2)。不拉回来的话会出现&quot;改了映射但不生效, 偶尔又生效&quot;。

</details>

**位置**：`kernel/mm/aarch64/stage2.c:426`

<a id="fn-kernel-mm-aarch64-stage2.c-440"></a>

### `stage2_tlb_flush_ipa`

```c
void stage2_tlb_flush_ipa(const s2_ctx_t *s2, uint64_t ipa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

重写本 VM 的 VTTBR 后按 IPA 做单页 stage-2 TLB 失效并同步。

**位置**：`kernel/mm/aarch64/stage2.c:440`

<a id="fn-kernel-mm-aarch64-stage2.c-451"></a>

### `stage2_set_ro`

```c
void stage2_set_ro(s2_ctx_t *s2, uint64_t ipa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把已映射页的权限改为只读并做单页 TLB 失效，表项无效时不做任何事。

**位置**：`kernel/mm/aarch64/stage2.c:451`

<a id="fn-kernel-mm-aarch64-stage2.c-462"></a>

### `stage2_restore`

```c
void stage2_restore(s2_ctx_t *s2, uint64_t ipa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把已映射页恢复为可读写并做单页 TLB 失效，表项无效时不做任何事。

**位置**：`kernel/mm/aarch64/stage2.c:462`

<a id="mod-kernel-mm-aarch64-vm_early.c"></a>

## kernel/mm/aarch64/vm_early.c — AArch64 虚拟内存管理

`kernel/mm/aarch64/vm_early.c` · 6 个函数 · 6 个有说明 · 194 行

参考：ref/Avatar/kernel/boot/earlypage.c
实现 48 位页表, 使用 TTBR0/TTBR1 双页表结构

<a id="fn-kernel-mm-aarch64-vm_early.c-45"></a>

### `set_table_entry`

```c
static inline void set_table_entry(uint64_t *pte, uint64_t *next_table)
```

set_table_entry - 设置页表项指向下一级页表

**位置**：`kernel/mm/aarch64/vm_early.c:45`

<a id="fn-kernel-mm-aarch64-vm_early.c-53"></a>

### `set_block_entry`

```c
static inline void set_block_entry(uint64_t *pte, uint64_t base_addr, uint64_t flags)
```

set_block_entry - 设置 1GB 块映射页表项

**位置**：`kernel/mm/aarch64/vm_early.c:53`

<a id="fn-kernel-mm-aarch64-vm_early.c-63"></a>

### `vm_kernel_pgtable`

```c
pte_t *vm_kernel_pgtable(void)
```

vm_kernel_pgtable - 获取内核页表基址(虚拟地址)

**位置**：`kernel/mm/aarch64/vm_early.c:63`

<a id="fn-kernel-mm-aarch64-vm_early.c-73"></a>

### `vm_get_kernel_pgtable`

```c
uint64_t vm_get_kernel_pgtable(void)
```

vm_get_kernel_pgtable - 获取内核页表基址(物理地址)

返回：内核页表物理地址(用于 TTBR1_EL1)

**位置**：`kernel/mm/aarch64/vm_early.c:73`

<a id="fn-kernel-mm-aarch64-vm_early.c-83"></a>

### `vm_get_boot_pgtable`

```c
uint64_t vm_get_boot_pgtable(void)
```

vm_get_boot_pgtable - 获取启动页表基址(物理地址)

返回：启动页表物理地址(用于 TTBR0_EL1, 低地址恒等映射)

**位置**：`kernel/mm/aarch64/vm_early.c:83`

<a id="fn-kernel-mm-aarch64-vm_early.c-101"></a>

### `vm_init`

```c
uint64_t vm_init(void)
```

vm_init - 初始化虚拟内存管理器

建立初始页表映射(使用两个独立页表)：
- TTBR0(低地址空间)：暂时全 0
- TTBR1(高地址空间)：
- Entry[0]: 0x0000_0000 -> 0x0000_0000(设备内存, 1GB)
- Entry[1]: 0x4000_0000 -> 0x4000_0000(普通内存, 1GB)

返回：0 表示成功, 负值表示失败

<details>
<summary>函数内备注（21 条）</summary>

**第 103 行**

> ⚠️ 本函数由 boot.S 在 **MMU 打开之前**调用, 因此这里绝对不能出现
> KLOG_* / kprintf / 任何间接调用的输出路径。

> 原因：uart_putchar() 经 g_serial_ops 间接调用, 而 g_serial_ops 里存的是
> 链接期的**高虚拟地址**函数指针。代码本身能靠 PC 相对寻址在物理地址上
> 跑, 但解引用这个指针时 MMU 还没开 → data abort。此时 VBAR_EL2 尚未
> 设置, 异常落到 0x200, 紧接着是未定义指令的原地死循环 —— 表现为
> &quot;LOG=debug 一个字符都没有, info/warn 正常&quot;。

> LOG=debug 曾经在这里打 KLOG_MM，正好踩中。要输出早期信息只能等
> kernel_main 的 platform_early_init() 之后。

**第 117 行**

> platform_conf_scan() 在 kernel_main 中才运行, 现在 dw_uart_base / reg_shift
> 仍是编译期默认值(RISC-V QEMU：base=0x10000000, reg_shift=0)。
> 在此直接覆写为 RK3588 物理值, 供后续(MMU 打开后)使用：
> reg_shift=2 → 寄存器步长 4 字节(LSR = base + 0x14 = 0xFEB50014)
> reg_shift=0 会导致 LSR 地址=0xFEB50005(错！THRE 永远读不到 → 死循环)

**第 130 行**

> 清空所有页表

**第 136 行**

> 设置 TTBR1 页表(高地址空间, 内核使用)

> TTBR1 覆盖的虚拟地址范围：[0xffff000000000000, 0xffffffffffffffff]

> 页表结构：
> - L0(PT0)：1 个 entry, 指向 L1 页表
> - L1(PT1)：512 个 entry, 每个映射 1GB

> 映射规则：
> - 访问虚拟地址 0xffff000000000000 + offset
> - 页表索引计算使用 offset(去掉符号扩展)
> - 例如：0xffff00000000400000 -&gt; 索引 0x0000000000040000

**第 151 行**

> L0 页表：entry[0] 指向 L1 页表

**第 154 行**

> L1 页表：1GB 块映射, 内容由平台决定

> QEMU virt:
> [0] 0x00000000-0x3FFFFFFF  设备内存 (UART@0x09000000, GIC@0x08000000)
> [1] 0x40000000-0x7FFFFFFF  普通内存 (QEMU RAM, 内核在此)
> [2] 0x80000000-0xBFFFFFFF  普通内存 (QEMU 2GB 扩展)

> RK3588:
> [0] 0x00000000-0x3FFFFFFF  普通内存 (RAM, 内核在 0x400000)
> [3] 0xC0000000-0xFFFFFFFF  设备内存 (UART@0xFEB50000, GIC@0xFE600000)

**第 167 行（行尾）**

> RAM Bank0 0x00000000-0x3FFFFFFF

**第 168 行（行尾）**

> RAM Bank1 0x40000000-0x7FFFFFFF

**第 169 行（行尾）**

> RAM Bank2 0x80000000-0xBFFFFFFF

**第 170 行（行尾）**

> MMIO      0xC0000000-0xFFFFFFFF

**第 172 行（行尾）**

> 设备内存(0x00000000 - 0x3fffffff)

**第 173 行（行尾）**

> 普通内存(0x40000000 - 0x7fffffff)

**第 174 行（行尾）**

> 普通内存(0x80000000 - 0xbfffffff)

**第 177 行**

> 设置 TTBR0 页表(低地址空间, 恒等映射, MMU 开启后低地址代码继续可用)

**第 183 行（行尾）**

> RAM Bank0 0x00000000-0x3FFFFFFF

**第 184 行（行尾）**

> RAM Bank1 0x40000000-0x7FFFFFFF

**第 185 行（行尾）**

> RAM Bank2 0x80000000-0xBFFFFFFF

**第 186 行（行尾）**

> MMIO      0xC0000000-0xFFFFFFFF

**第 188 行（行尾）**

> 设备内存(0x00000000 - 0x3fffffff)

**第 189 行（行尾）**

> 普通内存(0x40000000 - 0x7fffffff)

**第 190 行（行尾）**

> 普通内存(0x80000000 - 0xbfffffff)

</details>

**位置**：`kernel/mm/aarch64/vm_early.c:101`

<a id="mod-kernel-mm-aarch64-vmm.c"></a>

## kernel/mm/aarch64/vmm.c — 兼容性定义(替换参考项目中的符号)

`kernel/mm/aarch64/vmm.c` · 23 个函数 · 23 个有说明 · 622 行

<a id="fn-kernel-mm-aarch64-vmm.c-40"></a>

### `read_ttbr0_el1`

```c
static inline uint64_t read_ttbr0_el1(void)
```

内联汇编函数 - 读取 TTBR0_EL1 寄存器

**位置**：`kernel/mm/aarch64/vmm.c:40`

<a id="fn-kernel-mm-aarch64-vmm.c-49"></a>

### `find_pte`

```c
pte_t *find_pte(pte_t *page_dir, uint64_t vaddr, int32_t alloc)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 PGD 逐级向下查找 vaddr 的 L3 表项，缺失时按 alloc 决定分配并清零子表或返回 NULL。

<details>
<summary>函数内备注（8 条）</summary>

**第 54 行**

> 获取PGD索引

**第 57 行**

> 分配 PUD

**第 65 行（行尾）**

> valid + table

**第 69 行**

> 获取PUD表项

**第 74 行**

> 分配 PMD

**第 86 行**

> 获取PMD表项

**第 91 行**

> 分配 Page Table

**第 103 行**

> 获取PTE表项

</details>

**位置**：`kernel/mm/aarch64/vmm.c:49`

<a id="fn-kernel-mm-aarch64-vmm.c-110"></a>

### `memory_create_map`

```c
int32_t memory_create_map(void *page_dir, uint64_t vaddr, uint64_t paddr, int32_t count, uint64_t perm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

为 count 个页逐个查找或分配 PTE 并写入 PFN 与按 perm 选择的权限位，重复映射或失败返回 -1。

<details>
<summary>函数内备注（17 条）</summary>

**第 113 行**

> extern char __kernel_end[];

**第 117 行**

> 如果 heap_start 不是页对齐的, 将其向上对齐

**第 119 行**

> 这里start和end计算出来的都是物理地址

**第 126 行**

> 获取对应的 PTE

**第 129 行**

> 映射失败, 调用方拿不到它要的地址空间 —— 真错误

**第 135 行**

> 重复映射是调用方的约定违反：降级但继续, 且每个调用点只说一次
> (循环调用的调用方会让它反复触发)。两个 return 都在循环体内,
> 所以单次调用最多触发一次。

**第 144 行**

> 设置 PTE 为有效并设置物理地址

**第 147 行（行尾）**

> 36 bits PFN Page Frame Number

**第 149 行**

> 设置权限

**第 152 行（行尾）**

> Inner shareable

**第 156 行（行尾）**

> Normal memory

**第 159 行（行尾）**

> Inner shareable

**第 163 行（行尾）**

> Normal memory

**第 166 行（行尾）**

> Inner shareable

**第 170 行（行尾）**

> device memory

**第 173 行**

> 映射结果：页表遍历的落点, 排查&quot;映射对不对&quot;时最想要的一条

**第 179 行**

> 更新虚拟地址和物理地址

</details>

**位置**：`kernel/mm/aarch64/vmm.c:110`

<a id="fn-kernel-mm-aarch64-vmm.c-187"></a>

### `current_page_dir`

```c
pte_t *current_page_dir()
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 TTBR0_EL1 中的当前页目录地址。

**位置**：`kernel/mm/aarch64/vmm.c:187`

<a id="fn-kernel-mm-aarch64-vmm.c-192"></a>

### `memory_get_paddr`

```c
uint64_t memory_get_paddr(void *page_dir, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

查找 PTE 并确认其为有效页描述符后，返回 PFN 加页内偏移的物理地址，否则返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 196 行**

> find_pte(alloc=0) 在页表路径存在时, 即使 leaf 无效也会返回指针。
> 这里必须自己确认它确实是有效的 page descriptor(L3 的 bit[1:0]=11),
> 否则会把垃圾 pfn 当成物理地址返回。当前调用点都传页对齐地址, 所以
> 只是&quot;恰好&quot;没炸；下一次改动踩中就是随机物理页。

</details>

**位置**：`kernel/mm/aarch64/vmm.c:192`

<a id="fn-kernel-mm-aarch64-vmm.c-208"></a>

### `memory_get_pte_raw`

```c
uint64_t memory_get_pte_raw(void *page_dir, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

查找 vaddr 对应的 PTE 并返回其原始值，页表路径不存在时返回 0。

**位置**：`kernel/mm/aarch64/vmm.c:208`

<a id="fn-kernel-mm-aarch64-vmm.c-216"></a>

### `memory_set_pte_nofree`

```c
void memory_set_pte_nofree(void *page_dir, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

找到 PTE 后置 PTE_NOFREE 位，标记该页交给共享页账本管理。

**位置**：`kernel/mm/aarch64/vmm.c:216`

<a id="fn-kernel-mm-aarch64-vmm.c-223"></a>

### `memory_alloc_page`

```c
uint64_t memory_alloc_page(void *page_dir, uint64_t vaddr, uint64_t size, int32_t perm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐页分配物理页并映射到指定虚拟范围，失败时回滚已映射页并返回 -1，成功返回 0。

<details>
<summary>函数内备注（5 条）</summary>

**第 231 行**

> 逐页分配内存, 然后建立映射关系

**第 233 行**

> 分配需要的内存

**第 240 行**

> 建立分配的内存与指定地址的关联

**第 244 行**

> 这一页还没进页表, 单独还回去

**第 256 行**

> 失败时把本次已经映射的页逐个拆掉。以前这里写的是
> pmm_free_pages(vaddr, i + 1) —— vaddr 是用户虚拟地址, 不是刚分配的
> 物理地址；PMM 会按非法地址拒绝, 于是已分配的页全部泄漏。

</details>

**位置**：`kernel/mm/aarch64/vmm.c:223`

<a id="fn-kernel-mm-aarch64-vmm.c-266"></a>

### `memory_free_page`

```c
void memory_free_page(void *page_dir, uint64_t addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 PTE 有效后按 KEEP、NOFREE 分别只拆表、归还 PMM 或递减共享引用，最后清零表项。

<details>
<summary>函数内备注（1 条）</summary>

**第 272 行**

> PTE_KEEP：设备缓冲(ION / dma-buf)映射。页归句柄所有, 不是这个
> 映射的 —— 所以 unmap 只拆页表项, 既不释放也不碰 shared_page 计数。

> 少了这一条：munmap 会把 dma_buf 底下的物理页释放回 PMM, 而 dma_buf
> 仍然指着它；随后 close(fd) → dma_heap_free() 再释放一次(双重释放),
> 中间那段窗口里这些页随时会被分配给别的用途 —— 对 NPU 缓冲区来说
> 就是&quot;设备往别人的内存里写&quot;。实测症状是满屏
> `PMM: partial free detected: requested=N, actually_freed=0`。

</details>

**位置**：`kernel/mm/aarch64/vmm.c:266`

<a id="fn-kernel-mm-aarch64-vmm.c-299"></a>

### `memory_set_pte_keep`

```c
void memory_set_pte_keep(void *page_dir, uint64_t vaddr)
```

把某页标成 PTE_KEEP(设备缓冲)。与 memory_set_pte_nofree 同构,
区别见 include/arch/aarch64/mmu.h 里 PTE_KEEP 的说明。

**位置**：`kernel/mm/aarch64/vmm.c:299`

<a id="fn-kernel-mm-aarch64-vmm.c-306"></a>

### `create_uvm`

```c
pte_t *create_uvm(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配并清零 PGD，把内核区间与设备区间恒等映射进去后返回页目录指针，分配失败返回 0。

<details>
<summary>函数内备注（6 条）</summary>

**第 309 行**

> extern char __kernel_end[];

**第 319 行**

> 如果 heap_start 不是页对齐的, 将其向上对齐

**第 321 行**

> 这里start和end计算出来的都是物理地址

**第 329 行**

> TODO: 这个地方需要让el0进程共享内核空间
> TODO: 这个地方原理上并不需要映射, 因为内核应该使用 FFFF_0000_0000_0000 之后的地址

**第 334 行（行尾）**

> 内核空间先恒等映射

**第 339 行（行尾）**

> 设备内存恒等映射

</details>

**位置**：`kernel/mm/aarch64/vmm.c:306`

<a id="fn-kernel-mm-aarch64-vmm.c-344"></a>

### `_destroy_page_table_vm`

```c
void _destroy_page_table_vm(pte_t *table, int32_t level)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

递归销毁用户页表：叶页按 KEEP、NOFREE、普通页分别处理并跳过内核与设备区间，中间表释放后清零。

<details>
<summary>函数内备注（2 条）</summary>

**第 378 行**

> 注意： 这里是进程退出销毁地址空间的路径, 与 munmap 走的
> memory_free_page() 是两趟不同的遍历。只修那一边的话,
> 子进程退出时照样会把设备缓冲的页释放掉(实测：fork 用例
> 里恰好冒出 1 条 partial free)。两处都要认 PTE_KEEP。

**第 385 行**

> 设备缓冲：页归 dma_buf / ION 句柄所有, 这里只拆表

</details>

**位置**：`kernel/mm/aarch64/vmm.c:344`

<a id="fn-kernel-mm-aarch64-vmm.c-407"></a>

### `_destroy_page_table`

```c
void _destroy_page_table(pte_t *table, int32_t level)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

递归释放到第 3 级为止的中间页表页，不处理叶页也不清零表项。

<details>
<summary>函数内备注（6 条）</summary>

**第 409 行**

> 输出当前正在处理的层级

**第 414 行**

> 各级页表的最大项数(按实际情况调整)

**第 419 行**

> 遍历当前层级的所有页表项

**第 429 行**

> 输出当前页表项的信息

**第 431 行**

> 递归释放下一层页表

**第 434 行**

> 释放当前这一级的页表页

</details>

**位置**：`kernel/mm/aarch64/vmm.c:407`

<a id="fn-kernel-mm-aarch64-vmm.c-441"></a>

### `_copy_page_table`

```c
bool _copy_page_table(pte_t *src_table, pte_t *dst_table, int32_t level)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

递归复制页表：内核、设备与 KEEP 项原样共享，NOFREE 增加共享引用，其余叶页复制新页，中间表新建。

<details>
<summary>函数内备注（7 条）</summary>

**第 444 行**

> extern char __kernel_end[];

**第 452 行**

> 第4级页表：实际映射的物理页

**第 468 行**

> 只有 NOFREE(共享页)要加引用；KEEP(设备缓冲)页归句柄
> 所有, 复制一份映射不产生新的持有者。

**第 480 行**

> 拷贝页内容

**第 483 行**

> 设置目标页表项

**第 486 行**

> 中间层级：创建目标子表并递归拷贝

**第 497 行**

> 设置当前页表项指向新分配的页表

</details>

**位置**：`kernel/mm/aarch64/vmm.c:441`

<a id="fn-kernel-mm-aarch64-vmm.c-508"></a>

### `destroy_uvm_4level`

```c
void destroy_uvm_4level(pte_t *page_dir)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 PGD 开始递归销毁用户页表，最后释放 PGD 自身。

<details>
<summary>函数内备注（2 条）</summary>

**第 510 行**

> level: 0 = PGD, 1 = PUD, 2 = PMD, 3 = PTE

**第 512 行（行尾）**

> 最后释放 PGD 自身

</details>

**位置**：`kernel/mm/aarch64/vmm.c:508`

<a id="fn-kernel-mm-aarch64-vmm.c-515"></a>

### `destory_4level`

```c
void destory_4level(pte_t *page_dir)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 PGD 开始递归销毁中间页表，最后释放 PGD 自身。

<details>
<summary>函数内备注（2 条）</summary>

**第 517 行**

> level: 0 = PGD, 1 = PUD, 2 = PMD, 3 = PTE

**第 519 行（行尾）**

> 最后释放 PGD 自身

</details>

**位置**：`kernel/mm/aarch64/vmm.c:515`

<a id="fn-kernel-mm-aarch64-vmm.c-523"></a>

### `copydata_to_uvm`

```c
void copydata_to_uvm(void *page_dir, uint64_t vaddr, uint64_t paddr, uint64_t size)
```

内核将数据拷贝到指定进程空间下

<details>
<summary>函数内备注（7 条）</summary>

**第 529 行**

> 获取页表项

**第 532 行**

> 页未映射 —— 拷贝会被静默截断, 数据已经不一致了。
> 循环体内但紧跟 return, 所以每次调用最多一次, 不需要采样。

**第 539 行**

> 获取物理页帧号(PFN), 并计算出物理地址

**第 543 行**

> 虚拟页起始地址(用于写入)

**第 546 行**

> 当前页剩余空间

**第 551 行**

> 源地址

**第 554 行**

> 拷贝数据

</details>

**位置**：`kernel/mm/aarch64/vmm.c:523`

<a id="fn-kernel-mm-aarch64-vmm.c-562"></a>

### `memory_copy_uvm_4level`

```c
int32_t memory_copy_uvm_4level(void *dst_pgd, void *src_pgd)
```

复制某个进程空间的所有内存到另一个进程空间下

**位置**：`kernel/mm/aarch64/vmm.c:562`

<a id="fn-kernel-mm-aarch64-vmm.c-574"></a>

### `mm_vm_init_user_pgd`

```c
void mm_vm_init_user_pgd(void *pgd, uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

AArch64 下的空实现，用户页表不含内核映射，内核映射位于 TTBR1。

<details>
<summary>函数内备注（1 条）</summary>

**第 578 行**

> TTBR0/TTBR1 硬件分割：用户页表不包含内核映射。

</details>

**位置**：`kernel/mm/aarch64/vmm.c:574`

<a id="fn-kernel-mm-aarch64-vmm.c-581"></a>

### `mm_vm_destroy_user_space`

```c
void mm_vm_destroy_user_space(uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对非零 PGD 物理地址先转虚拟地址，再走用户页表销毁流程。

**位置**：`kernel/mm/aarch64/vmm.c:581`

<a id="fn-kernel-mm-aarch64-vmm.c-588"></a>

### `mm_vm_unmap_page`

```c
int mm_vm_unmap_page(void *pgd, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若 vaddr 有有效映射则拆页并按类型释放物理页，返回 1；无映射返回 0。

**位置**：`kernel/mm/aarch64/vmm.c:588`

<a id="fn-kernel-mm-aarch64-vmm.c-597"></a>

### `mm_vm_flush_tlb_user`

```c
void mm_vm_flush_tlb_user(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 dsb、tlbi vmalle1is、dsb、isb 的顺序刷新用户 TLB。

<details>
<summary>函数内备注（4 条）</summary>

**第 599 行**

> 页表写 → TLBI → 同步。tlbi 是架构指令, barrier API 里没有对应物。

**第 600 行（行尾）**

> 原 dsb ishst

**第 602 行（行尾）**

> 原 dsb ish

**第 603 行（行尾）**

> 原 isb

</details>

**位置**：`kernel/mm/aarch64/vmm.c:597`

<a id="fn-kernel-mm-aarch64-vmm.c-606"></a>

### `mm_vm_switch_user`

```c
void mm_vm_switch_user(uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次写 TTBR0_EL1、isb、全量 TLBI、dsb、isb，切换到新的用户页表。

<details>
<summary>函数内备注（1 条）</summary>

**第 608 行**

> execve 原地换页表。和 arch_task_switch 里的 TTBR0 切换保持同一序列：
> 写 TTBR0 → isb → 全量 TLBI → dsb → isb。

> 内核映射在 TTBR1，切 TTBR0 时当前内核栈/代码不受影响；但旧页表的
> 用户映射必须在新页表生效后才能销毁，所以调用点都严格按
> switch → destroy 的顺序。

</details>

**位置**：`kernel/mm/aarch64/vmm.c:606`

<a id="mod-kernel-mm-elf_image.c"></a>

## kernel/mm/elf_image.c — ELF 镜像加载

`kernel/mm/elf_image.c` · 5 个函数 · 5 个有说明 · 449 行

职责：ELF 格式校验 + PT_LOAD 段加载到用户地址空间 + RELA 重定位
输入：已初始化的用户页表(含内核映射, 由调用方准备)
输出：elf_image_info_t(入口点, VA 范围, 程序头地址等)

<a id="fn-kernel-mm-elf_image.c-30"></a>

### `elf_load_segment`

```c
static int elf_load_segment(void *pgd, elf64_phdr_t *phdr, uint8_t *file_data)
```

elf_load_segment - 加载单个 ELF 段到用户空间

<details>
<summary>函数内备注（1 条）</summary>

**第 47 行（行尾）**

> 默认：用户可读写可执行

</details>

**位置**：`kernel/mm/elf_image.c:30`

<a id="fn-kernel-mm-elf_image.c-82"></a>

### `elf_load_segment_with_base`

```c
static int elf_load_segment_with_base(void *pgd, elf64_phdr_t *phdr, uint8_t *file_data, uint64_t image_base)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把段的虚拟地址加上 image_base 后复用 elf_load_segment 装载。

**位置**：`kernel/mm/elf_image.c:82`

<a id="fn-kernel-mm-elf_image.c-94"></a>

### `elf_image_load_impl`

```c
static int elf_image_load_impl(uint8_t *file_data, uint64_t file_size, void *pgd, uint64_t force_base, elf_image_info_t *out)
```

内部实现。force_base=0 → 自动选择基址；force_base≠0 → 强制使用(用于 interpreter)。

<details>
<summary>函数内备注（10 条）</summary>

**第 137 行**

> 调用栈回溯的触发点(只在 PANIC_TEST=1 的构建里存在, 见 Makefile §7)。

> 放在这里是有讲究的：ELF 头部刚刚校验通过, 程序头还没开始解析,
> 离 boot 足够远 —— 打出来的是一条真实的加载器调用链
> platform_panic ← elf_image_load_impl ← elf_image_load
> ← exec_elf_image ← exec_handler/elf_loader …
> 而不是只有 kernel_main 两层, 正好用来看展开和符号化对不对。

> 直接调 platform_panic() 而不是 assert_always()：走的是和真实断言失败
> 完全一样的那条路(platform_panic 自己会摘 klog 锁, 打栈, 停机),
> 所以看到的东西不会因为是&quot;演示&quot;而和真实崩溃不一样。

> 用法： make PLATFORM=&lt;p&gt; test-panic
> 撤掉： 不传 PANIC_TEST 就是普通内核, 本段不参与编译。

**第 168 行**

> ET_DYN 使用 PIE 基址 USER_CODE_BASE, 或调用方指定的 force_base(interpreter 用)

**第 180 行**

> 扫描 PT_INTERP(仅主程序；加载 interpreter 时不递归)

**第 200 行**

> 加载所有 PT_LOAD 段

**第 219 行**

> ── 处理 RELA 重定位(无解释器的 PIE 需要)────────────────────

**第 380 行**

> R_X86_64_COPY: 需要符号解析后从共享库复制数据
> 交由 ld.so 完成, 内核静默跳过

**第 402 行**

> ── 计算程序头的用户态地址(AT_PHDR auxv)──────────────────

**第 407 行**

> 优先查找 PT_PHDR 段(类型值 6)

**第 410 行（行尾）**

> PT_PHDR

**第 415 行**

> 没有 PT_PHDR：用第一个 PT_LOAD 的基址 + 文件内偏移

</details>

**位置**：`kernel/mm/elf_image.c:94`

<a id="fn-kernel-mm-elf_image.c-441"></a>

### `elf_image_load`

```c
int elf_image_load(uint8_t *file_data, uint64_t file_size, void *pgd, elf_image_info_t *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 image_base 为 0 调用 elf_image_load_impl 装载 ELF 镜像。

**位置**：`kernel/mm/elf_image.c:441`

<a id="fn-kernel-mm-elf_image.c-446"></a>

### `elf_image_load_at`

```c
int elf_image_load_at(uint8_t *file_data, uint64_t file_size, void *pgd, uint64_t force_base, elf_image_info_t *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以指定 force_base 调用 elf_image_load_impl 装载 ELF 镜像。

**位置**：`kernel/mm/elf_image.c:446`

<a id="mod-kernel-mm-kmalloc.c"></a>

## kernel/mm/kmalloc.c — 内核内存分配器

`kernel/mm/kmalloc.c` · 4 个函数 · 4 个有说明 · 117 行

在 PMM 之上提供返回虚拟地址的分配接口。

现在 kmalloc 在每笔分配的第一页开头放一个 16 字节 header(magic +
页数 + 请求大小), 返回 base + 16。这样 kfree(ptr) 不再依赖调用方
传对 size：传小会泄漏, 传大会释放相邻页。

返回地址从"页对齐"改成"16 字节对齐"：当前调用者只要求自然对齐；
需要页对齐/DMA 的场景继续用 kalloc_pages()。

<a id="fn-kernel-mm-kmalloc.c-36"></a>

### `kalloc_pages`

```c
void *kalloc_pages(uint32_t pages)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 PMM 分配指定页数，失败返回 NULL，成功则转为虚拟地址并清零后返回。

**位置**：`kernel/mm/kmalloc.c:36`

<a id="fn-kernel-mm-kmalloc.c-47"></a>

### `kfree_pages`

```c
void kfree_pages(void *addr, uint32_t pages)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空指针直接返回，否则把虚拟地址转物理地址后交 PMM 释放，保留区保护由 PMM 负责。

<details>
<summary>函数内备注（1 条）</summary>

**第 52 行**

> 内核/rootfs/平台保留区的保护在 pmm_free_pages() 内部做(那里对所有
> 调用者生效)。这里以前有一份单独判断内核范围的副本, 已经删掉。

</details>

**位置**：`kernel/mm/kmalloc.c:47`

<a id="fn-kernel-mm-kmalloc.c-59"></a>

### `kmalloc`

```c
void *kmalloc(size_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按页分配并在头部记录魔数、页数与大小，返回 16 字节头之后的指针；零或过大尺寸返回 NULL。

**位置**：`kernel/mm/kmalloc.c:59`

<a id="fn-kernel-mm-kmalloc.c-81"></a>

### `kfree`

```c
void kfree(void *ptr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验页内偏移、魔数与头部一致性，通过后先清魔数再按记录页数释放，非法指针仅报错返回。

<details>
<summary>函数内备注（2 条）</summary>

**第 86 行**

> kmalloc 指针固定落在页内 offset 16。这个检查把 kalloc_pages 的
> 页对齐指针(offset 0)以及任意野指针挡在外面, 避免把 header
> 之前的 16 字节当成账本。

**第 110 行**

> 先毒化 magic 再释放。页如果还没被复用, 第二次 kfree 会在这里
> 被挡住；如果已经被复用成新的 kmalloc, 跨复用的 double free
> 仍然无法完全识别 —— 那需要 slab/元数据表, 后面再做。

</details>

**位置**：`kernel/mm/kmalloc.c:81`

<a id="mod-kernel-mm-mm_struct.c"></a>

## kernel/mm/mm_struct.c — 进程地址空间对象（mm_struct）

`kernel/mm/mm_struct.c` · 3 个函数 · 3 个有说明 · 93 行

从 task_t 拆出 pgd/heap/mmap/interp/exe_path，原因见
docs/task_t_refactor.md：
- CLONE_VM 线程共享同一份地址空间，fork 复制一份新的；
- “谁拥有 pgd、最后一个用户何时销毁”由 refcount/owns_pgd 决定，
不再依赖 task_t 的 is_thread / shares_pgd 两个手工标志；
- exec 换映像时只替换 mm，旧 mm 留给仍在共享它的线程。

静态池大小 = TASK_MAX + AVATAR_MAX_CPUS（每个 CPU 的 idle 任务
也有一份空 mm）。mm_put() 在锁外销毁页表（几百页，不能占着池锁）。

<a id="fn-kernel-mm-mm_struct.c-33"></a>

### `mm_alloc`

```c
mm_struct_t *mm_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁扫描 mm 池取引用计数为 0 的槽位，清零后置 1 返回，无空闲槽位返回 NULL。

**位置**：`kernel/mm/mm_struct.c:33`

<a id="fn-kernel-mm-mm_struct.c-52"></a>

### `mm_share`

```c
mm_struct_t *mm_share(mm_struct_t *mm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对非空 mm 在锁内递增引用计数并返回同一指针。

**位置**：`kernel/mm/mm_struct.c:52`

<a id="fn-kernel-mm-mm_struct.c-63"></a>

### `mm_put`

```c
void mm_put(mm_struct_t *mm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

锁内递减引用；最后一个引用且拥有 pgd 时摘出 pgd 并清空槽位，解锁后销毁对应用户页表。

<details>
<summary>函数内备注（1 条）</summary>

**第 78 行**

> 最后一个引用：先摘走 pgd 再把槽位标成空闲。销毁页表在锁外做，
> 但要先保存 pgd 本地副本 —— 解锁后槽位可能立刻被别人复用。
> 内核任务的 mm 也可能保存过内核 pgd（owns_pgd=false），不能销毁。

</details>

**位置**：`kernel/mm/mm_struct.c:63`

<a id="mod-kernel-mm-pmm.c"></a>

## kernel/mm/pmm.c — 物理内存管理器实现

`kernel/mm/pmm.c` · 5 个函数 · 5 个有说明 · 321 行

参考：ref/Avatar/kernel/mem/pmm.c

<a id="fn-kernel-mm-pmm.c-34"></a>

### `pmm_init`

```c
void pmm_init(pmm_t *pmm, uint64_t start_addr, uint64_t size, uint8_t *bitmap_buffer, size_t bitmap_size)
```

pmm_init - 初始化物理内存管理器
@pmm:           物理内存管理器实例
@start_addr:    物理内存起始地址
@size:          内存大小(字节)
@bitmap_buffer: 位图缓冲区
@bitmap_size:   位图大小(字节)

<details>
<summary>函数内备注（4 条）</summary>

**第 40 行**

> 初始化基本信息

**第 50 行**

> bitmap_init 的 size 单位是 bit。位图缓冲区按 PMM_BITMAP_MAX_BYTES
> 给(最多 4GB RAM), 但逻辑容量必须是 total_pages —— 多出来的
> 尾部位对 bitmap_find_* 来说全是&quot;空闲&quot;, next-fit 一旦越过 total_pages
> 就会把 RAM 之外的页当成可分配的页交出去。

**第 59 行**

> 初始化自旋锁

**第 62 行**

> 原来是六行结构体 dump —— 合并成一行：这是操作者真正要的那条内存事实

</details>

**位置**：`kernel/mm/pmm.c:34`

<a id="fn-kernel-mm-pmm.c-76"></a>

### `pmm_alloc_pages`

```c
uint64_t pmm_alloc_pages(pmm_t *pmm, uint32_t page_count)
```

pmm_alloc_pages - 分配连续的物理页面
@pmm:        物理内存管理器
@page_count: 需要分配的页面数

返回：分配的物理地址, 失败返回 0

<details>
<summary>函数内备注（5 条）</summary>

**第 85 行**

> 查找连续的空闲页面：先从上一次的落点找(next-fit), 失败再回绕。
> 位图的逻辑容量在 pmm_init() 里就是 total_pages, 所以查找结果天然
> 落在 [0, total_pages) 内, 不需要再按 total_pages 复核。

**第 94 行**

> 标记页面为已分配

**第 97 行**

> 计算物理地址

**第 100 行**

> 更新空闲页面计数与 next-fit 提示

**第 107 行**

> 日志放在解锁之后：klog 会持 g_klog_lock(关中断)把整条消息逐字符
> 轮询写 UART, 在 pmm-&gt;lock —— 全内核最热的锁 —— 里做这件事, 等于让
> 每个分配者多等一次串口输出。paddr == 0 即分配失败。

</details>

**位置**：`kernel/mm/pmm.c:76`

<a id="fn-kernel-mm-pmm.c-126"></a>

### `pmm_free_pages`

```c
bool pmm_free_pages(pmm_t *pmm, uint64_t paddr, uint32_t page_count)
```

pmm_free_pages - 释放物理页面
@pmm:        物理内存管理器
@paddr:      物理地址
@page_count: 页面数

<details>
<summary>函数内备注（4 条）</summary>

**第 136 行**

> 检查地址范围

**第 155 行**

> 保留区检查必须在清位之前：位图里&quot;保留&quot;和&quot;已分配&quot;都是 1, 单看位图
> 分不出来。误 free 内核/rootfs 页面时如果照常清位, 就等于把内核代码
> 重新交给了分配器 —— 这里拒绝整笔释放, 并且一个 bit 都不动。

**第 168 行**

> 整笔检查：只要有一页本来就是空闲的, 就是 double free。此时
> 一个 bit 都不清 —— 旧实现会&quot;释放一半, 把另一半当成已释放&quot;,
> 调用方拿不到任何错误, free_pages 的账从此漂移。

**第 190 行**

> 日志一律在解锁之后打(理由同 pmm_alloc_pages)

</details>

**位置**：`kernel/mm/pmm.c:126`

<a id="fn-kernel-mm-pmm.c-216"></a>

### `pmm_reserve`

```c
bool pmm_reserve(pmm_t *pmm, uint64_t start_addr, uint64_t end_addr, const char *tag)
```

pmm_reserve - 预留一段物理内存
@pmm:        物理内存管理器
@start_addr: 起始物理地址(含)
@end_addr:   结束物理地址(不含)
@tag:        保留区名字(静态生命周期)

位图标 1, 并记录到保留区表。之后 pmm_free_pages() 命中这张表会拒绝释放
且不清位 —— 位图本身区分不了"已分配"和"保留"。

<details>
<summary>函数内备注（3 条）</summary>

**第 221 行**

> 空区间：没有东西要预留, 不算失败(rootfs 大小为 0 的配置会走到)

**第 225 行**

> 半开区间向外取整：调用方给的首尾不必页对齐

**第 252 行**

> 仅统计 0-&gt;1 的位：区间重叠时 reserved_pages 不会重复计数

</details>

**位置**：`kernel/mm/pmm.c:216`

<a id="fn-kernel-mm-pmm.c-280"></a>

### `pmm_initialize`

```c
void pmm_initialize(uint64_t kernel_start_phys, uint64_t kernel_end_phys)
```

pmm_initialize - 初始化物理内存管理器
@kernel_start_phys: 内核镜像起始物理地址(含)
@kernel_end_phys:   内核镜像结束物理地址(不含)

必须在 kernel_main 中早期调用, 在任何内存分配之前。

内核镜像区间由 boot 层(kernel/main.c)从链接符号换算成物理地址后传入。
PMM 只认物理地址 —— 高半区/虚拟地址布局的知识不在这里。

<details>
<summary>函数内备注（5 条）</summary>

**第 284 行**

> 初始化 PMM

**第 287 行**

> 预留内核镜像：失败等于允许分配器把内核代码发出去, 不能继续启动

**第 295 行**

> 运行时保留区(由 platform_conf_scan() 从静态平台配置读取)。
> platform.conf 里的 stop 是闭区间, 这里转换成半开区间 [start, stop+1)。

**第 308 行**

> 预留 rootfs 物理区域, 防止 PMM 将其分配出去（平台内存布局，与具体块驱动无关）

**第 315 行**

> 一行说清最终账目：内核/rootfs/平台保留一共占了多少, 还剩多少

</details>

**位置**：`kernel/mm/pmm.c:280`

<a id="mod-kernel-mm-riscv64-gstage.c"></a>

## kernel/mm/riscv64/gstage.c — RISC-V H-extension G-stage (Stage-2) 实现

`kernel/mm/riscv64/gstage.c` · 14 个函数 · 14 个有说明 · 438 行

每 VM 一份 + 按需分页。老实现是一组全局页表 + identity map("先把 4 GiB
全映射再把非 RAM 清成无效"), 只能有一个 VM, 而且 guest RAM 必须先整段
预留。现在是：

- 每个 VM 一个 gstage_ctx_t；静态 root/L1 按 slot 索引(GSTAGE_MAX_VMS 份)；
- init 之后表是空的 —— "空表即全 trap", 不再需要 enable_mmio_trap；
- guest RAM 靠缺页驱动分配：没人预映射, guest 第一次访问某页时
VMM 在缺页处理里调 rv_gstage_map_page/block() 现分配一个宿主物理页；
- 宿主自己要写的几块(内核映像/DTB/initrd/初始栈)在加载期用
rv_gstage_map_range() 显式映射。

页表布局(Sv39x4)：
root: 2048 项 × 1 GiB(16 KiB), 本平台只用 root[0..3](低 4 GiB)
L1  : 4 × 512 项 × 2 MiB
L0  : 每个 2 MiB 块一张, 按需从 PMM 分配

记账不需要额外位图：l0_tbl[i] 非空就说明"该 2 MiB 块有页", 销毁时遍历
每张表的每个有效表项即可把按需页精确还回去(与 aarch64 的 stage2.c 同款)。

注意： CSR 一律用数值地址(hgatp = 0x680)+ 内联汇编。原因见 hext.h 顶部：
-march=rv64gc 下汇编器把 `hgatp` 这个名字静默映射到 VS 级编号(0x680 →
vsatp 之类), 编译全绿, 运行期才炸 —— 这是本项目在 riscv 上最贵的一课。

<a id="fn-kernel-mm-riscv64-gstage.c-78"></a>

### `root_index`

```c
static inline int root_index(uint64_t gpa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 Sv39x4 根表的 11 位掩码从 GPA 取根表项下标。

<details>
<summary>函数内备注（1 条）</summary>

**第 80 行**

> Sv39x4 的根索引是 11 位(2048 项), 不能复用 RV_PTE_INDEX 的 9 位掩码

</details>

**位置**：`kernel/mm/riscv64/gstage.c:78`

<a id="fn-kernel-mm-riscv64-gstage.c-83"></a>

### `l1_index`

```c
static inline int l1_index(uint64_t gpa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用通用 PTE 索引宏取 GPA 的 L1 页表下标。

**位置**：`kernel/mm/riscv64/gstage.c:83`

<a id="fn-kernel-mm-riscv64-gstage.c-87"></a>

### `l0_index`

```c
static inline int l0_index(uint64_t gpa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用通用 PTE 索引宏取 GPA 的 L0 页表下标。

**位置**：`kernel/mm/riscv64/gstage.c:87`

<a id="fn-kernel-mm-riscv64-gstage.c-99"></a>

### `ram_blk_index`

```c
static int ram_blk_index(const gstage_ctx_t *g, uint64_t gpa)
```

RAM 窗口内的 2 MiB 块序号(0..GSTAGE_MAX_L0_TABLES-1), 窗口外返回 -1。

注意： 这个序号故意与 L1 下标解耦：RAM 窗口起点不保证 1 GiB 对齐
(本平台 guest 用 0xA0000000, 落在 root[2] 的 L1[256] 而不是 L1[0]),
直接拿它当 L1 下标会算错。aarch64 那边同款函数有一样的说明。

**位置**：`kernel/mm/riscv64/gstage.c:99`

<a id="fn-kernel-mm-riscv64-gstage.c-114"></a>

### `l0_ensure`

```c
static uint64_t *l0_ensure(gstage_ctx_t *g, uint64_t gpa)
```

确保 gpa 所在的 2 MiB 块有一张 L0 表(没有就从 PMM 现分配)。
返回该表, 失败返回 NULL(PMM 没页, 或地址不在 RAM 窗口内)。

<details>
<summary>函数内备注（2 条）</summary>

**第 126 行**

> 越界检查必须在分配之前 —— 否则这条路径会泄露刚拿到的表页

**第 137 行**

> 在 L1 里装上指向 L0 的非叶项(V=1, R=W=X=0)

</details>

**位置**：`kernel/mm/riscv64/gstage.c:114`

<a id="fn-kernel-mm-riscv64-gstage.c-147"></a>

### `rv_gstage_vm_init`

```c
void rv_gstage_vm_init(gstage_ctx_t *g, uint32_t slot, uint32_t vmid, uint64_t ram_base, uint64_t ram_size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 slot 后清空 root 与 L1，建立 root 到 L1 的表结构并计算 hgatp，随后激活该 G-stage。

<details>
<summary>函数内备注（4 条）</summary>

**第 157 行（行尾）**

> memset 之后: 锁对象不能被清零

**第 159 行（行尾）**

> hgatp.VMID 是 14 位

**第 165 行**

> 空表 = 全 trap：没有一条有效映射, 任何 GPA 访问都陷入 HS-mode

**第 171 行**

> 注意： root 必须指向 L1 —— 这几条 entry 是&quot;表结构&quot;, 不是&quot;映射&quot;, 所以即使
> 一张空表也要有。漏掉的症状极具误导性：l0_ensure()/map_page() 只碰
> L1/L0, 看上去完全正常(第二次访问同一 GPA 能命中已有表项, 还回
> 同一个 PA), 但硬件从 root 就走进死胡同 —— guest 于是在同一条取指上
> 无限重复 fault。aarch64 那边就是这么踩过一次的
> (见 kernel/mm/aarch64/stage2.c 的 stage2_vm_init)。

</details>

**位置**：`kernel/mm/riscv64/gstage.c:147`

<a id="fn-kernel-mm-riscv64-gstage.c-195"></a>

### `rv_gstage_vm_destroy`

```c
void rv_gstage_vm_destroy(gstage_ctx_t *g)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

释放所有受管 L0 叶页与表页，清空 root 与 L1 并刷 G-stage TLB，最后清零上下文。

<details>
<summary>函数内备注（2 条）</summary>

**第 212 行（行尾）**

> 表页本身

**第 216 行**

> 先关掉映射(清表)再 flush —— 顺序反了会短暂留下指向已释放页的映射

</details>

**位置**：`kernel/mm/riscv64/gstage.c:195`

<a id="fn-kernel-mm-riscv64-gstage.c-229"></a>

### `rv_gstage_activate`

```c
void rv_gstage_activate(const gstage_ctx_t *g)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

当前 hgatp 与目标相同时直接返回，否则写入 hgatp、刷新 G-stage TLB 并打调试日志。

<details>
<summary>函数内备注（3 条）</summary>

**第 236 行**

> 幂等：hgatp 已经是这个值就直接返回。

> 本函数现在每次进 guest 前都会被调一次(见 hext_run.c 的
> vmm_arch_restore_guest_ctx)—— 因为 hgatp 是 per-hart 的, 而
> vmm_arch_vm_init() 是在 helper(/bin/vmm-run)所在的 hart 上跑的。若每次
> 都无条件写, 顺带的 hfence.gvma 会把整个 G-stage TLB 刷掉, 进 guest
> 的成本白白翻倍。

> 多 VM 下这个比较自然就分流了：本核可能刚跑过另一个 VM 的 vCPU 任务,
> 它的 hgatp 与本次不同 ⇒ 重写(这正是我们要的&quot;切 VM 时换页表&quot;)。

**第 252 行**

> READ/WRITE_HGATP 展开成数值 CSR 0x680(符号名会被汇编器映射错)

**第 256 行**

> 注意： DEBUG, 不是 INFO。本函数在每次进入 guest 前都会被调一次, 多 VM
> 时每轮都在两个 VM 的 hgatp 之间来回切 ⇒ 这行日志会以「每次 guest
> 入口一条」的速率刷屏。实测两个 VM 跑起来后它占了整个日志的 81%
> (670/825 行), 把真正有用的东西全冲掉了 —— 这是 klog 规范里
> 「INFO 不得进循环体」那条的典型案例。

</details>

**位置**：`kernel/mm/riscv64/gstage.c:229`

<a id="fn-kernel-mm-riscv64-gstage.c-268"></a>

### `rv_gstage_ipa_is_ram`

```c
int rv_gstage_ipa_is_ram(const gstage_ctx_t *g, uint64_t gpa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断 GPA 是否落在该 VM 的 RAM 窗口内。

**位置**：`kernel/mm/riscv64/gstage.c:268`

<a id="fn-kernel-mm-riscv64-gstage.c-273"></a>

### `rv_gstage_map_page`

```c
uint64_t rv_gstage_map_page(gstage_ctx_t *g, uint64_t gpa, int zero)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

锁内确保 L0 表存在并幂等映射一页，未映射时分配物理页并按需清零，失败返回 0。

<details>
<summary>函数内备注（2 条）</summary>

**第 288 行**

> 已映射：幂等(缺页重试, 以及&quot;先 map_range 后 fault&quot;都会走到)

**第 298 行（行尾）**

> 注意： 调用方必须处理, 不能当成功继续

</details>

**位置**：`kernel/mm/riscv64/gstage.c:273`

<a id="fn-kernel-mm-riscv64-gstage.c-321"></a>

### `rv_gstage_map_block`

```c
uint64_t rv_gstage_map_block(gstage_ctx_t *g, uint64_t gpa, int zero)
```

rv_gstage_map_block — 一次把 gpa 所在的整个 2 MiB 块装好(512 个 4 KiB 页)

为什么需要：按需分页下每次缺页都要一次完整的 HS-mode 往返(保存/恢复
vcpu 上下文 + PMM 分配 + 装表)。aarch64 那边实测把启动期的 1447 次缺页
压到 6 次；riscv 的 guest Linux 触摸的内存只多不少, 所以同一手法直接照搬。

代价是最多 2 MiB 的过取, 相对启动期的密集触碰可以接受。
块内 512 页是各自独立分配的, 不要求物理连续 —— 省下的是陷入次数,
不是分配开销。返回本次装上的页数。

<details>
<summary>函数内备注（3 条）</summary>

**第 329 行**

> 整块映射持锁完成：两个 vCPU 同时缺同一块时, 后者会看到前者装好的
> 表项而直接跳过 —— 不能出现「两边各建一张 L0 表」或「同一页两个 HPA」。
> PMM 分配与 512 页清零都在锁内, 时间上确实偏长, 但这是缺页路径且
> 每个 2 MiB 块只发生一次(后续块内访问全是命中)。

**第 347 行（行尾）**

> 已经映射过

**第 351 行（行尾）**

> PMM 没页：装多少算多少

</details>

**位置**：`kernel/mm/riscv64/gstage.c:321`

<a id="fn-kernel-mm-riscv64-gstage.c-364"></a>

### `rv_gstage_map_range`

```c
uint64_t rv_gstage_map_range(gstage_ctx_t *g, uint64_t gpa, uint64_t size, int zero)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐页建立映射，失败时报错并返回已映射页数，全部成功后在末尾统一刷 G-stage TLB。

<details>
<summary>函数内备注（1 条）</summary>

**第 380 行**

> 批量映射末尾统一刷一次；中途每页都刷会慢到不可用

</details>

**位置**：`kernel/mm/riscv64/gstage.c:364`

<a id="fn-kernel-mm-riscv64-gstage.c-386"></a>

### `rv_gstage_lookup`

```c
int rv_gstage_lookup(gstage_ctx_t *g, uint64_t gpa, uint64_t *pa_out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

锁内查 RAM 窗口内的 L0 叶项，命中时输出带页内偏移的物理地址并返回 1。

<details>
<summary>函数内备注（1 条）</summary>

**第 409 行（行尾）**

> 未映射, 或不是叶项

</details>

**位置**：`kernel/mm/riscv64/gstage.c:386`

<a id="fn-kernel-mm-riscv64-gstage.c-422"></a>

### `rv_gstage_tlb_flush`

```c
void rv_gstage_tlb_flush(const gstage_ctx_t *g)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 .insn 编码执行 hfence.gvma 全刷再执行 sfence.vma，并带编译器内存屏障。

<details>
<summary>函数内备注（2 条）</summary>

**第 426 行**

> hfence.gvma(全刷)+ sfence.vma。两个都是TLB 失效指令本身, 不是
> 通用内存屏障, barrier API 里没有对应物, 所以保留内联汇编。
> (别再拿&quot;统一屏障&quot;去替换它们 —— 换掉就丢掉了失效动作。)

> hfence.gvma 的助记符在 -march=rv64gc 下不被汇编器识别, 用 .insn 数值
> 编码：SYSTEM(0x73), funct3=0, funct7(HFENCE.GVMA)=0x31,
> rd=x0, rs1=x0(vaddr), rs2=x0(vmid) → 刷新全部 G-stage 映射。

**第 435 行（行尾）**

> hfence.gvma

</details>

**位置**：`kernel/mm/riscv64/gstage.c:422`

<a id="mod-kernel-mm-riscv64-vmm.c"></a>

## kernel/mm/riscv64/vmm.c — RISC-V Sv39 页表操作实现

`kernel/mm/riscv64/vmm.c` · 13 个函数 · 13 个有说明 · 268 行

提供 Sv39 三级页表(L2→L1→L0)的遍历, 映射与用户地址空间生命周期操作。

<a id="fn-kernel-mm-riscv64-vmm.c-24"></a>

### `rv_walk_l0_pte`

```c
uint64_t *rv_walk_l0_pte(void *page_dir, uint64_t vaddr, bool alloc)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从根表逐级下行，缺表时按 alloc 分配并清零子表，遇叶项返回 NULL，成功返回 L0 表项指针。

**位置**：`kernel/mm/riscv64/vmm.c:24`

<a id="fn-kernel-mm-riscv64-vmm.c-62"></a>

### `rv_perm_to_flags`

```c
uint64_t rv_perm_to_flags(vm_perm_t perm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把内核、内核 MMIO、用户三种权限分别转换为对应的读写执行与属性标志组合。

**位置**：`kernel/mm/riscv64/vmm.c:62`

<a id="fn-kernel-mm-riscv64-vmm.c-75"></a>

### `mm_vm_get_pte`

```c
uint64_t mm_vm_get_pte(void *page_dir, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

不分配地走到 L0 表项并返回其原始值，页表路径不存在时返回 0。

**位置**：`kernel/mm/riscv64/vmm.c:75`

<a id="fn-kernel-mm-riscv64-vmm.c-85"></a>

### `mm_vm_map_pages`

```c
int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr, uint64_t paddr, int32_t count, vm_perm_t perm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐页走到 L0 表项并写入物理地址、权限与有效位，已存在映射或分配失败返回 -1。

**位置**：`kernel/mm/riscv64/vmm.c:85`

<a id="fn-kernel-mm-riscv64-vmm.c-106"></a>

### `mm_vm_get_paddr`

```c
uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

查 L0 表项并确认是有效叶项后，返回物理地址加页内偏移，否则返回 0。

**位置**：`kernel/mm/riscv64/vmm.c:106`

<a id="fn-kernel-mm-riscv64-vmm.c-116"></a>

### `mm_vm_copy_to_uva`

```c
void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr, uint64_t src_paddr, uint64_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按页分段查目标物理页并把源物理页数据拷入，遇到未映射的目标地址提前返回。

**位置**：`kernel/mm/riscv64/vmm.c:116`

<a id="fn-kernel-mm-riscv64-vmm.c-141"></a>

### `rv_destroy_table`

```c
static void rv_destroy_table(uint64_t *table)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

递归释放用户页表：KEEP 只拆表、NOFREE 递减共享引用、普通页归还 PMM，并释放子表页。

<details>
<summary>函数内备注（1 条）</summary>

**第 150 行**

> 设备缓冲：页归 dma_buf/ION 句柄所有, 只拆表

</details>

**位置**：`kernel/mm/riscv64/vmm.c:141`

<a id="fn-kernel-mm-riscv64-vmm.c-174"></a>

### `mm_vm_verify_kernel_mappings`

```c
void mm_vm_verify_kernel_mappings(void)
```

启动自检：mmu.S 按 RV_KERNEL_MAP_LIST 建立的高半区映射必须每一条都在。
历史上漏掉 L1[0x103] 时内核自己跑得好好的, 直到 syscall 期间
satp 指向用户页表, 恰好在 0xc0000000 以上分配页面才 Store PF ——
那时 pc 在 syscall 里, 离根因非常远。这里在启动时就把它变成一条
带文件名/行号的 ASSERT。

**位置**：`kernel/mm/riscv64/vmm.c:174`

<a id="fn-kernel-mm-riscv64-vmm.c-185"></a>

### `mm_vm_init_user_pgd`

```c
void mm_vm_init_user_pgd(void *pgd, uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从当前 satp 取内核页表，按内核映射清单把高半区 L1 项复制进用户页表。

<details>
<summary>函数内备注（1 条）</summary>

**第 189 行**

> 用户页表必须包含内核高半区映射, 否则从 U 态陷入(ecall/中断/异常)
> 时 stvec(高地址)不可达, 会在陷入路径中失联。

> 复制哪些 L1 项由 RV_KERNEL_MAP_LIST(mem_layout.h)决定, 与启动时
> mmu.S 建立的映射是同一份列表, 这里不再逐条列举下标。
> 内核根表取 boot 时记录的那份, 而不是现读 satp —— 本函数可能在
> 用户地址空间下被调用。

</details>

**位置**：`kernel/mm/riscv64/vmm.c:185`

<a id="fn-kernel-mm-riscv64-vmm.c-204"></a>

### `mm_vm_destroy_user_space`

```c
void mm_vm_destroy_user_space(uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

只销毁用户半区的 L1 项及其子树，保留共享的内核映射，最后释放根页表页。

<details>
<summary>函数内备注（1 条）</summary>

**第 211 行**

> 只释放用户半区。高半区 L1[FIRST_IDX..] 是从内核页表复制的共享映射。

</details>

**位置**：`kernel/mm/riscv64/vmm.c:204`

<a id="fn-kernel-mm-riscv64-vmm.c-230"></a>

### `mm_vm_unmap_page`

```c
int mm_vm_unmap_page(void *pgd, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

查有效叶项后按 KEEP、NOFREE、普通页分别处理并清零表项，返回 1，否则返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 237 行**

> 设备缓冲：页归 dma_buf/ION 句柄所有, 只拆映射

</details>

**位置**：`kernel/mm/riscv64/vmm.c:230`

<a id="fn-kernel-mm-riscv64-vmm.c-247"></a>

### `mm_vm_flush_tlb_user`

```c
void mm_vm_flush_tlb_user(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

执行一条 sfence.vma 全量刷新用户 TLB。

**位置**：`kernel/mm/riscv64/vmm.c:247`

<a id="fn-kernel-mm-riscv64-vmm.c-252"></a>

### `mm_vm_switch_user`

```c
void mm_vm_switch_user(uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写 satp 后依次执行 sfence.vma 与 fence.i，切换页表并同步新映像的指令缓存。

<details>
<summary>函数内备注（1 条）</summary>

**第 256 行**

> execve 原地换页表。用户根页表包含内核高半区映射，切 satp 后当前
> 内核栈/代码仍可达。

> fence.i 是这里必须的：新映像的代码页是内核刚 memcpy 进去的，而
> trap 返回路径(boot/riscv64/exception.S 的 sret)不会同步 I-cache。
> 以前每次 exec 都新建任务、首次进入走 arch_switch_to_user，那里有
> fence.i；原地 exec 改走 trap 返回，必须自己补上。

</details>

**位置**：`kernel/mm/riscv64/vmm.c:252`

<a id="mod-kernel-mm-shared_page.c"></a>

## kernel/mm/shared_page.c — MAP_SHARED 物理页引用计数

`kernel/mm/shared_page.c` · 5 个函数 · 5 个有说明 · 182 行

这张表会被 mmap, fork, munmap, 进程退出四条路径, 多个 CPU 并发访问,
所以用一把 IRQ-safe 自旋锁保护；并用哈希桶代替线性扫描：fork 一个带
大量共享页的进程时, 每页一次 ref, 线性扫描会退化成 O(n^2)。

表满时 ref 返回 false, 不静默丢弃引用。旧实现里表满后调用方照样
把页标成 NOFREE, 而 unref 找不到记录时又会直接 pmm_free_pages() ——
等于把一个还有别的进程在用的页还给了分配器。现在 unref 对未知地址
只报错, 不释放：找不到账本说明引用记账已经坏了, 继续 free 只会把数据
损坏推迟到更难查的时候。

<a id="fn-kernel-mm-shared_page.c-48"></a>

### `shared_pages_init_locked`

```c
static void shared_pages_init_locked(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

首次调用时把桶清为 -1 并把所有记录串成空闲链表，置初始化标志；已初始化则直接返回。

**位置**：`kernel/mm/shared_page.c:48`

<a id="fn-kernel-mm-shared_page.c-65"></a>

### `shared_page_bucket`

```c
static uint32_t shared_page_bucket(uint64_t paddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把物理页号混合散列后取低位，得到该页记录所在的桶号。

<details>
<summary>函数内备注（1 条）</summary>

**第 67 行**

> 页号混合一下再取低位。物理页号通常很密集, 直接 &amp; 掩码也能用；
> 这里多一步乘法是为了避免低地址的大段连续页挤在少数桶里。

</details>

**位置**：`kernel/mm/shared_page.c:65`

<a id="fn-kernel-mm-shared_page.c-79"></a>

### `shared_page_find_locked`

```c
static int32_t shared_page_find_locked(uint64_t paddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在对应桶的链表中线性查找给定物理页，找到返回记录下标，否则返回 -1。

**位置**：`kernel/mm/shared_page.c:79`

<a id="fn-kernel-mm-shared_page.c-90"></a>

### `shared_page_ref`

```c
bool shared_page_ref(uint64_t paddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验页对齐后锁内查找或新建记录并递增引用计数，记录耗尽或计数饱和时返回 false。

**位置**：`kernel/mm/shared_page.c:90`

<a id="fn-kernel-mm-shared_page.c-129"></a>

### `shared_page_unref`

```c
void shared_page_unref(uint64_t paddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

锁内递减引用，最后一个持有者时摘除记录并在持锁状态下释放物理页；未登记或未对齐页仅报错。

<details>
<summary>函数内备注（2 条）</summary>

**第 163 行**

> 最后一个持有者：先把账本摘掉, 再释放物理页。

**第 174 行**

> 持锁释放：否则&quot;摘账本 → 解锁 → free&quot;之间, 另一个 CPU 可能为同一
> paddr 新建一条记录, 然后这个页被我们释放掉 —— 账本指向已回收的页。
> 锁序固定为 shared_page -&gt; pmm, PMM 路径不会反向调用本模块。

</details>

**位置**：`kernel/mm/shared_page.c:129`

<a id="mod-kernel-mm-vm_user.c"></a>

## kernel/mm/vm_user.c — 用户进程虚拟内存管理

`kernel/mm/vm_user.c` · 5 个函数 · 3 个有说明 · 172 行

架构无关实现, 依赖 mm_vm.h 提供的抽象接口。
各架构在其 include/arch/<arch>/mm_vm.h 中提供具体实现或 stub。

<a id="fn-kernel-mm-vm_user.c-25"></a>

### `vm_create_user_process`

```c
uint64_t vm_create_user_process(uint64_t user_code_vaddr, uint64_t user_code_size, uint64_t user_stack_top, uint64_t user_stack_size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配清零 PGD，逐页映射代码段与栈并拷入代码，成功返回 PGD 物理地址，任一步失败走完整销毁并返回 0。

<details>
<summary>函数内备注（7 条）</summary>

**第 28 行**

> 分配并清零页目录(PGD)

**第 37 行**

> 架构要求的内核映射注入(RISC-V/x86 复制内核高半区, AArch64 为空操作)。
> 具体实现在 kernel/mm/&lt;arch&gt;/vmm.c, 根目录不出现架构条件编译。

**第 49 行**

> ── 映射用户代码段 ───────────────────────────────────────────

**第 69 行**

> ── 映射用户栈 ───────────────────────────────────────────────

**第 87 行**

> ── 拷贝用户代码到用户地址空间 ─────────────────────────────────

**第 95 行**

> 验证：读取前 16 字节

**第 110 行**

> 失败时可能已经映射了代码/栈的一部分, 甚至已经分配了中间页表。
> 只 free PGD 会把这些页全部泄漏；走完整的销毁路径。

</details>

**位置**：`kernel/mm/vm_user.c:25`

<a id="fn-kernel-mm-vm_user.c-118"></a>

### `vm_destroy_user_process`

```c
void vm_destroy_user_process(uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对非零 PGD 调用架构钩子销毁整棵用户页表及叶页。

<details>
<summary>函数内备注（1 条）</summary>

**第 124 行**

> 整棵页表, leaf 页与共享引用都由架构钩子回收(含 PGD 自身)。

</details>

**位置**：`kernel/mm/vm_user.c:118`

<a id="fn-kernel-mm-vm_user.c-128"></a>

### `vm_unmap_user_range`

```c
uint64_t vm_unmap_user_range(uint64_t pgd_phys, uint64_t vaddr, uint64_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

页对齐遍历范围内每页调用架构 unmap 并统计释放数，有释放则刷新用户 TLB 后返回数量。

**位置**：`kernel/mm/vm_user.c:128`

<a id="fn-kernel-mm-vm_user.c-162"></a>

### `mm_vm_kernel_root_init`

```c
void mm_vm_kernel_root_init(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/mm/vm_user.c:162`

<a id="fn-kernel-mm-vm_user.c-168"></a>

### `mm_vm_kernel_root_phys`

```c
uint64_t mm_vm_kernel_root_phys(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/mm/vm_user.c:168`

<a id="mod-kernel-mm-x86_64-ept.c"></a>

## kernel/mm/x86_64/ept.c — x86_64 EPT (Extended Page Table) 实现

`kernel/mm/x86_64/ept.c` · 15 个函数 · 15 个有说明 · 404 行

移植自 x-kernel: virt/kvmm/src/mm/ept.rs, 适配 Avatar OS。

── 每 VM 一份 + 按需分页 ────────────────────────────────────

老实现是一组全局页表 + 全 4 GiB 预建 identity map("先全映射再把非 RAM 清
成无效"), 只能有一个 VM, 而且 guest RAM 必须先整段预留。现在是：

- 每个 VM 一个 ept_ctx_t；静态 PML4/PDPT/PD 按 slot 索引(EPT_MAX_VMS 份)；
- init 之后表是空的 —— "空表即全 trap", 不再需要 enable_mmio_trap；
- guest RAM 靠缺页驱动分配：没人预映射, guest 第一次访问某页时
VMM 在 EPT violation 处理里调 x86_ept_map_page/block() 现分配一个宿主
物理页；
- 宿主自己要写的几块(bzImage/initrd/boot_params/初始页表)在加载期用
x86_ept_map_range() 显式映射。

页表布局(4 级)：
PML4[0] → PDPT
PDPT[i] (i<4, 每项 1 GiB) → PD[i]
PD[i][j] (512 项 × 2 MiB) —— guest RAM 窗口内的那几项是"指向 PT 的表项",
窗口外一律留空(访问 → EPT violation)
PT：每个 2 MiB 块一张, 按需从 PMM 分配, 叶项是 4 KiB 页

记账不需要额外位图：pt_tbl[i] 非空就说明"该 2 MiB 块有页", 销毁时遍历每张
表的每个有效表项即可把按需页精确还回去(与 stage2.c / gstage.c 同款)。

注意： x86 与另外两个架构有一处语义差异值得记一笔：guest 的 GPA 必须从 0
开始(Linux 假定低端有常规内存, 内核装载在 1 MiB), 而宿主的物理 0 不能给
它。老实现靠 EPT 的线性偏移(hpa = hpa_base + gpa)一次解决；按需分页之后
每个 GPA 各自映射到一个现分配的宿主页, 这个约束自然满足, 不再需要
hpa_base**。

<a id="fn-kernel-mm-x86_64-ept.c-77"></a>

### `pdpt_index`

```c
static inline int pdpt_index(uint64_t gpa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 GPA 中取 9 位 PDPT 页表下标。

**位置**：`kernel/mm/x86_64/ept.c:77`

<a id="fn-kernel-mm-x86_64-ept.c-81"></a>

### `pd_index`

```c
static inline int pd_index(uint64_t gpa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 GPA 中取 9 位 PD 页表下标。

**位置**：`kernel/mm/x86_64/ept.c:81`

<a id="fn-kernel-mm-x86_64-ept.c-85"></a>

### `pt_index`

```c
static inline int pt_index(uint64_t gpa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 GPA 中取 9 位 PT 页表下标。

**位置**：`kernel/mm/x86_64/ept.c:85`

<a id="fn-kernel-mm-x86_64-ept.c-97"></a>

### `ram_blk_index`

```c
static int ram_blk_index(const ept_ctx_t *e, uint64_t gpa)
```

RAM 窗口内的 2 MiB 块序号(0..EPT_MAX_PT_TABLES-1), 窗口外返回 -1。

注意： 这个序号故意与 PD 下标解耦：RAM 窗口起点不保证 1 GiB 对齐
(aarch64 的 guest 在 0x70000000, riscv 在 0xA0000000；x86 虽然从 0 开始,
但保持同一形状, 省得三份代码长得不一样)。直接拿它当 PD 下标会算错。

**位置**：`kernel/mm/x86_64/ept.c:97`

<a id="fn-kernel-mm-x86_64-ept.c-112"></a>

### `pt_ensure`

```c
static uint64_t *pt_ensure(ept_ctx_t *e, uint64_t gpa)
```

确保 gpa 所在的 2 MiB 块有一张 PT(没有就从 PMM 现分配)。
返回该表, 失败返回 NULL(PMM 没页, 或地址不在 RAM 窗口内)。

<details>
<summary>函数内备注（1 条）</summary>

**第 130 行**

> 在 PD 里装上指向 PT 的表项(RWX 但不设 EPT_LARGE：这是表不是大页)

</details>

**位置**：`kernel/mm/x86_64/ept.c:112`

<a id="fn-kernel-mm-x86_64-ept.c-140"></a>

### `ept_leaf`

```c
static inline uint64_t ept_leaf(uint64_t pa)
```

叶项：4 KiB 页, RAM 语义(WB, 可读可写可执行)

**位置**：`kernel/mm/x86_64/ept.c:140`

<a id="fn-kernel-mm-x86_64-ept.c-147"></a>

### `x86_ept_vm_init`

```c
void x86_ept_vm_init(ept_ctx_t *e, uint32_t slot, uint64_t ram_base, uint64_t ram_size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 slot 后清空 EPT 各级表，建立 PML4 到 PDPT 再到 PD 的表结构并计算 EPTP。

<details>
<summary>函数内备注（3 条）</summary>

**第 165 行**

> 空表 = 全 trap：没有一条有效映射, 任何 GPA 访问都触发 EPT violation

**第 172 行**

> 注意： PML4→PDPT→PD 这几条是&quot;表结构&quot;, 不是&quot;映射&quot;, 所以即使一张空表也要有。
> 漏掉的症状极具误导性：pt_ensure()/map_page() 只碰 PD/PT, 看上去完全
> 正常(第二次访问同一 GPA 能命中已有表项, 还回同一个 HPA), 但硬件从
> PML4 就走进了死胡同 —— guest 于是在同一条取指上无限重复 fault。
> aarch64 那边就是这么踩过一次的(见 kernel/mm/aarch64/stage2.c)。

> 保留 4 条而不是只建 RAM 窗口那一条：窗口外的 GPA(设备 0xFEC00000 之类)
> 因此会走到 PD 级才判定无效, 与老实现的&quot;走到 PD 再发现项是 0&quot;一致。

**第 186 行**

> root_pa | (page_walk_length-1)&lt;&lt;3 | memory_type(4 级 → 3；WB → 6)

</details>

**位置**：`kernel/mm/x86_64/ept.c:147`

<a id="fn-kernel-mm-x86_64-ept.c-193"></a>

### `x86_ept_vm_destroy`

```c
void x86_ept_vm_destroy(ept_ctx_t *e)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

释放受管叶页与各 PT 表页，清空表后执行 INVEPT，并清零整个 EPT 上下文。

<details>
<summary>函数内备注（3 条）</summary>

**第 213 行（行尾）**

> 表页本身

**第 217 行**

> 先关掉映射(清表)再刷 —— 顺序反了会短暂留下指向已释放页的映射

**第 228 行**

> eptp 一并清掉：留着的话下一次 INVEPT 会拿一个已经被清空的 pml4 去刷。
> 清空之后 x86_ept_invept_all() 会因为 eptp==0 直接返回。

</details>

**位置**：`kernel/mm/x86_64/ept.c:193`

<a id="fn-kernel-mm-x86_64-ept.c-235"></a>

### `x86_ept_eptp`

```c
uint64_t x86_ept_eptp(const ept_ctx_t *e)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 EPT 上下文的 EPTP 值，上下文为空指针时返回 0。

**位置**：`kernel/mm/x86_64/ept.c:235`

<a id="fn-kernel-mm-x86_64-ept.c-242"></a>

### `x86_ept_ipa_is_ram`

```c
int x86_ept_ipa_is_ram(const ept_ctx_t *e, uint64_t gpa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断 GPA 是否落在该 VM 的 RAM 窗口内。

**位置**：`kernel/mm/x86_64/ept.c:242`

<a id="fn-kernel-mm-x86_64-ept.c-247"></a>

### `x86_ept_map_page`

```c
uint64_t x86_ept_map_page(ept_ctx_t *e, uint64_t gpa, int zero)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

锁内确保 PT 存在并幂等映射一页，未映射时分配物理页并按需清零，返回 HPA 或 0。

<details>
<summary>函数内备注（2 条）</summary>

**第 261 行**

> 已映射：幂等(缺页重试, 以及&quot;先 map_range 后 fault&quot;都会走到)

**第 271 行（行尾）**

> 注意： 调用方必须处理, 不能当成功继续

</details>

**位置**：`kernel/mm/x86_64/ept.c:247`

<a id="fn-kernel-mm-x86_64-ept.c-282"></a>

### `x86_ept_map_block`

```c
uint64_t x86_ept_map_block(ept_ctx_t *e, uint64_t gpa, int zero)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

锁内确保 2MiB 块对应的 PT，为块内每个未映射页分配并安装叶项，返回新装页数。

<details>
<summary>函数内备注（3 条）</summary>

**第 290 行**

> 整个「确保 PT → 装 512 个叶项」必须在 map_lock 下完成：两颗 vCPU
> 同时缺页到同一个 2 MiB 块时, 都看到 pt_tbl[idx]==NULL → 各建一张
> PT、各装一套页, 后者覆盖前者的 PD 项。被覆盖的那组页从 guest
> 视角看就是内存被换成另一组清零页(aarch64 上实测表现为 qspinlock
> owner &quot;消失&quot;, 见 MULTI_VCPU.md §5.3)。

**第 308 行（行尾）**

> 已经映射过

**第 312 行（行尾）**

> PMM 没页：装多少算多少

</details>

**位置**：`kernel/mm/x86_64/ept.c:282`

<a id="fn-kernel-mm-x86_64-ept.c-324"></a>

### `x86_ept_map_range`

```c
uint64_t x86_ept_map_range(ept_ctx_t *e, uint64_t gpa, uint64_t size, int zero)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐页建立映射，失败时报错并返回已映射页数，全部成功后在末尾统一执行 INVEPT。

<details>
<summary>函数内备注（1 条）</summary>

**第 340 行**

> 批量映射末尾统一刷一次；中途每页都刷会慢到不可用

</details>

**位置**：`kernel/mm/x86_64/ept.c:324`

<a id="fn-kernel-mm-x86_64-ept.c-346"></a>

### `x86_ept_lookup`

```c
int x86_ept_lookup(const ept_ctx_t *e, uint64_t gpa, uint64_t *hpa_out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

查 RAM 窗口内的 PT 叶项，命中时输出带页内偏移的 HPA 并返回 1。

**位置**：`kernel/mm/x86_64/ept.c:346`

<a id="fn-kernel-mm-x86_64-ept.c-372"></a>

### `x86_ept_invept_all`

```c
void x86_ept_invept_all(const ept_ctx_t *e)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用单 context 描述符和手写字节执行 INVEPT 刷新 EPT TLB，EPTP 为 0 时直接返回。

<details>
<summary>函数内备注（5 条）</summary>

**第 374 行**

> INVEPT —— 刷新 EPT 派生 TLB。

> 工具链不认 `invept` 助记符(需要特定 binutils 配置), 手工写编码：
> 66 0F 38 80 /r
> /r 的 reg 字段 = 类型(1=单 context, 2=全 context),
> rm 字段 = 16 字节描述符(低 8 字节是 EPTP)。
> 这里生成 `invept %rax,(%rdx)` → ModRM=0x02。

**第 387 行（行尾）**

> single-context：只刷本 VM 这个 EPTP

**第 390 行（行尾）**

> 还没配置(或已销毁)：没有 TLB 要刷

**第 395 行**

> 注意： INVEPT 是传统编码 + 强制 66 前缀(SDM: 66 0F 38 80 /r),
> 没有 VEX 形式。最初写成 VEX(C4 E2 79 80)直接吃了个 #6
> invalid opcode —— 那套字节是给别的指令用的。

**第 400 行（行尾）**

> invept %rax,(%rdx)

</details>

**位置**：`kernel/mm/x86_64/ept.c:372`

<a id="mod-kernel-mm-x86_64-vmm.c"></a>

## kernel/mm/x86_64/vmm.c — x86_64 4 级页表操作实现

`kernel/mm/x86_64/vmm.c` · 13 个函数 · 13 个有说明 · 279 行

提供 PML4 → PDPT → PD → PT 四级页表的遍历, 映射, 复制等操作。

<a id="fn-kernel-mm-x86_64-vmm.c-22"></a>

### `x86_walk_pt`

```c
uint64_t *x86_walk_pt(void *page_dir, uint64_t vaddr, bool alloc)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 PML4 逐级下行到 PT，缺表时按 alloc 分配并清零，遇大页或分配失败返回 NULL。

<details>
<summary>函数内备注（4 条）</summary>

**第 30 行**

> Level 4: PML4 → PDPT

**第 43 行**

> Level 3: PDPT → PD

**第 57 行**

> Level 2: PD → PT

**第 71 行**

> Level 1: PT → Page

</details>

**位置**：`kernel/mm/x86_64/vmm.c:22`

<a id="fn-kernel-mm-x86_64-vmm.c-78"></a>

### `x86_perm_to_flags`

```c
uint64_t x86_perm_to_flags(vm_perm_t perm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把内核、内核 MMIO、用户权限转换为含 NX、PCD、PWT、USER 等位的 PTE 标志。

**位置**：`kernel/mm/x86_64/vmm.c:78`

<a id="fn-kernel-mm-x86_64-vmm.c-91"></a>

### `mm_vm_map_pages`

```c
int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr, uint64_t paddr, int32_t count, vm_perm_t perm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐页走到 PT 并写入物理地址与权限位，已存在映射或分配失败返回 -1。

**位置**：`kernel/mm/x86_64/vmm.c:91`

<a id="fn-kernel-mm-x86_64-vmm.c-113"></a>

### `mm_vm_get_paddr`

```c
uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

查 PT 项且有效时返回页物理地址加页内偏移，否则返回 0。

**位置**：`kernel/mm/x86_64/vmm.c:113`

<a id="fn-kernel-mm-x86_64-vmm.c-123"></a>

### `x86_copy_pt_recursive`

```c
static int x86_copy_pt_recursive(uint64_t *src, uint64_t *dst, int level)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

递归复制页表：内核区不复制，大页与 KEEP 原样共享，NOFREE 加引用，其余叶页复制新页。

<details>
<summary>函数内备注（1 条）</summary>

**第 138 行**

> KEEP(设备缓冲)同样原样复制, 但不加引用 —— 页归句柄所有

</details>

**位置**：`kernel/mm/x86_64/vmm.c:123`

<a id="fn-kernel-mm-x86_64-vmm.c-166"></a>

### `mm_vm_copy_user_space`

```c
int32_t mm_vm_copy_user_space(void *dst_pgd, void *src_pgd)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从源 PML4 到目标递归复制用户页表，返回底层复制结果。

**位置**：`kernel/mm/x86_64/vmm.c:166`

<a id="fn-kernel-mm-x86_64-vmm.c-173"></a>

### `mm_vm_copy_to_uva`

```c
void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr, uint64_t src_paddr, uint64_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按页分段查目标物理页并把源数据拷入，遇到未映射地址时提前返回。

**位置**：`kernel/mm/x86_64/vmm.c:173`

<a id="fn-kernel-mm-x86_64-vmm.c-199"></a>

### `x86_destroy_table`

```c
static void x86_destroy_table(uint64_t *table, int level)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

递归释放页表：KEEP 只拆表、NOFREE 递减共享引用、普通页归还 PMM，并释放子表页。

<details>
<summary>函数内备注（1 条）</summary>

**第 208 行**

> 设备缓冲：页归 dma_buf/ION 句柄所有, 只拆表

</details>

**位置**：`kernel/mm/x86_64/vmm.c:199`

<a id="fn-kernel-mm-x86_64-vmm.c-225"></a>

### `mm_vm_init_user_pgd`

```c
void mm_vm_init_user_pgd(void *pgd, uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用内核 PML4 物理地址把内核高区表项复制进用户 PGD，内核 PML4 未初始化时报错。

**位置**：`kernel/mm/x86_64/vmm.c:225`

<a id="fn-kernel-mm-x86_64-vmm.c-233"></a>

### `mm_vm_destroy_user_space`

```c
void mm_vm_destroy_user_space(uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

只销毁用户 PML4 项对应的子树，保留内核项，最后释放 PML4 页。

**位置**：`kernel/mm/x86_64/vmm.c:233`

<a id="fn-kernel-mm-x86_64-vmm.c-252"></a>

### `mm_vm_unmap_page`

```c
int mm_vm_unmap_page(void *pgd, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

查有效 PTE 后按 KEEP、NOFREE、普通页分别处理、清零表项并 invlpg，返回 1，否则 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 259 行**

> 设备缓冲：页归 dma_buf/ION 句柄所有, 只拆映射

</details>

**位置**：`kernel/mm/x86_64/vmm.c:252`

<a id="fn-kernel-mm-x86_64-vmm.c-270"></a>

### `mm_vm_flush_tlb_user`

```c
void mm_vm_flush_tlb_user(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，因为逐页 unmap 时已执行 invlpg，无需额外全刷。

<details>
<summary>函数内备注（1 条）</summary>

**第 272 行**

> mm_vm_unmap_page() 已经逐页 invlpg, 不需要额外的全刷。

</details>

**位置**：`kernel/mm/x86_64/vmm.c:270`

<a id="fn-kernel-mm-x86_64-vmm.c-275"></a>

### `mm_vm_switch_user`

```c
void mm_vm_switch_user(uint64_t pgd_phys)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接写 CR3 切换用户页表，未启用 PCID 因此等同全量刷新 TLB。

<details>
<summary>函数内备注（1 条）</summary>

**第 277 行**

> execve 原地换页表。没启用 PCID，写 CR3 就是整 TLB 刷新。

</details>

**位置**：`kernel/mm/x86_64/vmm.c:275`

<a id="mod-kernel-net-bwtest.c"></a>

## kernel/net/bwtest.c — 以太网带宽测试：UDP 丢弃式接收端 + 每秒速率统计

`kernel/net/bwtest.c` · 6 个函数 · 6 个有说明 · 241 行

── 为什么用 UDP 而不是 TCP ──────────────────────────────────────────────
kernel/net/lwip_port/lwipopts.h 里
TCP_WND     = 4 * TCP_MSS = 5840 字节
TCP_SND_BUF = 4 * TCP_MSS = 5840 字节
收发窗口都只有 5840 字节, TCP 吞吐被"在途字节数 / RTT"卡死在几 MB/s 量级 ——
拿 TCP 流去测, 量到的是窗口大小, 不是以太网和驱动本身的能力。
UDP 没有窗口也没有 ACK 往返, 才能真正把线打满, 暴露 RX 环深度,
轮询节奏, 拷贝开销这些真实瓶颈。

── 为什么直接调 kprintf 而不走 KLOG_* ───────────────────────────────────
测性能必须用 LOG=none 构建：收包热路径上的 KLOG(kernel/net/netdev.c 与
kernel/net/lwip_port/netif_avatar.c 每 32 帧各打一条)走的是 SG2002 的
同步串口路径 driver/uart/uart_dw.c 的 sg2002_dbg_putc(), 逐字节 busy-wait,
每 32 帧要阻塞 ~7.6ms, 而同样 32 帧在线上只要 3.9ms —— 日志本身就是瓶颈。
但 LOG=none 会把 KLOG_* 编译期抹成 do{}while(0), 所以本模块的统计输出
自己调 kprintf(include/klog.h), 这样 LOG=none 构建里依然可见。

── 报文格式 ────────────────────────────────────────────────────────────
每条 UDP datagram 开头 8 字节头, 其后是任意填充(内容不解析)：
offset 0..3: magic = 0x41564257 ("AVBW"), 小端
offset 4..7: seq   从 0 递增的序号, 小端
带 magic 是为了不把链路上的杂包(ARP / IPv6 / DHCP 广播)混进统计。

PC 侧(PowerShell, 无需装任何软件)：

$ip='192.168.7.1'; $port=1235; $n=50000; $size=1400
$c = New-Object Net.Sockets.UdpClient
$c.Connect($ip, $port)
$buf = New-Object byte[] $size
[BitConverter]::GetBytes([uint32]0x41564257).CopyTo($buf, 0)
$sw = [Diagnostics.Stopwatch]::StartNew()
for ($i=0; $i -lt $n; $i++) {
[BitConverter]::GetBytes([uint32]$i).CopyTo($buf, 4)
[void]$c.Send($buf, $size)
}
$sw.Stop()
$mb = $n * $size / 1MB
"发送 {0:N1} MB / {1:N2} s = {2:N2} MB/s ({3:N1} Mbps)" -f `
$mb, $sw.Elapsed.TotalSeconds, ($mb/$sw.Elapsed.TotalSeconds), `
($mb*8/$sw.Elapsed.TotalSeconds)
$c.Close()

注意：Windows 每发送一条就要做一次 sendto 系统调用, 1400 字节/包时
100 Mbps 只需要约 9000 包/秒, PowerShell 的循环开销远够用, 不会成为瓶颈。

<a id="fn-kernel-net-bwtest.c-94"></a>

### `bw_rd32le`

```c
static inline uint32_t bw_rd32le(const uint8_t *p)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按小端顺序从 4 个字节拼出 32 位整数。

**位置**：`kernel/net/bwtest.c:94`

<a id="fn-kernel-net-bwtest.c-99"></a>

### `bw_reset_window`

```c
static void bw_reset_window(uint64_t now_ms)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清零当前统计窗口的包数、字节数、首末序号、乱序数与起始时间。

**位置**：`kernel/net/bwtest.c:99`

<a id="fn-kernel-net-bwtest.c-111"></a>

### `bw_report`

```c
static void bw_report(uint32_t window_ms)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由窗口统计计算丢失数、MB/s、Mbps 与平均包长，并入累计值后打印一行报告。

<details>
<summary>函数内备注（2 条）</summary>

**第 118 行**

> 序号从 0 连续递增时, [first, max] 之间应有的包数减去实收即为丢失

**第 129 行**

> 先并入累计再打印 —— 否则同一行里窗口 lost 和累计 lost 会差一个窗口,
> 看起来像是&quot;丢了 14242 但累计一个没丢&quot;, 很误导。

</details>

**位置**：`kernel/net/bwtest.c:111`

<a id="fn-kernel-net-bwtest.c-144"></a>

### `bwtest_poll`

```c
void bwtest_poll(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未运行时直接返回；距上次报告满间隔则报告并重置窗口，连续空窗口达上限后停止统计。

<details>
<summary>函数内备注（2 条）</summary>

**第 147 行（行尾）**

> 快路径：一次 load

**第 157 行**

> 空窗口：发送端可能已经停了, 连着几个空窗口就收工不再刷屏

</details>

**位置**：`kernel/net/bwtest.c:144`

<a id="fn-kernel-net-bwtest.c-171"></a>

### `bw_recv`

```c
static void bw_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验魔数后累计包数与字节数并更新序号乱序统计，首包时开启统计，最后释放 pbuf。

<details>
<summary>函数内备注（1 条）</summary>

**第 187 行**

> pbuf 可能是链式的, 用 pbuf_copy_partial 取头, 不假设内存连续

</details>

**位置**：`kernel/net/bwtest.c:171`

<a id="fn-kernel-net-bwtest.c-223"></a>

### `bwtest_init`

```c
void bwtest_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

创建 UDP PCB、绑定带宽测试端口并注册接收回调，失败时清理并报错。

**位置**：`kernel/net/bwtest.c:223`

<a id="mod-kernel-net-dhcp_server.c"></a>

## kernel/net/dhcp_server.c

`kernel/net/dhcp_server.c` · 10 个函数 · 10 个有说明 · 235 行

<a id="fn-kernel-net-dhcp_server.c-54"></a>

### `put_be32`

```c
static void put_be32(uint8_t *p, uint32_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 32 位值按大端顺序写入 4 字节缓冲。

**位置**：`kernel/net/dhcp_server.c:54`

<a id="fn-kernel-net-dhcp_server.c-62"></a>

### `ip4_to_be32`

```c
static uint32_t ip4_to_be32(const ip4_addr_t *ip)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 lwIP 的 ip4 地址按 32 位大端序重新排列返回。

**位置**：`kernel/net/dhcp_server.c:62`

<a id="fn-kernel-net-dhcp_server.c-69"></a>

### `put_ip`

```c
static void put_ip(uint8_t *p, const ip4_addr_t *ip)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

经 ip4_to_be32 把 IP 地址以大端序写入缓冲。

**位置**：`kernel/net/dhcp_server.c:69`

<a id="fn-kernel-net-dhcp_server.c-74"></a>

### `add_opt_u8`

```c
static void add_opt_u8(uint8_t *buf, size_t *off, uint8_t opt, uint8_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在缓冲偏移处写入一个 1 字节长度的 DHCP 选项并推进偏移。

**位置**：`kernel/net/dhcp_server.c:74`

<a id="fn-kernel-net-dhcp_server.c-81"></a>

### `add_opt_ip`

```c
static void add_opt_ip(uint8_t *buf, size_t *off, uint8_t opt, const ip4_addr_t *ip)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写入一个长度 4 的 IP 地址 DHCP 选项并推进偏移。

**位置**：`kernel/net/dhcp_server.c:81`

<a id="fn-kernel-net-dhcp_server.c-89"></a>

### `add_opt_u32`

```c
static void add_opt_u32(uint8_t *buf, size_t *off, uint8_t opt, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写入一个长度 4 的 32 位大端 DHCP 选项并推进偏移。

**位置**：`kernel/net/dhcp_server.c:89`

<a id="fn-kernel-net-dhcp_server.c-97"></a>

### `dhcp_get_msg_type`

```c
static uint8_t dhcp_get_msg_type(const uint8_t *pkt, size_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从固定头之后遍历选项，返回单字节消息类型，越界、缺项或长度不符返回 0。

**位置**：`kernel/net/dhcp_server.c:97`

<a id="fn-kernel-net-dhcp_server.c-120"></a>

### `dhcp_send_reply`

```c
static void dhcp_send_reply(const uint8_t *req, size_t req_len, uint8_t msg_type)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

构造 BOOTREPLY 回显事务号与客户端地址、填租约和选项，经 UDP 广播给客户端并打日志。

<details>
<summary>函数内备注（5 条）</summary>

**第 129 行（行尾）**

> xid

**第 130 行（行尾）**

> flags

**第 131 行（行尾）**

> yiaddr

**第 132 行（行尾）**

> siaddr

**第 133 行（行尾）**

> chaddr

</details>

**位置**：`kernel/net/dhcp_server.c:120`

<a id="fn-kernel-net-dhcp_server.c-172"></a>

### `dhcp_recv`

```c
static void dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拷贝并校验请求头、硬件类型与魔数，DISCOVER 回 OFFER、REQUEST 回 ACK，其余丢弃。

**位置**：`kernel/net/dhcp_server.c:172`

<a id="fn-kernel-net-dhcp_server.c-210"></a>

### `dhcp_server_init`

```c
void dhcp_server_init(const ip4_addr_t *server_ip, const ip4_addr_t *netmask, const ip4_addr_t *lease_ip)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

已初始化则返回，否则保存服务器配置并创建绑定 DHCP 端口的 UDP PCB 注册回调。

**位置**：`kernel/net/dhcp_server.c:210`

<a id="mod-kernel-net-http_server.c"></a>

## kernel/net/http_server.c

`kernel/net/http_server.c` · 11 个函数 · 11 个有说明 · 284 行

<a id="fn-kernel-net-http_server.c-19"></a>

### `snfmt`

```c
static int snfmt(char *buf, int size, const char *fmt, ...)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 my_vsnprintf 按可变参数把格式化结果写入缓冲并返回长度。

**位置**：`kernel/net/http_server.c:19`

<a id="fn-kernel-net-http_server.c-98"></a>

### `build_response`

```c
static void build_response(http_conn_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以内嵌首页为响应体，生成 200 响应头与长度，并把连接状态置为发送头部。

**位置**：`kernel/net/http_server.c:98`

<a id="fn-kernel-net-http_server.c-116"></a>

### `conn_try_send`

```c
static void conn_try_send(struct tcp_pcb *tpcb, http_conn_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按发送缓冲分块写响应头与响应体，全部写完则关闭连接，无空间则等下次回调继续。

**位置**：`kernel/net/http_server.c:116`

<a id="fn-kernel-net-http_server.c-158"></a>

### `conn_close`

```c
static void conn_close(struct tcp_pcb *tpcb, http_conn_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先清空 TCP 的 arg、recv、sent、err 回调，再释放连接槽并关闭 PCB。

**位置**：`kernel/net/http_server.c:158`

<a id="fn-kernel-net-http_server.c-168"></a>

### `http_sent`

```c
static err_t http_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

发送缓冲出现空位时继续推进该连接的响应发送。

**位置**：`kernel/net/http_server.c:168`

<a id="fn-kernel-net-http_server.c-178"></a>

### `http_recv`

```c
static err_t http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

出错或空 pbuf 时关闭连接，否则确认接收、生成固定首页响应并开始发送。

**位置**：`kernel/net/http_server.c:178`

<a id="fn-kernel-net-http_server.c-203"></a>

### `http_err`

```c
static void http_err(void *arg, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

释放出错连接对应的连接槽。

**位置**：`kernel/net/http_server.c:203`

<a id="fn-kernel-net-http_server.c-215"></a>

### `conn_alloc`

```c
static http_conn_t *conn_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

寻找空闲连接槽，标记占用、清零并置为接收请求状态，无空闲则返回 NULL。

**位置**：`kernel/net/http_server.c:215`

<a id="fn-kernel-net-http_server.c-228"></a>

### `conn_free`

```c
static void conn_free(http_conn_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由连接指针计算槽位下标并清除其占用标记。

**位置**：`kernel/net/http_server.c:228`

<a id="fn-kernel-net-http_server.c-237"></a>

### `http_accept`

```c
static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配连接槽并为新 PCB 注册 recv、sent、err 回调，无槽时中止连接。

**位置**：`kernel/net/http_server.c:237`

<a id="fn-kernel-net-http_server.c-258"></a>

### `http_server_init`

```c
void http_server_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清空连接表，创建并绑定 HTTP 端口的 TCP PCB，转为监听并注册 accept 回调。

**位置**：`kernel/net/http_server.c:258`

<a id="mod-kernel-net-lwip_port-netif_avatar.c"></a>

## kernel/net/lwip_port/netif_avatar.c

`kernel/net/lwip_port/netif_avatar.c` · 3 个函数 · 3 个有说明 · 115 行

<a id="fn-kernel-net-lwip_port-netif_avatar.c-18"></a>

### `avatar_linkoutput`

```c
static err_t avatar_linkoutput(struct netif *netif, struct pbuf *p)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 pbuf 链拷贝进栈上帧缓冲，超长返回 ERR_BUF，否则交 netdev_send 发送并映射错误码。

**位置**：`kernel/net/lwip_port/netif_avatar.c:18`

<a id="fn-kernel-net-lwip_port-netif_avatar.c-38"></a>

### `avatar_netif_init`

```c
err_t avatar_netif_init(struct netif *netif)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从默认网卡取 MAC，设置 netif 名称、ARP 输出、链路输出、硬件地址、MTU 与标志。

**位置**：`kernel/net/lwip_port/netif_avatar.c:38`

<a id="fn-kernel-net-lwip_port-netif_avatar.c-57"></a>

### `avatar_lwip_poll_rx`

```c
void avatar_lwip_poll_rx(struct netif *netif)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

无 pending 时每 16 次兜底查一次，循环按最大帧长分配 pbuf 让驱动直收，收窄长度后交给 netif input。

<details>
<summary>函数内备注（4 条）</summary>

**第 62 行**

> ── 空轮询快速路径 ──────────────────────────────────────────────────

> 收包中断是可靠的&quot;有新帧了&quot;信号(驱动在 ISR 里 netdev_rx_wakeup())。
> 没有该信号时整段跳过, 连描述符都不碰 —— 描述符的 cache invalidate
> 每次带两次 fence, 而 net-poll 空闲时每秒空转约 10 万次, 那是 ~9% 的
> CPU, 在 CPU 份额紧张时这 9% 直接决定丢不丢包。

> 每 16 次兜底真查一遍：万一中断丢了(或驱动没有中断通知机制)也不会
> 永久停摆, 最坏是空载时多花 ~0.6% 的 CPU。

**第 78 行**

> 先按最大帧长分配 pbuf, 再让驱动把帧直接写进它的 payload ——
> 这样全路径只剩一次 1400 字节的拷贝(此前是&quot;驱动→栈上 frame→pbuf&quot;
> 两次, 实测共占每帧开销的 62%)。

> 按 PBUF_POOL_BUFSIZE(1536) 分配, pool 本来就不管请求长度, 总是给
> 一整块, 所以按最大帧长要并不会更贵。收到后 pbuf_realloc 收窄到实际
> 长度(单缓冲 pbuf 只是改 len/tot_len, 不搬数据)。

**第 90 行（行尾）**

> 池满, 下一轮再试

**第 102 行**

> 与 netdev.c 的 rx 日志同一级别 + 同一模块位：同一个包可能被两层
> 各记一次(recv_into 缺失时 netdev 会走自己的 recv)。

</details>

**位置**：`kernel/net/lwip_port/netif_avatar.c:57`

<a id="mod-kernel-net-lwip_port-sys_arch.c"></a>

## kernel/net/lwip_port/sys_arch.c

`kernel/net/lwip_port/sys_arch.c` · 1 个函数 · 1 个有说明 · 14 行

<a id="fn-kernel-net-lwip_port-sys_arch.c-11"></a>

### `sys_now`

```c
u32_t sys_now(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回基于系统运行时间的毫秒计数，供 lwIP 使用。

**位置**：`kernel/net/lwip_port/sys_arch.c:11`

<a id="mod-kernel-net-net.c"></a>

## kernel/net/net.c

`kernel/net/net.c` · 3 个函数 · 3 个有说明 · 148 行

<a id="fn-kernel-net-net.c-28"></a>

### `net_init`

```c
void net_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化 lwIP、添加并启用 av 网卡、设置静态地址，再启动 DHCP、echo、带宽测试与 HTTP 服务。

<details>
<summary>函数内备注（1 条）</summary>

**第 52 行**

> 收包中断不唤醒任何任务(原因见下面&quot;收包唤醒链路(暂缓)&quot;)：
> 驱动在 ISR 里只调 netdev_rx_wakeup() 置一个&quot;有新帧&quot;的标志, net-poll
> 用 netdev_rx_pending() 读它来挡掉空轮询。net-poll 本身走协作式让出,
> 始终在就绪队列里, 不存在&quot;需要被唤醒&quot;这回事。

> 等异常返回路径能支持&quot;任务上下文里阻塞, 由中断唤醒&quot;之后, 再在这里挂
> 真正唤醒 net-poll 的钩子。

</details>

**位置**：`kernel/net/net.c:28`

<a id="fn-kernel-net-net.c-73"></a>

### `net_poll_once`

```c
void net_poll_once(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

网络就绪时轮询网卡收包、lwIP 超时处理与带宽统计。

**位置**：`kernel/net/net.c:73`

<a id="fn-kernel-net-net.c-116"></a>

### `net_poll_task`

```c
void net_poll_task(void *arg)
```

目标形态是：net-poll 空闲时 task_block() 阻塞, 由收包中断唤醒。这样它在
没有流量时不烧 CPU, 也就不会在 g++ 编译这类 CPU 饱和场景下跟别的任务抢
时间片, 进而让 RX 环溢出丢包。

这条路目前走不通, 已退回协作式让出(见 net_poll_task)。
原因在内核的异常返回路径, 不在网络层：

boot/riscv64/exception.S 在 sched_check_and_yield_from_trap() 返回后,
仍然假定 sp 指向进入 trap 时构造的那个帧：

call sched_check_and_yield_from_trap   ← 这里可能已经切换了任务
ld   t0, 32*8(sp) / csrw sepc, t0
ld   t0, 35*8(sp) / csrw sstatus, t0   ← 仍按旧假设从栈上读

被中断抢占的任务满足这个假设(它的 sp 本就是自己 trap frame 的基址),
所以一直没暴露。但在任务上下文里通过 task_block() 睡下的任务不满足 ——
它的 sp 指向内核栈中间的 sched_schedule 调用帧, 那里没有 trap frame。
被唤醒切过去时会从栈上读到垃圾并写进 sstatus, 结果中断从此不再投递。

实测(SG2002, LOG=none)：改成阻塞之后, tick 回调打在 10/20/30/40ms 后
再无输出(定时器停摆), 串口失去响应, ping 不通；而 net-poll 的循环计数
停在它阻塞的那一刻 —— 任务被解阻塞了, 但再没被调度起来。把阻塞改回
task_yield() 后全部恢复：收包 95 Mbps, 零丢包。

要支持这个模式, 得先让异常返回路径在调度切换之后重新取得当前任务的帧指针
(例如把 frame 指针存进 per-CPU 变量, 由调度器在切换时更新)。
在那之前, netdev 层的 netdev_rx_wakeup()(驱动 ISR 调用)只置标志, 不唤醒
任何任务：net-poll 靠 netdev_rx_pending() 读这个标志来省掉空轮询。

<details>
<summary>函数内备注（1 条）</summary>

**第 130 行**

> 回到协作式让出。

> 注意： 不能改成 task_block() 阻塞 + 由中断唤醒：本内核的异常返回路径
> (boot/riscv64/exception.S)在 sched_check_and_yield_from_trap()
> 返回后仍然假定 sp 指向本次 trap 的帧：
> call sched_check_and_yield_from_trap   ← 这里可能已切换任务
> ld t0, 35*8(sp) / csrw sstatus, t0     ← 按旧假设从栈上读
> 被抢占的任务满足这个假设(它的 sp 本就是自己的 trap frame),
> 但在任务上下文里通过 task_block() 睡下的任务, sp 指向内核栈中间的
> sched_schedule 调用帧 —— 被唤醒切过去时会从那里读到垃圾并写进
> sstatus, 结果中断从此不再投递(实测：定时器几毫秒内停摆)。

> 要支持&quot;中断唤醒阻塞任务&quot;, 得先让异常返回路径在切换后重新取得正确
> 的帧指针。在那之前, net-poll 只能继续协程式让出。

</details>

**位置**：`kernel/net/net.c:116`

<a id="mod-kernel-net-netdev.c"></a>

## kernel/net/netdev.c — 收包通知状态。只在启动阶段写一次(irq_backed), 运行期读；

`kernel/net/netdev.c` · 9 个函数 · 9 个有说明 · 118 行

pending 由 ISR 置位, 轮询方读取并清除。

<a id="fn-kernel-net-netdev.c-21"></a>

### `netdev_rx_set_irq_backed`

```c
void netdev_rx_set_irq_backed(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

标记当前网卡收包有中断通知，使轮询可依赖 pending 标志跳过空转。

**位置**：`kernel/net/netdev.c:21`

<a id="fn-kernel-net-netdev.c-27"></a>

### `netdev_rx_wakeup`

```c
void netdev_rx_wakeup(void)
```

中断上下文：只置一个标志

**位置**：`kernel/net/netdev.c:27`

<a id="fn-kernel-net-netdev.c-32"></a>

### `netdev_rx_pending`

```c
bool netdev_rx_pending(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

无中断通知时恒返回 true；有通知时读取并清除 pending 标志后返回是否有新帧。

<details>
<summary>函数内备注（1 条）</summary>

**第 34 行**

> 没有中断通知机制的驱动：老实报告&quot;可能有帧&quot;, 轮询方每次都查

</details>

**位置**：`kernel/net/netdev.c:32`

<a id="fn-kernel-net-netdev.c-45"></a>

### `netdev_register`

```c
int netdev_register(netdev_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验设备包含 send 与 recv 后设为默认网卡并打印名称与 MAC，非法设备返回 -1。

**位置**：`kernel/net/netdev.c:45`

<a id="fn-kernel-net-netdev.c-58"></a>

### `netdev_default`

```c
netdev_t *netdev_default(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回当前注册的默认网卡指针。

**位置**：`kernel/net/netdev.c:58`

<a id="fn-kernel-net-netdev.c-63"></a>

### `netdev_send`

```c
int netdev_send(const uint8_t *frame, size_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

参数合法时把帧交给默认网卡的 send 回调发送，否则返回 -1。

**位置**：`kernel/net/netdev.c:63`

<a id="fn-kernel-net-netdev.c-71"></a>

### `netdev_recv`

```c
int netdev_recv(uint8_t *frame, size_t maxlen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用默认网卡 recv 接收一帧，成功时累计计数并采样打印帧长度与以太类型。

<details>
<summary>函数内备注（1 条）</summary>

**第 81 行**

> 逐包一条：只能 DEBUG + 模块位 + 采样。注意 netdev_recv_into 在驱动
> 没有 recv_into 时会回退到这里, 而 netif_avatar 自己也会打一条 ——
> 所以两处的级别和模块位必须一致, 否则同一个包会被两层重复输出。

</details>

**位置**：`kernel/net/netdev.c:71`

<a id="fn-kernel-net-netdev.c-96"></a>

### `netdev_recv_into`

```c
int netdev_recv_into(uint8_t *dst, size_t maxlen)
```

同 netdev_recv, 但让驱动直接写进 dst。
驱动没有实现 recv_into 时回退到 recv(此时 driver 会经由内部缓冲再拷一次)。

**位置**：`kernel/net/netdev.c:96`

<a id="fn-kernel-net-netdev.c-109"></a>

### `netdev_mac`

```c
void netdev_mac(uint8_t mac[6])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

无默认网卡时把输出 MAC 清零，否则复制其 6 字节 MAC。

**位置**：`kernel/net/netdev.c:109`

<a id="mod-kernel-net-tcp_echo.c"></a>

## kernel/net/tcp_echo.c

`kernel/net/tcp_echo.c` · 4 个函数 · 4 个有说明 · 88 行

<a id="fn-kernel-net-tcp_echo.c-13"></a>

### `echo_recv`

```c
static err_t echo_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对端关闭时关闭连接；否则确认接收数据并逐 pbuf 回写，最后 output 并释放。

**位置**：`kernel/net/tcp_echo.c:13`

<a id="fn-kernel-net-tcp_echo.c-44"></a>

### `echo_err`

```c
static void echo_err(void *arg, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印连接被中止的警告日志。

**位置**：`kernel/net/tcp_echo.c:44`

<a id="fn-kernel-net-tcp_echo.c-50"></a>

### `echo_accept`

```c
static err_t echo_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

为新连接注册接收与错误回调并打印已接受连接的日志。

**位置**：`kernel/net/tcp_echo.c:50`

<a id="fn-kernel-net-tcp_echo.c-64"></a>

### `tcp_echo_init`

```c
void tcp_echo_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

创建绑定 1234 端口的 TCP PCB，转为监听并注册 accept 回调，失败时关闭并报错。

**位置**：`kernel/net/tcp_echo.c:64`

<a id="mod-kernel-net-webcam_httpd.c"></a>

## kernel/net/webcam_httpd.c

`kernel/net/webcam_httpd.c` · 13 个函数 · 13 个有说明 · 360 行

<a id="fn-kernel-net-webcam_httpd.c-22"></a>

### `snfmt`

```c
static int snfmt(char *buf, int size, const char *fmt, ...)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 my_vsnprintf 按可变参数把格式化结果写入缓冲并返回长度。

**位置**：`kernel/net/webcam_httpd.c:22`

<a id="fn-kernel-net-webcam_httpd.c-38"></a>

### `webcam_grab_frame`

```c
static int webcam_grab_frame(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

抓帧间隔未到或失败次数达上限时返回缓存帧长度，否则记录时间、递增失败计数并返回失败。

**位置**：`kernel/net/webcam_httpd.c:38`

<a id="fn-kernel-net-webcam_httpd.c-121"></a>

### `match_path`

```c
static int match_path(const char *req, uint16_t len, const char *path, size_t plen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验请求以 GET 空格开头且路径前缀匹配，并跟随空格、问号或回车，返回是否命中。

**位置**：`kernel/net/webcam_httpd.c:121`

<a id="fn-kernel-net-webcam_httpd.c-134"></a>

### `build_response`

```c
static int build_response(webcam_conn_t *c, const char *req, uint16_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对 /frame.jpg 抓帧成功回 JPEG、失败回 503，其余路径回内嵌首页，均置为发送头部。

**位置**：`kernel/net/webcam_httpd.c:134`

<a id="fn-kernel-net-webcam_httpd.c-185"></a>

### `conn_try_send`

```c
static void conn_try_send(struct tcp_pcb *tpcb, webcam_conn_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按发送缓冲分块写响应头与响应体，写完或响应体为空时关闭连接，无空间则等待下次回调。

**位置**：`kernel/net/webcam_httpd.c:185`

<a id="fn-kernel-net-webcam_httpd.c-235"></a>

### `conn_close`

```c
static void conn_close(struct tcp_pcb *tpcb, webcam_conn_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先清空 TCP 的 arg、recv、sent、err 回调，再释放连接槽并关闭 PCB。

**位置**：`kernel/net/webcam_httpd.c:235`

<a id="fn-kernel-net-webcam_httpd.c-245"></a>

### `webcam_sent`

```c
static err_t webcam_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

发送缓冲出现空位时继续推进该连接的响应发送。

**位置**：`kernel/net/webcam_httpd.c:245`

<a id="fn-kernel-net-webcam_httpd.c-255"></a>

### `webcam_recv`

```c
static err_t webcam_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

出错或空 pbuf 时关闭连接，否则确认接收、按请求构建响应并开始发送。

**位置**：`kernel/net/webcam_httpd.c:255`

<a id="fn-kernel-net-webcam_httpd.c-280"></a>

### `webcam_err`

```c
static void webcam_err(void *arg, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

释放出错连接对应的连接槽。

**位置**：`kernel/net/webcam_httpd.c:280`

<a id="fn-kernel-net-webcam_httpd.c-291"></a>

### `conn_alloc`

```c
static webcam_conn_t *conn_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

寻找空闲连接槽，标记占用、清零并置为接收请求状态，无空闲则返回 NULL。

**位置**：`kernel/net/webcam_httpd.c:291`

<a id="fn-kernel-net-webcam_httpd.c-304"></a>

### `conn_free`

```c
static void conn_free(webcam_conn_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由连接指针计算槽位下标并清除其占用标记。

**位置**：`kernel/net/webcam_httpd.c:304`

<a id="fn-kernel-net-webcam_httpd.c-313"></a>

### `webcam_accept`

```c
static err_t webcam_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配连接槽并为新 PCB 注册 recv、sent、err 回调，无槽时中止连接。

**位置**：`kernel/net/webcam_httpd.c:313`

<a id="fn-kernel-net-webcam_httpd.c-334"></a>

### `webcam_httpd_init`

```c
void webcam_httpd_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清空连接表，创建并绑定 webcam 端口的 TCP PCB，转为监听并注册 accept 回调。

**位置**：`kernel/net/webcam_httpd.c:334`

<a id="mod-kernel-rt-rt_event.c"></a>

## kernel/rt/rt_event.c — RT 事件位等待/置位

`kernel/rt/rt_event.c` · 6 个函数 · 0 个有说明 · 166 行

等待队列与 bits 在同一把自旋锁下修改：检查条件、入队、置 BLOCKED
与 set 方的置位、出队、唤醒不会互相落在对方的缝隙里。

等待者对象放在 rt_event_wait() 的栈上（内核栈），不动态分配。
事件 set 方在锁内只做 task_unblock()，所以 ISR 可以直接调用。

<a id="fn-kernel-rt-rt_event.c-31"></a>

### `rt_event_satisfied`

```c
static bool rt_event_satisfied(uint32_t bits, uint32_t mask, bool wait_all)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_event.c:31`

<a id="fn-kernel-rt-rt_event.c-36"></a>

### `rt_event_init`

```c
void rt_event_init(rt_event_t *e)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_event.c:36`

<a id="fn-kernel-rt-rt_event.c-43"></a>

### `rt_event_get`

```c
uint32_t rt_event_get(rt_event_t *e)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_event.c:43`

<a id="fn-kernel-rt-rt_event.c-57"></a>

### `rt_event_set`

```c
void rt_event_set(rt_event_t *e, uint32_t bits)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 74 行**

> 锁序：event-&gt;lock → 目标核 rq_lock。和 completion/mutex 一致。
> 等待者可能还在 CPU 上（已置 BLOCKED、尚未切出），
> sched_unblock 只置 wake_pending，不会重复入队。

</details>

**位置**：`kernel/rt/rt_event.c:57`

<a id="fn-kernel-rt-rt_event.c-87"></a>

### `rt_event_clear`

```c
void rt_event_clear(rt_event_t *e, uint32_t bits)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_event.c:87`

<a id="fn-kernel-rt-rt_event.c-98"></a>

### `rt_event_wait`

```c
uint32_t rt_event_wait(rt_event_t *e, uint32_t mask, bool wait_all, uint64_t timeout_ns, bool clear)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（3 条）</summary>

**第 137 行（行尾）**

> 0 = 无限等待

**第 145 行**

> set 方判定满足后会把 matched 写好再唤醒。不能只看当前 bits：
> 另一个 clear=true 的等待者可能已经先清掉了位，否则这次唤醒会被
> 误判成“没满足”而重新睡下去。

**第 159 行**

> set 方会先把节点摘掉再唤醒；超时/信号唤醒则节点还在，需要自己删。
> 之后统一回到循环开头重新检查 bits 和 deadline。

</details>

**位置**：`kernel/rt/rt_event.c:98`

<a id="mod-kernel-rt-rt_latency.c"></a>

## kernel/rt/rt_latency.c — R0 实时延迟统计

`kernel/rt/rt_latency.c` · 18 个函数 · 12 个有说明 · 373 行

见 docs/realtime/ARCHITECTURE.md。这里只做“常开、低成本”的近似测量，
真正的 irq-off trace 后续再上（R0.5）：
- wake→run：sched_unblock() 记录 task->wake_ns，调度器切入时结算；
- syscall：用户 syscall 返回路径结算内核执行时长；
- tick_gap：timer tick 间隔，作为长关中断/长临界区的近似指示。
数据经 /proc/rtstat 暴露。

<a id="fn-kernel-rt-rt_latency.c-65"></a>

### `rt_lat_cpu`

```c
static inline rt_lat_cpu_t *rt_lat_cpu(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回当前 CPU 的延迟统计结构，CPU 号越界时回退到 0 号。

**位置**：`kernel/rt/rt_latency.c:65`

<a id="fn-kernel-rt-rt_latency.c-73"></a>

### `rt_latency_wake`

```c
void rt_latency_wake(task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把任务的唤醒时刻记为当前定时器纳秒值。

**位置**：`kernel/rt/rt_latency.c:73`

<a id="fn-kernel-rt-rt_latency.c-80"></a>

### `rt_latency_run`

```c
void rt_latency_run(task_t *t, uint64_t now_ns)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若有待统计唤醒时刻，算出唤醒延迟并更新本 CPU 的 last/max/计数后清零该时刻。

**位置**：`kernel/rt/rt_latency.c:80`

<a id="fn-kernel-rt-rt_latency.c-99"></a>

### `rt_latency_syscall`

```c
void rt_latency_syscall(uint64_t duration_ns, uint32_t nr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

记录一次系统调用耗时，更新本 CPU 的 last、max 值（记下最慢调用号）与计数。

**位置**：`kernel/rt/rt_latency.c:99`

<a id="fn-kernel-rt-rt_latency.c-111"></a>

### `rt_latency_reset`

```c
void rt_latency_reset(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清零全部 CPU 的延迟统计，并置关中断追踪就绪标志。

**位置**：`kernel/rt/rt_latency.c:111`

<a id="fn-kernel-rt-rt_latency.c-117"></a>

### `rt_latency_irq_off_begin`

```c
void rt_latency_irq_off_begin(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

就绪且非嵌套时，记录本 CPU 关中断起始时刻与调用者返回地址。

<details>
<summary>函数内备注（1 条）</summary>

**第 127 行（行尾）**

> 嵌套关中断：只统计最外层区间

</details>

**位置**：`kernel/rt/rt_latency.c:117`

<a id="fn-kernel-rt-rt_latency.c-134"></a>

### `rt_latency_irq_off_end`

```c
void rt_latency_irq_off_end(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

结束本 CPU 关中断区间，累加计数并更新持续时长、最大值及最长区间的调用点。

<details>
<summary>函数内备注（1 条）</summary>

**第 157 行**

> 最长区间起点对应的调用点（arch_irq_save/disable 的调用者）。

</details>

**位置**：`kernel/rt/rt_latency.c:134`

<a id="fn-kernel-rt-rt_latency.c-162"></a>

### `rt_latency_preempt_off_begin`

```c
void rt_latency_preempt_off_begin(uint64_t callsite_pc)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 172 行（行尾）**

> 嵌套 preempt_disable：只统计最外层

</details>

**位置**：`kernel/rt/rt_latency.c:162`

<a id="fn-kernel-rt-rt_latency.c-179"></a>

### `rt_latency_preempt_off_end`

```c
void rt_latency_preempt_off_end(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 202 行**

> 最长区间起点对应的调用点（preempt_disable 的调用者）。

</details>

**位置**：`kernel/rt/rt_latency.c:179`

<a id="fn-kernel-rt-rt_latency.c-207"></a>

### `rt_latency_preempt_off_max_ns`

```c
uint64_t rt_latency_preempt_off_max_ns(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_latency.c:207`

<a id="fn-kernel-rt-rt_latency.c-212"></a>

### `rt_latency_preempt_off_count`

```c
uint64_t rt_latency_preempt_off_count(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_latency.c:212`

<a id="fn-kernel-rt-rt_latency.c-217"></a>

### `rt_latency_switch`

```c
void rt_latency_switch(uint32_t cpu_id, bool migrated)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_latency.c:217`

<a id="fn-kernel-rt-rt_latency.c-228"></a>

### `rt_latency_migration_count`

```c
uint64_t rt_latency_migration_count(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_latency.c:228`

<a id="fn-kernel-rt-rt_latency.c-238"></a>

### `rt_latency_tick`

```c
void rt_latency_tick(uint32_t cpu_id, uint64_t now_ns)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

记录该 CPU 本次 tick 时刻，与上次比较更新间隔 last/max 并累加计数。

**位置**：`kernel/rt/rt_latency.c:238`

<a id="fn-kernel-rt-rt_latency.c-256"></a>

### `lat_puts`

```c
static size_t lat_puts(char *buf, size_t cap, size_t pos, const char *s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把字符串追加进缓冲区并保留结尾 NUL，返回新的写入位置。

**位置**：`kernel/rt/rt_latency.c:256`

<a id="fn-kernel-rt-rt_latency.c-265"></a>

### `lat_put_u64`

```c
static size_t lat_put_u64(char *buf, size_t cap, size_t pos, uint64_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把无符号数按十进制转为文本追加，补 NUL 并返回新位置。

**位置**：`kernel/rt/rt_latency.c:265`

<a id="fn-kernel-rt-rt_latency.c-284"></a>

### `lat_put_hex`

```c
static size_t lat_put_hex(char *buf, size_t cap, size_t pos, uint64_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把无符号数按小写十六进制追加，补 NUL 并返回新位置。

**位置**：`kernel/rt/rt_latency.c:284`

<a id="fn-kernel-rt-rt_latency.c-304"></a>

### `rt_latency_snprintf`

```c
int rt_latency_snprintf(char *buf, size_t cap)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按固定列把各 CPU 的唤醒、系统调用、tick 间隔与关中断延迟统计渲染成多行文本，返回长度。

**位置**：`kernel/rt/rt_latency.c:304`

<a id="mod-kernel-rt-rt_mutex.c"></a>

## kernel/rt/rt_mutex.c — 直接优先级继承的睡眠锁

`kernel/rt/rt_mutex.c` · 8 个函数 · 2 个有说明 · 222 行

等待队列按“有效 RT 优先级”降序排列（同优先级 FIFO）。等待者入队时
把 owner 提升到不低于自己的优先级；释放时把锁交给队首（最高优先级）
等待者，并把它的继承优先级设为剩余等待者的最高值。

调度侧的提升由 sched_set_pi_boost() 完成：它会按需把 READY 任务在
就绪队列里重新定位，并让被提升的任务在 tick 上按 FIFO 对待。

锁序：rt_mutex->lock → 目标核 rq_lock（sched_set_pi_boost / task_unblock
内部）。调度器不会反向获取 rt_mutex->lock，因此不会形成环。

已知限制（V1）：
- 只做直接继承。owner 自己阻塞在另一把 rt_mutex 上时，不会沿等待
链继续提升（没有嵌套 PI 链）。
- 不跟踪任务持有的全部 rt_mutex；多把锁同时有等待者时，释放其中
一把会把继承优先级清零，可能丢失另一把锁带来的提升。
- 不检测 A→B→A 死锁。
需要完整 PI 链时再按 Linux rt_mutex 的 waiter/owner 链重做。

<a id="fn-kernel-rt-rt_mutex.c-35"></a>

### `rtm_prio_of`

```c
static uint8_t rtm_prio_of(const task_t *t)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 37 行**

> NORMAL 等待者不产生提升，按 0 排在队尾。

</details>

**位置**：`kernel/rt/rt_mutex.c:35`

<a id="fn-kernel-rt-rt_mutex.c-42"></a>

### `rtm_insert_waiter`

```c
static void rtm_insert_waiter(rt_mutex_t *m, task_t *t)
```

持 m->lock 调用。

**位置**：`kernel/rt/rt_mutex.c:42`

<a id="fn-kernel-rt-rt_mutex.c-57"></a>

### `rtm_boost_owner`

```c
static void rtm_boost_owner(rt_mutex_t *m, uint8_t prio)
```

持 m->lock 调用：owner 的提升只增不减，真正的恢复在 unlock。

**位置**：`kernel/rt/rt_mutex.c:57`

<a id="fn-kernel-rt-rt_mutex.c-67"></a>

### `rt_mutex_init`

```c
void rt_mutex_init(rt_mutex_t *m)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_mutex.c:67`

<a id="fn-kernel-rt-rt_mutex.c-74"></a>

### `rt_mutex_lock`

```c
int rt_mutex_lock(rt_mutex_t *m)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（4 条）</summary>

**第 85 行**

> 非递归：入口处自己已经持有就报错，不能走进等待队列等自己释放。

**第 93 行**

> 可能被唤醒后抢到（上一个 holder 已释放，但没把锁交给我们）。

**第 101 行**

> 走到这里是阻塞等待后的返回：unlock 会把 owner 直接交给我们。

**第 113 行**

> 入队与 BLOCKED 都在 m-&gt;lock 下发布；解锁方不会漏掉这次唤醒。
> 若在切出前被唤醒，sched_unblock 只置 wake_pending，由
> sched_schedule() 补入队列。

</details>

**位置**：`kernel/rt/rt_mutex.c:74`

<a id="fn-kernel-rt-rt_mutex.c-128"></a>

### `rt_mutex_trylock`

```c
bool rt_mutex_trylock(rt_mutex_t *m)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_mutex.c:128`

<a id="fn-kernel-rt-rt_mutex.c-146"></a>

### `rt_mutex_unlock`

```c
bool rt_mutex_unlock(rt_mutex_t *m)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（4 条）</summary>

**第 157 行**

> 非 owner 解锁是 API 误用，但返回值 false 已经是契约的一部分；
> 不要用 KLOG_ERROR —— RT_TEST 的负向用例会故意走这条路径，
> 用户态误用也会把启动日志刷成“内核出错了”。DEBUG 留作诊断。

**第 175 行**

> 新 owner 还要接住剩余等待者的提升；它自己已有的继承优先级
> （来自它持有的另一把 rt_mutex）只增不减。

**第 191 行**

> 先撤销本任务的继承，再唤醒新 owner：sched_kick_after_enqueue 需要
> 用 cur 已恢复后的优先级做比较，否则“80 的等待者 &lt;= 80 的旧 owner”
> 会漏掉这次立即抢占。
> 多锁场景的已知限制见文件头。

**第 199 行**

> 锁序：rt_mutex-&gt;lock → rq_lock。task_unblock 只在目标核上入队/
> 置 wake_pending，不会睡眠。

</details>

**位置**：`kernel/rt/rt_mutex.c:146`

<a id="fn-kernel-rt-rt_mutex.c-210"></a>

### `rt_mutex_owner`

```c
task_t *rt_mutex_owner(rt_mutex_t *m)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/rt/rt_mutex.c:210`

<a id="mod-kernel-rt-rt_worker.c"></a>

## kernel/rt/rt_worker.c — 用户可配置的 RT worker（模板执行器）

`kernel/rt/rt_worker.c` · 37 个函数 · 37 个有说明 · 1067 行

数据面：每个 worker 是一个 KERNEL + SCHED_FIFO 的内核任务，按
period_ns 周期调用模板的 step()。step 必须：
- 有界；无分配；无睡眠；无 VFS；不长时间持锁。
控制面：/dev/rtctl 的 ioctl（fs/pseudofs/rtctl_dev.c）。

V1 的周期由 100Hz tick 驱动（task->sleep_expire_ns），所以
period_ns 必须是 10ms 的整数倍。要支持 1kHz，需要先有 oneshot hrtimer。

<a id="fn-kernel-rt-rt_worker.c-126"></a>

### `rtw_clamp_i64`

```c
static int64_t rtw_clamp_i64(int64_t v, int64_t lo, int64_t hi)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

lo>=hi 时视为未配置限幅直接返回原值，否则把 v 夹到 [lo,hi] 区间。

<details>
<summary>函数内备注（1 条）</summary>

**第 129 行（行尾）**

> 没配置限幅

</details>

**位置**：`kernel/rt/rt_worker.c:126`

<a id="fn-kernel-rt-rt_worker.c-137"></a>

### `rtw_sat_add`

```c
static int64_t rtw_sat_add(int64_t a, int64_t b)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

饱和加法：上溢返回 RTW_I64_MAX，下溢返回 RTW_I64_MIN，否则返回两数之和。

**位置**：`kernel/rt/rt_worker.c:137`

<a id="fn-kernel-rt-rt_worker.c-146"></a>

### `rtw_input`

```c
static int64_t rtw_input(int64_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把输入按正负 RTW_INPUT_LIMIT 做区间限幅后返回。

**位置**：`kernel/rt/rt_worker.c:146`

<a id="fn-kernel-rt-rt_worker.c-151"></a>

### `rtw_gain`

```c
static int64_t rtw_gain(uint64_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把参数截断为 int32 再扩展为 int64，使增益乘输入后不溢出。

<details>
<summary>函数内备注（1 条）</summary>

**第 153 行**

> 增益限成 int32，乘上 ±2^40 的输入后不会溢出 int64。

</details>

**位置**：`kernel/rt/rt_worker.c:151`

<a id="fn-kernel-rt-rt_worker.c-159"></a>

### `periodic_io_init`

```c
static void periodic_io_init(struct rt_worker *w)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，不修改任何状态。

**位置**：`kernel/rt/rt_worker.c:159`

<a id="fn-kernel-rt-rt_worker.c-164"></a>

### `periodic_io_step`

```c
static uint64_t periodic_io_step(struct rt_worker *w, const uint64_t *params, uint64_t in)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

限幅输入乘增益后右移 16 位，加偏置做饱和加法，再按参数区间限幅输出。

**位置**：`kernel/rt/rt_worker.c:164`

<a id="fn-kernel-rt-rt_worker.c-177"></a>

### `event_init`

```c
static void event_init(struct rt_worker *w)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，不修改任何状态。

**位置**：`kernel/rt/rt_worker.c:177`

<a id="fn-kernel-rt-rt_worker.c-186"></a>

### `event_step`

```c
static uint64_t event_step(struct rt_worker *w, const uint64_t *params, uint64_t in)
```

每个事件做一次和 periodic_io 相同的定点变换：out = clamp(in*gain + offset)。
输入来自事件环，不是共享页 input。

**位置**：`kernel/rt/rt_worker.c:186`

<a id="fn-kernel-rt-rt_worker.c-193"></a>

### `pid_init`

```c
static void pid_init(struct rt_worker *w)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清零积分项与上次误差，并把 initialized 置为 false。

**位置**：`kernel/rt/rt_worker.c:193`

<a id="fn-kernel-rt-rt_worker.c-200"></a>

### `pid_step`

```c
static uint64_t pid_step(struct rt_worker *w, const uint64_t *params, uint64_t in)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

做限幅 PID 运算：积分项按 ilim 饱和、记录上次误差，输出加权后右移 16 位并限幅。

**位置**：`kernel/rt/rt_worker.c:200`

<a id="fn-kernel-rt-rt_worker.c-232"></a>

### `watchdog_init`

```c
static void watchdog_init(struct rt_worker *w)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清零 last_seq 与 last_change_ns，并把 initialized 置为 false。

**位置**：`kernel/rt/rt_worker.c:232`

<a id="fn-kernel-rt-rt_worker.c-243"></a>

### `watchdog_trip`

```c
static uint64_t watchdog_trip(struct rt_worker *w, uint64_t safe)
```

记录一次 watchdog 切到安全输出的动作。

**位置**：`kernel/rt/rt_worker.c:243`

<a id="fn-kernel-rt-rt_worker.c-255"></a>

### `watchdog_step`

```c
static uint64_t watchdog_step(struct rt_worker *w, const uint64_t *params, uint64_t in)
```

参数：[0]=target worker id [1]=timeout_ns [2]=safe_output
目标 worker 的 out_seq 在 timeout 内没有变化 → 输出 safe_output；
健康时输出 0。目标不存在/参数非法时直接输出 safe_output。

**位置**：`kernel/rt/rt_worker.c:255`

<a id="fn-kernel-rt-rt_worker.c-299"></a>

### `rtw_template`

```c
static const struct rt_template_ops *rtw_template(uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在模板表 g_templates 中按 id 查找，命中返回表项指针，否则返回 NULL。

**位置**：`kernel/rt/rt_worker.c:299`

<a id="fn-kernel-rt-rt_worker.c-310"></a>

### `rtw_sleep_until`

```c
static bool rtw_sleep_until(struct rt_worker *w, uint64_t deadline)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环设置睡眠到期时间并阻塞当前任务直到 stop 或 deadline，返回是否未被停止。

**位置**：`kernel/rt/rt_worker.c:310`

<a id="fn-kernel-rt-rt_worker.c-324"></a>

### `rtw_event_ring_init`

```c
static void rtw_event_ring_init(struct rtw_event_ring *ev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把事件环头尾清零、设置容量，并清零投递、丢弃、处理计数与 last_value。

**位置**：`kernel/rt/rt_worker.c:324`

<a id="fn-kernel-rt-rt_worker.c-335"></a>

### `rtw_event_ring_empty`

```c
static bool rtw_event_ring_empty(struct rtw_event_ring *ev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 acquire 读 head、relaxed 读 tail，两者相等即判为空。

**位置**：`kernel/rt/rt_worker.c:335`

<a id="fn-kernel-rt-rt_worker.c-347"></a>

### `rtw_pop_event`

```c
static bool rtw_pop_event(struct rt_worker *w, uint64_t *value)
```

消费者只由 RT worker 调用。生产者在发布 head 之前已经把 slot 写好，
这里 acquire 读取 head 后读 slot 是安全的；tail 用 release 发布，
生产者据此判断空/满。

**位置**：`kernel/rt/rt_worker.c:347`

<a id="fn-kernel-rt-rt_worker.c-372"></a>

### `rtw_event_rearm`

```c
static void rtw_event_rearm(struct rt_worker *w)
```

排空后重新武装 DATA 位。顺序必须是“先清位、再查环”：
生产者若在清位之后投递会重新置位；反过来先查再清会把刚到的事件
置位清掉，等待睡下去后环里有事件却永远不醒。

**位置**：`kernel/rt/rt_worker.c:372`

<a id="fn-kernel-rt-rt_worker.c-382"></a>

### `rtw_params_snapshot`

```c
static void rtw_params_snapshot(struct rt_worker *w, uint64_t *out)
```

周期开头取一份一致的参数快照；更新方用 seqlock（偶数稳定/奇数写入）。

<details>
<summary>函数内备注（1 条）</summary>

**第 385 行**

> 共享页对 supervisor 是可写的，params_seq 也可能被写坏（比如永远
> 停在奇数）。RT 线程不能为此无限自旋，否则会以 RT 优先级占满一
> 整颗核；重试有限次后退回内核私有的 w-&gt;params —— 那份只有内核
> 写，seqlock 一定能收敛。

</details>

**位置**：`kernel/rt/rt_worker.c:382`

<a id="fn-kernel-rt-rt_worker.c-417"></a>

### `rt_worker_entry`

```c
static void rt_worker_entry(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

worker 主循环：事件或周期唤醒后取输入、快照参数并调用模板 step，发布输出与统计到共享页和设备，退出时置安全态并结束任务。

<details>
<summary>函数内备注（4 条）</summary>

**第 437 行**

> period_ns == 0：无限等待，只有事件/STOP 能唤醒；
> period_ns &gt; 0：最多等这么久，超时后回到循环重新检查 stop。

**第 446 行**

> 超时/唤醒竞争：环里暂时没有数据，重新武装 DATA

**第 454 行（行尾）**

> stop

**第 529 行**

> oneshot 下唤醒精度是 µs 级，超过半个周期才算 miss；
> 退化为 tick 唤醒时，相位可能让一次唤醒晚最多一个 tick。

</details>

**位置**：`kernel/rt/rt_worker.c:417`

<a id="fn-kernel-rt-rt_worker.c-552"></a>

### `rtw_get_locked`

```c
static struct rt_worker *rtw_get_locked(uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 id 范围后返回对应 worker 槽位，槽位处于 FREE 状态则返回 NULL。

**位置**：`kernel/rt/rt_worker.c:552`

<a id="fn-kernel-rt-rt_worker.c-560"></a>

### `rtw_copy_name`

```c
static void rtw_copy_name(char *dst, const char *src, size_t dstsz)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐字符复制源串，最多写 dstsz-1 字节并补结尾 NUL。

**位置**：`kernel/rt/rt_worker.c:560`

<a id="fn-kernel-rt-rt_worker.c-570"></a>

### `rt_worker_init`

```c
void rt_worker_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

原子检查初始化标志，未初始化时注册内置 rtdev 后再置位，重复调用直接返回。

**位置**：`kernel/rt/rt_worker.c:570`

<a id="fn-kernel-rt-rt_worker.c-578"></a>

### `rt_worker_create`

```c
int rt_worker_create(const struct rtw_create_req *cfg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验参数后加锁分配空闲槽位，初始化共享页、事件环与模板并绑定设备，失败回滚并返回错误码。

<details>
<summary>函数内备注（4 条）</summary>

**第 594 行**

> event 模板的 period_ns 是“等待事件的最长时间”：
> 0 = 无限等待；否则至少 1ms。

**第 604 行**

> 目标核没有 oneshot 时只能按 100Hz tick 唤醒（period=0 不需要超时）。

**第 639 行**

> 每槽位一页共享内存：首次分配，之后复用。

**第 643 行**

> 槽位刚标成 CREATED，失败时必须还回 FREE，否则永远分不到。

</details>

**位置**：`kernel/rt/rt_worker.c:578`

<a id="fn-kernel-rt-rt_worker.c-690"></a>

### `rt_worker_start`

```c
int rt_worker_start(uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁把 worker 置 RUNNING 并清停止标志，解锁后创建 FIFO 内核任务，失败回滚状态。

<details>
<summary>函数内备注（2 条）</summary>

**第 704 行（行尾）**

> 保留 DATA：允许先投递后 start

**第 710 行**

> task_create_kernel_rt 可能触发 self-IPI / 调度，不能在 g_rtw_lock
> 下调用。state=RUNNING 已经占住，别的 START 会看到 EBUSY。

</details>

**位置**：`kernel/rt/rt_worker.c:690`

<a id="fn-kernel-rt-rt_worker.c-730"></a>

### `rt_worker_stop`

```c
int rt_worker_stop(uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

置停止标志并发布 STOP 事件，若任务处于阻塞态则清超时并唤醒它。

<details>
<summary>函数内备注（2 条）</summary>

**第 744 行**

> event worker 可能无限阻塞在 rt_event_wait()；STOP 位保证它一定被唤醒。
> 周期 worker 不走这里，但置位无害。必须在 g_rtw_lock 内发布：否则
> worker 可能先退出、槽位被 destroy/recreate，STOP 会打到新 worker。

**第 753 行**

> 如果 worker 正睡在 tick 上，清掉 sleep_expire 并唤醒；它醒来会在
> rtw_sleep_until() 里先检查 stop，然后退出。任务正在 step 里跑的话
> 不需要额外动作 —— 本轮结束就会看到 stop。

</details>

**位置**：`kernel/rt/rt_worker.c:730`

<a id="fn-kernel-rt-rt_worker.c-765"></a>

### `rt_worker_destroy`

```c
int rt_worker_destroy(uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拒绝销毁运行中的 worker，清空共享页标识与槽位后释放其占用的设备。

<details>
<summary>函数内备注（1 条）</summary>

**第 781 行**

> 页保留给同槽位下次 create；旧用户映射靠 magic=0 判断失效。

</details>

**位置**：`kernel/rt/rt_worker.c:765`

<a id="fn-kernel-rt-rt_worker.c-794"></a>

### `rt_worker_set_input`

```c
int rt_worker_set_input(uint32_t id, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁找到 worker 后，以 release 语义写入内部与共享页的输入值。

**位置**：`kernel/rt/rt_worker.c:794`

<a id="fn-kernel-rt-rt_worker.c-810"></a>

### `rt_worker_post_event`

```c
int rt_worker_post_event(uint32_t id, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

双重锁下把值写入事件环并推进 head，环满则记丢弃返回 ENOSPC，成功则置 DATA 位。

<details>
<summary>函数内备注（2 条）</summary>

**第 821 行**

> 生产者可能来自控制面和驱动 ISR，用 ev_lock 串行化；消费者（RT worker）
> 只读 head/tail，不需要这把锁。锁在 g_rtw_lock 内层，保证 post 与
> destroy/recreate 不会并发。

**第 843 行**

> 在 g_rtw_lock 内唤醒，防止 worker 被并发 destroy 后还访问 event。

</details>

**位置**：`kernel/rt/rt_worker.c:810`

<a id="fn-kernel-rt-rt_worker.c-849"></a>

### `rt_worker_set_params`

```c
int rt_worker_set_params(uint32_t id, const uint64_t params[RTW_PARAM_MAX])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁后用奇偶序号递增的 seqlock 把参数数组写入 worker 与共享页。

<details>
<summary>函数内备注（1 条）</summary>

**第 862 行**

> seqlock：奇数表示写入中；worker 在周期开头重试读取。

</details>

**位置**：`kernel/rt/rt_worker.c:849`

<a id="fn-kernel-rt-rt_worker.c-881"></a>

### `rt_worker_shm_lookup`

```c
int rt_worker_shm_lookup(uint32_t id, uint64_t *paddr, uint64_t *size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁返回 worker 共享页的物理地址与页大小。

**位置**：`kernel/rt/rt_worker.c:881`

<a id="fn-kernel-rt-rt_worker.c-899"></a>

### `rt_worker_peek_progress`

```c
bool rt_worker_peek_progress(uint32_t id, uint64_t *seq, uint32_t *state)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁读取 worker 的输出序号与状态，未找到返回 false。

**位置**：`kernel/rt/rt_worker.c:899`

<a id="fn-kernel-rt-rt_worker.c-916"></a>

### `rt_worker_get_output`

```c
int rt_worker_get_output(uint32_t id, uint64_t *value, uint64_t *seq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁读取 worker 的最近输出值与输出序号，不存在返回 ENOENT。

**位置**：`kernel/rt/rt_worker.c:916`

<a id="fn-kernel-rt-rt_worker.c-933"></a>

### `rt_worker_get_stats`

```c
int rt_worker_get_stats(uint32_t id, struct rtw_stats *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁把状态、周期、优先级、执行与唤醒延迟、错过及事件计数打包进统计结构。

**位置**：`kernel/rt/rt_worker.c:933`

<a id="fn-kernel-rt-rt_worker.c-989"></a>

### `rt_worker_reset_stats`

```c
int rt_worker_reset_stats(uint32_t id)
```

把统计窗口清零，不改变 worker 的配置、输出和事件环内容。
运行中的 worker 不会暂停，所以窗口边界是近似的：重置后仍可能有
一两次恰好并发完成的更新落在新窗口里。设备写错误和 watchdog 计数
也属于这个窗口。

<details>
<summary>函数内备注（1 条）</summary>

**第 1015 行**

> 生产者在本锁内，消费者只做原子递增；head/tail 不动，环内容不丢。

</details>

**位置**：`kernel/rt/rt_worker.c:989`

<a id="fn-kernel-rt-rt_worker.c-1035"></a>

### `rt_worker_list`

```c
int rt_worker_list(struct rtw_list_req *list)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁遍历槽位把非空闲 worker 概要填进列表，按容量截断并写回条目数。

**位置**：`kernel/rt/rt_worker.c:1035`

<a id="mod-kernel-rt-rtdev.c"></a>

## kernel/rt/rtdev.c — RT 模板可用的设备契约与注册表

`kernel/rt/rtdev.c` · 13 个函数 · 13 个有说明 · 203 行

只有满足 rtdev_ops 约束的驱动才注册到这里。详见 include/rt/rtdev.h。
当前内置一个示例设备 "vout"：把每次写当成 u64 记录下来，用于验证
“worker → rtdev → safe_state” 整条路径；真实 GPIO/PWM/UART 驱动按同一
契约注册即可。

<a id="fn-kernel-rt-rtdev.c-31"></a>

### `rtdev_find_locked`

```c
static struct rtdev *rtdev_find_locked(const char *name)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历设备表返回名称匹配且已注册的 rtdev 指针，未命中返回 NULL。

**位置**：`kernel/rt/rtdev.c:31`

<a id="fn-kernel-rt-rtdev.c-40"></a>

### `rtdev_register`

```c
int rtdev_register(const char *name, const struct rtdev_ops *ops, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验参数与重名后加锁占用空闲槽位，登记名称、操作表与上下文，表满返回 ENOSPC。

**位置**：`kernel/rt/rtdev.c:40`

<a id="fn-kernel-rt-rtdev.c-69"></a>

### `rtdev_claim`

```c
struct rtdev *rtdev_claim(const char *name, uint32_t owner)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁占用无主设备并记录 owner，再到锁外调用 setup，失败则释放所有权。

<details>
<summary>函数内备注（1 条）</summary>

**第 84 行**

> setup 允许睡眠/分配，必须在锁外调用。owner 已经占住，别的 worker
> 不可能在 setup 期间抢走。

</details>

**位置**：`kernel/rt/rtdev.c:69`

<a id="fn-kernel-rt-rtdev.c-98"></a>

### `rtdev_release`

```c
void rtdev_release(struct rtdev *d, uint32_t owner)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先调用设备 safe_state 与 teardown，再加锁清除该 owner 的占用标记。

<details>
<summary>函数内备注（1 条）</summary>

**第 103 行**

> 先让设备回安全态（RT-safe，不睡眠），再释放所有权。

</details>

**位置**：`kernel/rt/rtdev.c:98`

<a id="fn-kernel-rt-rtdev.c-116"></a>

### `rtdev_write`

```c
int rtdev_write(struct rtdev *d, const void *buf, uint32_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

参数或 write 回调缺失时返回 ENODEV，否则转发给设备 write 操作。

**位置**：`kernel/rt/rtdev.c:116`

<a id="fn-kernel-rt-rtdev.c-123"></a>

### `rtdev_read`

```c
int rtdev_read(struct rtdev *d, void *buf, uint32_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

参数或 read 回调缺失时返回 ENODEV，否则转发给设备 read 操作。

**位置**：`kernel/rt/rtdev.c:123`

<a id="fn-kernel-rt-rtdev.c-130"></a>

### `rtdev_safe_state`

```c
void rtdev_safe_state(struct rtdev *d)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

设备及其 safe_state 回调存在时调用之，否则无操作。

**位置**：`kernel/rt/rtdev.c:130`

<a id="fn-kernel-rt-rtdev.c-145"></a>

### `vout_setup`

```c
static int vout_setup(struct rtdev *d, const void *cfg, uint32_t cfg_len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清零全局 vout 上下文的上次输出值与写入计数。

**位置**：`kernel/rt/rtdev.c:145`

<a id="fn-kernel-rt-rtdev.c-155"></a>

### `vout_write`

```c
static int vout_write(struct rtdev *d, const void *buf, uint32_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验缓冲至少 8 字节后取出一个值存入上下文，并递增写入计数。

**位置**：`kernel/rt/rtdev.c:155`

<a id="fn-kernel-rt-rtdev.c-167"></a>

### `vout_read`

```c
static int vout_read(struct rtdev *d, void *buf, uint32_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验缓冲长度后把上下文保存的上次输出值拷入缓冲 8 字节。

**位置**：`kernel/rt/rtdev.c:167`

<a id="fn-kernel-rt-rtdev.c-176"></a>

### `vout_safe_state`

```c
static void vout_safe_state(struct rtdev *d)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 vout 上下文的上次输出值清零。

**位置**：`kernel/rt/rtdev.c:176`

<a id="fn-kernel-rt-rtdev.c-182"></a>

### `vout_teardown`

```c
static void vout_teardown(struct rtdev *d)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 vout 的累计写入次数与最后输出值到内核日志。

**位置**：`kernel/rt/rtdev.c:182`

<a id="fn-kernel-rt-rtdev.c-197"></a>

### `rtdev_register_builtin`

```c
void rtdev_register_builtin(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

幂等注册内置 vout 设备，注册成功后置全局已注册标志。

**位置**：`kernel/rt/rtdev.c:197`

<a id="mod-kernel-syscall-core-futex.c"></a>

## kernel/syscall/core/futex.c — 简易 futex 实现(fixed-size 表, 无哈希)

`kernel/syscall/core/futex.c` · 3 个函数 · 3 个有说明 · 123 行

仅支持 FUTEX_WAIT / FUTEX_WAKE 基本语义, 足以让 musl pthread mutex / cond 工作。
派生自 kernel/syscall/syscall.c, 作为大重构的第一步抽出。

<a id="fn-kernel-syscall-core-futex.c-35"></a>

### `futex_do_wake`

```c
int futex_do_wake(uintptr_t uaddr, int count)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁摘取同地址的至多 count 个等待者并清槽，解锁后逐个 sched_unblock，返回唤醒数。

<details>
<summary>函数内备注（1 条）</summary>

**第 51 行**

> 唤醒放在锁外, 走与 pipe/completion 同一条 sched_unblock()：
> 等待者可能还在这颗核上(已置 BLOCKED, 尚未切出), 不能直接
> state=READY + sched_enqueue, 否则会被另一颗核重复执行。

</details>

**位置**：`kernel/syscall/core/futex.c:35`

<a id="fn-kernel-syscall-core-futex.c-61"></a>

### `sys_futex_wait`

```c
int sys_futex_wait(uint32_t *uaddr, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

经 copy_from_user 读用户值，持锁比对不等返回 EAGAIN，否则登记等待槽、置阻塞并调度。

<details>
<summary>函数内备注（4 条）</summary>

**第 66 行**

> 用户指针一律经 copy_from_user_bytes 读：这是来自 syscall 参数的地址,
> 裸解引用(原来的 *(volatile uint32_t *)uaddr)遇到坏地址就是内核态
> 数据中止 → 整机挂死。见 CLAUDE.md 的用户指针规则。

**第 76 行**

> 原子检查：若 *uaddr != val, 立即返回 EAGAIN

**第 82 行**

> 找空闲槽

**第 102 行**

> 让出 CPU；被 futex_do_wake 设回 TASK_READY 后继续

</details>

**位置**：`kernel/syscall/core/futex.c:61`

<a id="fn-kernel-syscall-core-futex.c-111"></a>

### `futex_handler`

```c
void futex_handler(uint64_t regs[6])
```

futex syscall 派发：仅支持 FUTEX_WAIT / FUTEX_WAKE 基本语义

<details>
<summary>函数内备注（1 条）</summary>

**第 116 行**

> FUTEX_WAKE: regs[2] 是唤醒数量

</details>

**位置**：`kernel/syscall/core/futex.c:111`

<a id="mod-kernel-syscall-core-proc_ids.c"></a>

## kernel/syscall/core/proc_ids.c — 进程身份 / 进程组 / 会话相关 syscall

`kernel/syscall/core/proc_ids.c` · 2 个函数 · 2 个有说明 · 202 行

从 kernel/syscall/syscall.c 抽出。覆盖：
getpid / getppid / gettid / set_tid_address
getuid / geteuid / getgid / getegid / setuid / setgid
getresuid / getresgid / setresuid / setresgid
getpgid / setpgid / getsid / setsid
getgroups / setgroups

当前模型：root 可任意 setuid/setgid；不做权限检查。

<a id="fn-kernel-syscall-core-proc_ids.c-26"></a>

### `process_group_exists`

```c
static bool process_group_exists(uint32_t pgid)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历任务池，跳过未用、DEAD 与 ALLOCATING 项，命中相同 pgid 即返回 true。

**位置**：`kernel/syscall/core/proc_ids.c:26`

<a id="fn-kernel-syscall-core-proc_ids.c-39"></a>

### `proc_ids_handler`

```c
void proc_ids_handler(uint64_t syscall_num, uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 syscall 号分发 uid/gid/pid/pgid/sid 的查询与设置及 setsid 等，结果写回 regs[0]。

<details>
<summary>函数内备注（4 条）</summary>

**第 43 行**

> musl 线程初始化时设置线程 &#x27;clear-child-tid&#x27; 地址

**第 45 行**

> fall through

**第 109 行**

> 用户指针必须经 copy_to_user_bytes；裸写会让未映射地址变成内核态 #PF

**第 198 行**

> 不应到达：调用方负责派发已知号码

</details>

**位置**：`kernel/syscall/core/proc_ids.c:39`

<a id="mod-kernel-syscall-core-proc_lifecycle.c"></a>

## kernel/syscall/core/proc_lifecycle.c — 进程/线程生命周期 syscall

`kernel/syscall/core/proc_lifecycle.c` · 8 个函数 · 8 个有说明 · 884 行

从 kernel/syscall/syscall.c 抽出, 覆盖：
exit / exit_group        → sys_exit() + exit_handler()
clone (fork + thread)    → clone_handler()
execve                   → sys_execve() + execve_handler()
wait4 / waitid           → wait_handler()

sys_exit / sys_execve 在 syscall.h 中已有 public 声明。

<a id="fn-kernel-syscall-core-proc_lifecycle.c-154"></a>

### `sys_exit`

```c
void sys_exit(int status)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

记录退出码，清 ctid 并唤醒 futex，关闭非线程 fd、给子进程投递 pdeathsig、结算时间后调用 task_exit。

<details>
<summary>函数内备注（7 条）</summary>

**第 162 行**

> CLONE_CHILD_CLEARTID: 清零 tid 并唤醒 pthread_join 等待者

**第 168 行**

> CLONE_VM/CLONE_FILES threads share fd objects with the process.

**第 171 行**

> 摘表项要原子：进程退出时可能还有别的线程在 close 同一个 fd,
> 两边都拿到同一个 idx 就会把同一个槽归还两次。

**第 182 行**

> prctl(PR_SET_PDEATHSIG)：父进程退出时给仍然存活的子进程投递信号。
> Linux 的语义是&quot;父线程死亡&quot;，所以 is_thread 的父任务退出也要发。

**第 198 行**

> 被阻塞的子进程要能醒来看见信号（默认动作通常是终止）

**第 203 行**

> 在退出前计算 utime： wall_time − stime

**第 218 行**

> 不应该到达这里

</details>

**位置**：`kernel/syscall/core/proc_lifecycle.c:154`

<a id="fn-kernel-syscall-core-proc_lifecycle.c-223"></a>

### `exit_handler`

```c
void exit_handler(uint64_t regs[6])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 regs[0] 当作退出状态调用 sys_exit。

**位置**：`kernel/syscall/core/proc_lifecycle.c:223`

<a id="fn-kernel-syscall-core-proc_lifecycle.c-228"></a>

### `exit_group_handler`

```c
void exit_group_handler(uint64_t regs[6])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

扫描同 tgid 的其他用户进程置 DEAD 并出队，然后以 regs[0] 为状态调用 sys_exit。

<details>
<summary>函数内备注（1 条）</summary>

**第 244 行**

> 不能直接写 state = DEAD 再 sched_dequeue：兄弟线程可能正在
> 自己的 sched_schedule() 里，DEAD 会被覆盖回 RUNNING（LTP
> exit_group01 的偶发失败）。sched_kill_task 在目标核 rq_lock
> 下完成摘链和置 DEAD，并 kick 正在运行的目标核。

</details>

**位置**：`kernel/syscall/core/proc_lifecycle.c:228`

<a id="fn-kernel-syscall-core-proc_lifecycle.c-260"></a>

### `sys_execve`

```c
int64_t sys_execve(const char *pathname, char **argv, char **envp, trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从用户态拷入 argv/envp 到每 CPU 缓冲，解析绝对路径后调用 elf_loader 原地替换映像并返回结果。

<details>
<summary>函数内备注（5 条）</summary>

**第 269 行**

> execve 现在原地替换映像: fd 表保留, 只有 FD_CLOEXEC 项由
> task_execve 关闭(Linux 语义)。连接 socket、控制台等不受影响。

**第 273 行**

> 从 userspace 复制 argv(按 CPU 取行, 理由见 g_exec_arg_store 的注释)

**第 303 行**

> envp 也必须从用户空间拷进来；以前直接传 NULL，子进程拿不到任何环境变量

**第 328 行**

> 将相对路径转为绝对路径

**第 332 行**

> 成功时当前进程已被原地替换成新程序(PID 不变), 返回 0;
> 调用方的 syscall 返回路径会把控制权交给新入口。
> 失败时旧映像不变, 返回负错误码。

</details>

**位置**：`kernel/syscall/core/proc_lifecycle.c:260`

<a id="fn-kernel-syscall-core-proc_lifecycle.c-341"></a>

### `execve_handler`

```c
void execve_handler(uint64_t regs[6], trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取出路径、argv、envp 参数调用 sys_execve，并把返回值写回 regs[0]。

**位置**：`kernel/syscall/core/proc_lifecycle.c:341`

<a id="fn-kernel-syscall-core-proc_lifecycle.c-353"></a>

### `clone_handler`

```c
void clone_handler(uint64_t regs[6], task_t *parent, trap_frame_t *frame)
```

clone (fork + thread)

<details>
<summary>函数内备注（17 条）</summary>

**第 361 行**

> 分配子任务槽。走和 task_create 同一条路(task_alloc_slot)——
> 以前这里内联抄了一份自己的认领循环, 三个问题：
> 1. 无锁：两核同时 fork 会认领到同一个槽(共用 TCB + 栈)；
> 2. 不清零 TCB：子进程继承上一个占位者的 cpu_affinity /
> preempt_count 等残值(preempt_count 非 0 = 永久不可抢占)；
> 3. 回收循环只判 state == TASK_DEAD, 不判 parent_id：
> A 进程的 fork 会把 B 进程还没 wait 的僵尸收走, B 的
> waitpid 于是拿到 ECHILD(实测 SMP=4 下约 72%)。
> task_alloc_slot 内部已经处理了这三条。

**第 383 行（行尾）**

> fork/线程继承调度策略

**第 385 行（行尾）**

> rt_mutex 继承优先级不跨 fork 复制

**第 411 行**

> 信号处置继承：真正的线程组(CLONE_THREAD + CLONE_VM)共享
> signal_struct，fork/vfork 复制一份。放在分支之前做，失败时
> 子任务还没分配页表，回收只需要释放槽位。

**第 432 行**

> ── 共享地址空间路径：pthread 才是同一 thread group, vfork 仍是子进程。 ──
> task_alloc_slot 给每个任务分了一份空 mm；线程组改成共享父任务的
> 那一份（引用计数 +1），fork 路径则继续用自己那份新 mm。

**第 486 行**

> ── fork 路径 ──

**第 490 行（行尾）**

> signal/files/fs/mm 都还没发布

**第 499 行**

> 子进程 PGD 同样需要架构要求的内核映射。

**第 551 行**

> mmap 区从本进程真实的基址扫起, 不是写死的 USER_MMAP_BASE_EXEC。
> PIE 进程的基址是 0x50000000, 用 0x30000000 当起点会逐页空扫
> 512MB 的 VA 空洞(实测每次 fork 131,077 次四级页表遍历, 约 9ms)。
> 下界取 mmap_base 是安全的：mmap_next 是递增高水位, 且 munmap 的
> 回退下限也是 mmap_base(见 mmap.c), 所以 [mmap_base, mmap_next)
> 必然覆盖该进程全部的 mmap 映射。

**第 564 行**

> 动态链接器(ld-musl)那一带必须单独复制。

> exec 把 interp 加载在 USER_INTERP_BASE(0x40000000), 而上面三个区间
> 一个都不覆盖它([0,heap_end) 只到主程序末端, 对动态 busybox 就是
> 0x10E000)。而 musl 的 fork() 正是在 ld-musl 内部调 clone 的 ——
> 子进程从 syscall 返回时 PC 就在 0x4003xxxx, 那一页不存在 →
> 取指页错误, 子进程一启动就 SIGSEGV。

> 实测(SG2002 上跑动态链接的 busybox, 一条 fork 就中)：
> [clone/fork] parent=3 child=4 elr=0x4003d892 usp=0x6ffff690
> [exception] sync: code=12 pc=0x4003d892 stval=0x4003d892
> 判据是 code=12(RISC-V 指令页错误)且 stval == pc —— 和 x86_64 那个
> sigret 缺失(RIP==CR2, I/D=1)是同一类指纹：跳到未映射页执行。

> 为什么一直没被发现：默认 rootfs 装的是静态 busybox, 没有 interp,
> interp_end 恒为 0, 下面这行是空操作。只有跑动态链接的程序才会撞上。

> 为什么不干脆扫 [USER_INTERP_BASE, USER_MMAP_BASE_PIE)：那是 256MB 的
> 空洞, 每个 fork 白扫 65536 页 —— 正是 mmap_base 那条注释里抱怨过的
> 那种浪费。interp_end 只覆盖真正映射过的部分(ld-musl 约几十页)。

**第 590 行**

> x86_64 的信号返回蹦床页, 必须单独补一页。

> USER_SIGRET_PAGE = 0x70001000, 比用户栈顶 USER_STACK_TOP = 0x70000000
> 还高 0x1000 —— 上面三个区间一个都不覆盖它。它由 exec 映射
> (kernel/task/exec.c 的 4b), fork 却从来没复制过, 于是：

> fork 出来的子进程 → 用户态装了 handler → 收到信号 → handler 返回时
> `ret` 到 sa_restorer = USER_SIGRET_PAGE → 该页不存在 → 取指页故障。

> 实测(LTP kill02)：
> User exception #14 at RIP=0x70001000
> User PF CR2=0x70001000, bits: P=0 W=0 U=1 R=0 I=1   ← RIP==CR2 且 I/D=1
> RIP 等于 CR2 且是取指, 正是「跳到未映射页执行」的指纹。

> 为什么只有 x86_64 需要：aarch64/riscv64 在 sa_restorer == 0 时是
> 在用户栈上写一个 rt_sigreturn 蹦床(见 signal.c), 不依赖固定页。
> x86_64 不能用这招 —— 用户栈是 NX 的, 栈上蹦床取不了指。

> 宏内部对未映射页有 `src_pa == 0 → continue` 的保护, 父进程若也没有
> 这一页(例如父进程自己就是修复前 fork 出来的), 这里是安全的空操作。

**第 621 行**

> 不能只 free 根页表：mm_vm_map_pages 已经建好的中间页表,
> 已复制的私有页, 已 ref 的共享页(NOFREE)都挂在树上。
> vm_destroy_user_process() 会整棵回收, 并按 KEEP/NOFREE
> 的规则处理 leaf。

**第 642 行**

> 上面已按 [USER_INTERP_BASE, interp_end) 复制过, 子进程同样要记住

**第 656 行**

> 深拷贝 fd_table

**第 698 行**

> 信号继承（signal_struct 已在进入分支前共享/复制）

**第 706 行**

> Linux fork 的进程属性继承：
> - umask / 附加组 / rlimits / CPU 亲和性：继承
> - PR_SET_PDEATHSIG：不继承（子进程清零）
> - set_robust_list：不继承
> - nanosleep 状态：清零

**第 740 行**

> 父进程返回子进程 PID

</details>

**位置**：`kernel/syscall/core/proc_lifecycle.c:353`

<a id="fn-kernel-syscall-core-proc_lifecycle.c-757"></a>

### `wait_write_result`

```c
static bool wait_write_result(uint64_t ustatus, uint64_t urusage, const task_t *child)
```

wait_write_result - 把 wait 的结果写回用户空间

ustatus / urusage 是用户给的指针(可为 NULL)。必须走 copy_to_user_bytes：
以前这里直接 `*wstatus = ...`, 用户传一个非法地址(例如 (void*)-1)就会
让内核态写到坏地址, 触发 #PF 后落进异常处理的死循环, 整个系统挂死。

返回 false 表示用户指针非法(调用者应回 -EFAULT)。

<details>
<summary>函数内备注（1 条）</summary>

**第 766 行（行尾）**

> sizeof(struct rusage) == 144

</details>

**位置**：`kernel/syscall/core/proc_lifecycle.c:757`

<a id="fn-kernel-syscall-core-proc_lifecycle.c-779"></a>

### `wait_handler`

```c
void wait_handler(uint64_t regs[6], task_t *me)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

查找匹配子进程与已死子进程，命中则回写状态并回收；WNOHANG 返回 0，否则置等待标志阻塞后重新查找。

<details>
<summary>函数内备注（4 条）</summary>

**第 789 行**

> 先判断是否存在匹配的子进程

**第 811 行**

> 查找已退出的子进程

**第 841 行**

> 无已退出的子进程

**第 848 行**

> 阻塞等待

</details>

**位置**：`kernel/syscall/core/proc_lifecycle.c:779`

<a id="mod-kernel-syscall-core-resource.c"></a>

## kernel/syscall/core/resource.c — 身份/资源/时间类 Linux syscall

`kernel/syscall/core/resource.c` · 10 个函数 · 10 个有说明 · 434 行

覆盖：
getgroups / setgroups
getrlimit / setrlimit / prlimit64
umask
prctl（PDEATHSIG / DUMPABLE / NAME / NO_NEW_PRIVS）
set_robust_list
times

设计要点：
- 这些 syscall 以前是静默成功型桩（返回 0 但不改状态），调用方会
误以为生效。这里把状态真正挂到 task_t 上，并在 fork/exec 按 Linux
语义继承。
- 用户指针一律经 copy_to/from_user_bytes（CLAUDE.md 的「用户指针」
规则），坏地址返回 EFAULT 而不是把内核打挂。
- 权限模型只有 euid == 0 这一条（与项目现有 setuid 模型一致），
不实现 capability。

<a id="fn-kernel-syscall-core-resource.c-62"></a>

### `rlimit_res_valid`

```c
static bool rlimit_res_valid(int res)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断资源号是否落在 0 到 TASK_RLIMIT_COUNT 之前。

**位置**：`kernel/syscall/core/resource.c:62`

<a id="fn-kernel-syscall-core-resource.c-71"></a>

### `rlimit_apply`

```c
static int rlimit_apply(task_t *t, int res, const struct kernel_rlimit *newlim)
```

setrlimit 的公共校验/写入。返回 0 或负 errno。
newlim 已经是从用户空间拷进来的内核副本。

<details>
<summary>函数内备注（2 条）</summary>

**第 78 行**

> NOFILE 的硬上界：即使 root 也不能超过 nr_open。Linux 返回 EPERM
> （不是 EINVAL），LTP setrlimit03 就是按这个判的。

**第 85 行**

> 非 root 不得提高硬限制

</details>

**位置**：`kernel/syscall/core/resource.c:71`

<a id="fn-kernel-syscall-core-resource.c-94"></a>

### `rlimit_get`

```c
static void rlimit_get(task_t *t, int res, struct kernel_rlimit *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把任务指定资源的软硬限制复制到输出结构。

**位置**：`kernel/syscall/core/resource.c:94`

<a id="fn-kernel-syscall-core-resource.c-102"></a>

### `syscall_groups_handler`

```c
void syscall_groups_handler(uint64_t syscall_num, uint64_t regs[6], task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

getgroups 校验大小并拷出附加组；setgroups 要求 euid 为 0，拷入后写入任务组表。

<details>
<summary>函数内备注（3 条）</summary>

**第 112 行**

> getgroups(0, ...)：只返回数量，不写用户缓冲(POSIX 特例)

**第 131 行**

> setgroups(size, list)

**第 139 行**

> CAP_SETGID：本内核只认 root

</details>

**位置**：`kernel/syscall/core/resource.c:102`

<a id="fn-kernel-syscall-core-resource.c-161"></a>

### `syscall_rlimit_handler`

```c
void syscall_rlimit_handler(uint64_t syscall_num, uint64_t regs[6], task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

处理 get/setrlimit 与 prlimit64，校验资源号与用户指针并做权限检查后读写限制。

<details>
<summary>函数内备注（2 条）</summary>

**第 207 行**

> prlimit64(pid, resource, new_limit, old_limit)

**第 220 行**

> 非 root 不能改/看别人的限制

</details>

**位置**：`kernel/syscall/core/resource.c:161`

<a id="fn-kernel-syscall-core-resource.c-259"></a>

### `syscall_umask_handler`

```c
void syscall_umask_handler(uint64_t regs[6], task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存旧 umask，把新值掩蔽为 0777 后写入任务并返回旧值。

**位置**：`kernel/syscall/core/resource.c:259`

<a id="fn-kernel-syscall-core-resource.c-277"></a>

### `syscall_prctl_handler`

```c
void syscall_prctl_handler(uint64_t regs[6], task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 option 处理 pdeathsig、dumpable、进程名、no_new_privs 的读取与设置，未知项返回 EINVAL。

<details>
<summary>函数内备注（1 条）</summary>

**第 329 行**

> 内核 comm 最长 TASK_NAME_LEN-1，超长截断(Linux 也截断)

</details>

**位置**：`kernel/syscall/core/resource.c:277`

<a id="fn-kernel-syscall-core-resource.c-373"></a>

### `syscall_robust_list_handler`

```c
void syscall_robust_list_handler(uint64_t regs[6], task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

长度等于头部大小时保存 robust_list 指针返回 0，否则返回 EINVAL。

**位置**：`kernel/syscall/core/resource.c:373`

<a id="fn-kernel-syscall-core-resource.c-393"></a>

### `syscall_personality_handler`

```c
void syscall_personality_handler(uint64_t regs[6], task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

非 0xffffffff 时更新任务 personality，并始终返回旧值。

<details>
<summary>函数内备注（1 条）</summary>

**第 398 行**

> personality(0xffffffff) 是查询当前 persona(Linux 约定)

</details>

**位置**：`kernel/syscall/core/resource.c:393`

<a id="fn-kernel-syscall-core-resource.c-406"></a>

### `syscall_times_handler`

```c
void syscall_times_handler(uint64_t regs[6], task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按当前纳秒与 stime、创建时间算出 utime/stime 滴答，可选拷出 tms，返回总滴答。

<details>
<summary>函数内备注（2 条）</summary>

**第 412 行**

> struct tms { clock_t tms_utime, stime, cutime, cstime; }
> clock_t 在 64 位 Linux 上是 long(8 字节) → 共 32 字节。

**第 425 行（行尾）**

> 子进程时间：本内核未累计

</details>

**位置**：`kernel/syscall/core/resource.c:406`

<a id="mod-kernel-syscall-core-sched.c"></a>

## kernel/syscall/core/sched.c — 调度相关 syscall(sched_yield, nanosleep)

`kernel/syscall/core/sched.c` · 2 个函数 · 2 个有说明 · 108 行

从 kernel/syscall/syscall.c 抽出。

nanosleep 现在是真睡眠：
- 截止时刻记在 task->sleep_expire_ns，由 100Hz tick(kernel/time/itimer.c)
在到期时清标记并 task_unblock()；
- 有会真正打断阻塞的信号(task_signal_pending_effective)时提前返回
-EINTR，并把剩余时间写回 rem；
- 不再用 yield 空转，也不再有"睡 1ms 实际占满 CPU"的行为。

<a id="fn-kernel-syscall-core-sched.c-24"></a>

### `sched_yield_handler`

```c
void sched_yield_handler(uint64_t regs[6])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 task_yield 让出 CPU 并把 regs[0] 置 0。

**位置**：`kernel/syscall/core/sched.c:24`

<a id="fn-kernel-syscall-core-sched.c-30"></a>

### `nanosleep_handler`

```c
void nanosleep_handler(uint64_t regs[6])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拷入并校验 timespec，设睡眠到期时间，循环阻塞至到期或信号打断，被打断回填剩余时间并返回 EINTR。

<details>
<summary>函数内备注（3 条）</summary>

**第 64 行**

> 睡到到期或被有效信号打断。tick 到期时会把 sleep_expire_ns 清 0，
> 所以&quot;标记消失&quot;本身就代表睡完了。

**第 75 行**

> tick 已处理到期：直接成功返回

**第 89 行**

> -EINTR：回填剩余时间（Linux 语义；rem 可为 NULL）

</details>

**位置**：`kernel/syscall/core/sched.c:30`

<a id="mod-kernel-syscall-core-signal.c"></a>

## kernel/syscall/core/signal.c — 内核信号子系统(投递 + 三个 syscall 处理器)

`kernel/syscall/core/signal.c` · 9 个函数 · 9 个有说明 · 439 行

从 kernel/syscall/syscall.c 抽出。包含：
- deliver_pending_signals(): syscall 返回用户态前选取并投递一个信号
- sigaction_handler():       rt_sigaction (Linux 13)
- sigprocmask_handler():     rt_sigprocmask (Linux 14)
- sigreturn_handler():       rt_sigreturn (Linux 15)

入口字符(Ctrl+C / 普通 stdin)的 UART → ringbuf 投递路径仍位于
syscall.c 中(signal_check_uart()), 因其耦合 g_termios 全局状态。

<a id="fn-kernel-syscall-core-signal.c-34"></a>

### `deliver_pending_signals`

```c
void deliver_pending_signals(task_t *t, trap_frame_t *frame)
```

── deliver_pending_signals ────────────────────────────────────
在 syscall 即将返回用户态前调用(且 syscall_abi_set_ret 已执行)。
选取最低编号的待投递信号, 执行 SIG_DFL / SIG_IGN / 用户 handler。
用户 handler 通过在用户栈上构建 sigframe 实现。

<details>
<summary>函数内备注（25 条）</summary>

**第 40 行**

> 取最低编号的待投递信号

**第 51 行**

> 清除待投递位

**第 54 行**

> NULL signal_struct 等价于全 SIG_DFL；helper 会填好默认值。

**第 62 行（行尾）**

> 忽略

**第 65 行**

> 默认动作：SIGCHLD/SIGCONT/SIGURG/SIGWINCH/SIGTTIN/SIGTTOU/SIGTSTP → 忽略；其余 → 终止

**第 79 行（行尾）**

> unreachable

**第 83 行**

> 在 handler 执行期间屏蔽本信号(+ sa_mask)

**第 87 行**

> 在用户栈上压入当前 trap_frame(已含 syscall 返回值), 建立 sigframe。

> 用户栈地址一律经 copy_to_user_bytes 写(可能跨页, 可能未映射)：
> 裸 memcpy 一旦踩到未映射页就是内核态数据中止 → 整机挂死。
> 写失败就 goto sigframe_fault 回滚：不设 sig_frame_sp, 不改 trap_frame,
> 信号重新置回 pending 并恢复 blocked 掩码, 等下次再投递。

**第 98 行（行尾）**

> 16 字节对齐

**第 102 行（行尾）**

> sigframe 起始地址

**第 103 行（行尾）**

> 返回地址 = restorer

**第 110 行（行尾）**

> 第一个参数

**第 111 行（行尾）**

> 清 DF

**第 116 行**

> 如果 sa_restorer == 0, 在栈上放一个 rt_sigreturn 蹦床

**第 119 行（行尾）**

> mov x8, #139

**第 120 行（行尾）**

> svc #0

**第 131 行（行尾）**

> x0 = signum

**第 132 行（行尾）**

> lr  = restorer

**第 138 行**

> 如果 sa_restorer == 0, 在栈上放一个 rt_sigreturn 蹦床

**第 141 行（行尾）**

> li a7, 139

**第 142 行（行尾）**

> ecall

**第 152 行（行尾）**

> sp

**第 153 行（行尾）**

> a0 = signum

**第 154 行（行尾）**

> ra = restorer

**第 164 行**

> 用户栈不可写(地址被改坏 / 未映射 / 跨页)：回滚上面已做的状态改动,
> 让信号回到&quot;待投递&quot;, 并且不设 sig_frame_sp —— 否则 rt_sigreturn
> 会去读一段根本没写成功的垃圾。内核侧 trap_frame 保持原样, 任务继续跑。
> 相比原来的裸 memcpy(踩到未映射页 = 内核态数据中止 = 整机挂死),
> 这里退化成&quot;信号暂时投递不出去&quot;。

</details>

**位置**：`kernel/syscall/core/signal.c:34`

<a id="fn-kernel-syscall-core-signal.c-177"></a>

### `sigaction_handler`

```c
void sigaction_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 sigsetsize 与信号号，可选出旧动作、写入新动作，设置失败返回 ENOMEM。

**位置**：`kernel/syscall/core/signal.c:177`

<a id="fn-kernel-syscall-core-signal.c-216"></a>

### `sigprocmask_handler`

```c
void sigprocmask_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

可选出阻塞掩码，再按 how 修改屏蔽集，并强制 SIGKILL/SIGSTOP 不可屏蔽。

<details>
<summary>函数内备注（1 条）</summary>

**第 232 行**

> SIGKILL / SIGSTOP 不可屏蔽

</details>

**位置**：`kernel/syscall/core/signal.c:216`

<a id="fn-kernel-syscall-core-signal.c-248"></a>

### `sigpending_handler`

```c
void sigpending_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验大小与指针非空，把待决信号与阻塞掩码的交集拷回用户。

**位置**：`kernel/syscall/core/signal.c:248`

<a id="fn-kernel-syscall-core-signal.c-282"></a>

### `signal_deliver_from_trap`

```c
void signal_deliver_from_trap(void *frame_ptr)
```

signal_deliver_from_trap - 在 trap 返回用户态之前投递一个待处理信号

由各架构的异常/中断返回路径调用(AArch64 的 HANDLE_IRQ,
x86_64 与 RISC-V 的公共 stub)。

为什么必须有这一步：deliver_pending_signals() 的另一处调用点在 syscall
返回路径上, 而纯用户态自旋, 不发任何 syscall 的任务永远不会经过那里。
LTP 的 setsid01 正是这种形态 ——

void do_child_2(void) { for (;;) ; }        // 子进程死循环
父进程: setsid(); kill(pid, SIGKILL); wait(&status);   // 期望 status==9

没有 trap 返回路径上的投递, SIGKILL 永远不生效, 子进程不死, 父进程
wait() 永远等下去, 整个测试卡死。只有从用户态陷入时才投递：
内核态陷入时 frame 不是用户现场, 改它没有意义。

<details>
<summary>函数内备注（3 条）</summary>

**第 291 行（行尾）**

> 来自 EL1, 非用户现场

**第 296 行（行尾）**

> CS.RPL != 3, 来自内核

**第 301 行（行尾）**

> 来自 S 态, 非用户现场

</details>

**位置**：`kernel/syscall/core/signal.c:282`

<a id="fn-kernel-syscall-core-signal.c-309"></a>

### `sigreturn_handler`

```c
void sigreturn_handler(uint64_t regs[6], task_t *current, trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 sig_frame_sp 经 copy_from_user 恢复 trap_frame 与屏蔽字并回填返回寄存器，失败返回 EFAULT。

<details>
<summary>函数内备注（5 条）</summary>

**第 311 行**

> 从信号 handler 返回：恢复进入 handler 前的 trap_frame

**第 314 行**

> sigframe 在用户栈上, 且这是用户可控的地址(handler 里能改 sp,
> 也能 munmap 那段栈), 必须经 copy_from_user_bytes 读：裸 memcpy
> 踩到坏地址就是内核态数据中止 → 整机挂死。
> 该接口先校验再拷贝, 失败时 frame 不会被写坏半截。

**第 321 行**

> 读不回来：不清 sig_frame_sp(用户修正栈指针后还能重试),
> 不动 blocked_sigs, 不改 trap_frame, 直接返回 -EFAULT。

**第 329 行**

> 恢复信号投递前的 blocked_sigs

**第 331 行**

> 让 syscall_abi_set_ret 写入已恢复帧中的正确寄存器值(幂等)

</details>

**位置**：`kernel/syscall/core/signal.c:309`

<a id="fn-kernel-syscall-core-signal.c-349"></a>

### `kill_handler`

```c
void kill_handler(uint64_t regs[6], task_t *current)
```

── kill (Linux 129) ──────────────────────────────────────────
pid > 0 : 单进程; pid == 0 / -1 : 当前 pgid; pid < -1 : (-pid) pgid
sig == 0 : 仅做存在性检查

<details>
<summary>函数内备注（2 条）</summary>

**第 377 行**

> 简化：向 current 的 pgid 广播

**第 383 行**

> pid &lt; -1: send to process group (-pid)

</details>

**位置**：`kernel/syscall/core/signal.c:349`

<a id="fn-kernel-syscall-core-signal.c-397"></a>

### `tkill_handler`

```c
void tkill_handler(uint64_t regs[6])
```

── tkill (Linux 130) ─────────────────────────────────────────
旧接口：按 tid 向单个任务发送信号。

**位置**：`kernel/syscall/core/signal.c:397`

<a id="fn-kernel-syscall-core-signal.c-420"></a>

### `tgkill_handler`

```c
void tgkill_handler(uint64_t regs[6])
```

── tgkill (Linux 131) ────────────────────────────────────────
tgid 是线程组 leader 的 PID(不是 pgid)。

**位置**：`kernel/syscall/core/signal.c:420`

<a id="mod-kernel-syscall-core-signal_struct.c"></a>

## kernel/syscall/core/signal_struct.c — 进程级信号处置（signal_struct）

`kernel/syscall/core/signal_struct.c` · 7 个函数 · 7 个有说明 · 247 行

把 sig_action_t[NSIG] 从 task_t 里拆出来，原因见
docs/task_t_refactor.md：
- 2 KiB 的数组占了 TCB 的 58%，但典型进程只装 2~5 个 handler；
- Linux 中信号处置属于线程组（CLONE_THREAD 共享），不是每线程一份。

约定：
- task_t::sig == NULL 表示全部 SIG_DFL，不为没装 handler 的任务
浪费 2 KiB；第一次 rt_sigaction 或创建线程组时才分配。
- 静态池 g_signal_pool[TASK_MAX]：同时存在的 signal_struct 不会
超过活任务数，所以不需要动态分配，也不会在早期启动/IRQ 路径失败。
- refcount 的所有修改都在 g_signal_lock 下；task_t::sig 的读写用
acquire/release 原子操作，避免发布未初始化完的 actions。

<a id="fn-kernel-syscall-core-signal_struct.c-34"></a>

### `sig_alloc_locked`

```c
static signal_struct_t *sig_alloc_locked(void)
```

持锁调用：取一个空闲槽位并初始化为全 SIG_DFL（refcount=1）。

<details>
<summary>函数内备注（1 条）</summary>

**第 40 行（行尾）**

> 0 = SIG_DFL

</details>

**位置**：`kernel/syscall/core/signal_struct.c:34`

<a id="fn-kernel-syscall-core-signal_struct.c-47"></a>

### `task_sigaction_get`

```c
bool task_sigaction_get(task_t *t, int sig, sig_action_t *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先预填 SIG_DFL，原子读取任务信号结构并复制对应动作，无结构时返回 false。

**位置**：`kernel/syscall/core/signal_struct.c:47`

<a id="fn-kernel-syscall-core-signal_struct.c-63"></a>

### `task_sigaction_set`

```c
int task_sigaction_set(task_t *t, int sig, const sig_action_t *act)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁按需分配信号结构并写入动作，分配失败返回 -1，否则返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 72 行**

> 纯 SIG_DFL 且 mask/flags 全 0 时无需实例化（NULL 本身就是这个
> 语义）；但 SIG_DFL 带非零 mask 或 SIG_IGN 都要存下来，否则
> rt_sigaction 的 oldact 读回值会和 Linux 不一致。

</details>

**位置**：`kernel/syscall/core/signal_struct.c:63`

<a id="fn-kernel-syscall-core-signal_struct.c-100"></a>

### `task_signal_share`

```c
signal_struct_t *task_signal_share(task_t *parent)
```

task_signal_share - CLONE_THREAD：子线程与父任务共享同一份处置

如果父任务还是 NULL（线程组里没人装过 handler），先分配一份全
SIG_DFL 的实例。否则线程 A 装 handler 时只会写自己的 t->sig，
线程 B 仍看到 NULL，进程级语义就断了。

**位置**：`kernel/syscall/core/signal_struct.c:100`

<a id="fn-kernel-syscall-core-signal_struct.c-125"></a>

### `task_signal_copy`

```c
bool task_signal_copy(task_t *parent, task_t *child)
```

fork：复制父进程的处置（不是共享）。父进程为 NULL 时子进程也为 NULL。

**位置**：`kernel/syscall/core/signal_struct.c:125`

<a id="fn-kernel-syscall-core-signal_struct.c-154"></a>

### `task_signal_release`

```c
void task_signal_release(task_t *t)
```

TCB 回收时调用：最后一个引用释放后才把池槽位还回去。

**位置**：`kernel/syscall/core/signal_struct.c:154`

<a id="fn-kernel-syscall-core-signal_struct.c-179"></a>

### `task_signal_exec_reset`

```c
void task_signal_exec_reset(task_t *t)
```

exec：被 caught 的 handler 重置为 SIG_DFL，SIG_IGN 保留。

与旧实现一样保留 SIG_IGN（glibc/musl 依赖它跨 exec 忽略 SIGCHLD 等）。
和线程共享时先 unshare：exec 只应改变当前任务的处置，不能把还在
跑的其他线程的 handler 清掉。没有 SIG_IGN 可保留时直接把引用还回去，
让 t->sig 回到 NULL（省掉一份全默认的 2 KiB）。

<details>
<summary>函数内备注（2 条）</summary>

**第 215 行**

> 共享：没有要保留的 SIG_IGN 就只是解除共享。

**第 225 行**

> 池满（理论上是 64 个任务全部分配了 signal_struct，又恰好在
> exec）。退化为原地重置：当前任务语义正确，但会连带清掉共享
> 线程的 handler。exec 在多线程进程里本来就是罕见路径。

</details>

**位置**：`kernel/syscall/core/signal_struct.c:179`

<a id="mod-kernel-syscall-fs-dir.c"></a>

## kernel/syscall/fs/dir.c — getcwd / chdir / renameat / unlinkat

`kernel/syscall/fs/dir.c` · 6 个函数 · 6 个有说明 · 275 行

由 kernel/syscall/syscall.c 拆出。

<a id="fn-kernel-syscall-fs-dir.c-22"></a>

### `getcwd_handler`

```c
void getcwd_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

检查用户缓冲足够容纳当前 cwd 后拷出，返回含结尾 NUL 的长度。

**位置**：`kernel/syscall/fs/dir.c:22`

<a id="fn-kernel-syscall-fs-dir.c-58"></a>

### `chdir_copy_canonical`

```c
static int chdir_copy_canonical(const char *upath, char *out, int outsz)
```

chdir_copy_canonical - 分块读取用户路径并规范化到内核缓冲

为什么不能简单 copy_string_from_user 后判长度：
LTP chdir02 会传 1..PATH_MAX-1 个连续的 '/' —— Linux 认为它们
都是合法的根目录路径，而 128 字节的 cwd 缓冲一截断就误报
ENAMETOOLONG。反过来，chdir04 里 256 字节的**单个组件**必须报
ENAMETOOLONG。

所以按 Linux 规则判定：组件 > NAME_MAX 或总长 > PATH_MAX 才算超长；
规范化（合并 '/'、处理 '.'/'..'）之后若仍放不进 cwd 缓冲，才按
本内核的限制返回 ENAMETOOLONG。

逐页分块拷贝，避免"字符串贴在页尾、下一字节未映射"被整段预检误杀。

<details>
<summary>函数内备注（5 条）</summary>

**第 88 行（行尾）**

> 单个组件超过 NAME_MAX

**第 95 行**

> 一个组件结束：处理 &quot;.&quot; / &quot;..&quot; / 普通名字

**第 98 行**

> 当前目录，忽略

**第 115 行**

> 上一个已经是 &quot;..&quot;，再追加一个

**第 124 行（行尾）**

> 去掉分隔 &#x27;/&#x27;

</details>

**位置**：`kernel/syscall/fs/dir.c:58`

<a id="fn-kernel-syscall-fs-dir.c-159"></a>

### `chdir_handler`

```c
void chdir_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拷贝并规范化用户路径，stat 校验必须是目录（或 ext4 可打开），成功后更新任务 cwd。

<details>
<summary>函数内备注（2 条）</summary>

**第 167 行**

> 用户指针必须先拷进内核：resolve_path() 会直接逐字节解引用。
> 规范化结果必须放进 TASK_CWD_LEN。

**第 183 行**

> 普通文件不能 chdir —— Linux 返回 ENOTDIR

</details>

**位置**：`kernel/syscall/fs/dir.c:159`

<a id="fn-kernel-syscall-fs-dir.c-214"></a>

### `renameat_handler`

```c
void renameat_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分别解析新旧路径后调用 vfs_rename，跨挂载等错误原样返回。

<details>
<summary>函数内备注（1 条）</summary>

**第 241 行**

> 走 VFS：同一挂载内 rename；跨挂载返回 -EXDEV，让 busybox 走 copy+unlink

</details>

**位置**：`kernel/syscall/fs/dir.c:214`

<a id="fn-kernel-syscall-fs-dir.c-246"></a>

### `renameat2_handler`

```c
void renameat2_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

flags 非 0 时返回 EINVAL，否则直接转调 renameat_handler。

**位置**：`kernel/syscall/fs/dir.c:246`

<a id="fn-kernel-syscall-fs-dir.c-255"></a>

### `unlinkat_handler`

```c
void unlinkat_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析路径后调用 vfs_unlink，按 AT_REMOVEDIR 决定删除目录还是文件。

**位置**：`kernel/syscall/fs/dir.c:255`

<a id="mod-kernel-syscall-fs-fd_pool.c"></a>

## kernel/syscall/fs/fd_pool.c — 全局文件描述符对象池实现

`kernel/syscall/fs/fd_pool.c` · 13 个函数 · 13 个有说明 · 352 行

从 kernel/syscall/syscall.c 抽出。
fd_table[i] in task_t 持有进入本池的索引(-1 = 未打开)。
fd 0/1/2(stdin/stdout/stderr)特殊处理(UART), 不占用本池槽位。

<a id="fn-kernel-syscall-fs-fd_pool.c-39"></a>

### `fd_pool_alloc`

```c
int fd_pool_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

关中断持锁扫描空闲槽，标记为已分配并清空字段，返回槽号或 -1。

**位置**：`kernel/syscall/fs/fd_pool.c:39`

<a id="fn-kernel-syscall-fs-fd_pool.c-65"></a>

### `fd_pool_free`

```c
void fd_pool_free(int idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验下标后持锁清空等待队列与对象字段，把槽位标记为空闲。

**位置**：`kernel/syscall/fs/fd_pool.c:65`

<a id="fn-kernel-syscall-fs-fd_pool.c-83"></a>

### `fd_obj_ref`

```c
int fd_obj_ref(int idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验槽位后对 vfs_file 加引用或调用对象的 dup 回调，空闲槽返回 -1。

**位置**：`kernel/syscall/fs/fd_pool.c:83`

<a id="fn-kernel-syscall-fs-fd_pool.c-101"></a>

### `fd_obj_close`

```c
void fd_obj_close(int idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按对象类型分别关闭 vfs_file、销毁 epoll 实例或调用 ops->close。

**位置**：`kernel/syscall/fs/fd_pool.c:101`

<a id="fn-kernel-syscall-fs-fd_pool.c-120"></a>

### `fd_obj_attach_vfs`

```c
void fd_obj_attach_vfs(int idx, vfs_file_t *file)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 vfs 文件挂到指定池槽，设置类型与标志并复制文件路径。

**位置**：`kernel/syscall/fs/fd_pool.c:120`

<a id="fn-kernel-syscall-fs-fd_pool.c-139"></a>

### `task_alloc_fd`

```c
int task_alloc_fd(task_t *task, int pool_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁在 fd 表中找空位（受 RLIMIT_NOFILE 限制），写入池槽号并清除 CLOEXEC 位，返回新 fd。

<details>
<summary>函数内备注（5 条）</summary>

**第 141 行**

> 同一进程的两个线程共享 fd_table, 所以&quot;找 -1 槽 -&gt; 写上&quot;同样要互斥,
> 否则两个线程会拿到同一个 fd 号, 后写的那个把先写的挤掉。

**第 149 行**

> RLIMIT_NOFILE(7) 是软限制：fd 号必须小于 rlim_cur。
> 默认 1024 &gt; TASK_MAX_FD(256)，所以通常还是受静态表限制；
> 用户用 setrlimit 调小之后这里要真正生效(LTP setrlimit01 的 NOFILE 项)。

**第 155 行**

> fd 0,1,2 reserved for stdin/stdout/stderr

**第 162 行**

> 新 fd 默认不是 CLOEXEC。必须在这里清掉上一个占位者留下的位：
> close() 只把 fd_table 置 -1, 不动位图; 而 execve 现在会按位图
> 真正关闭 FD_CLOEXEC 的 fd —— 不清的话, 复用一个曾经标过
> CLOEXEC 的 fd 号会让新 fd 在下次 exec 时被意外关掉。

**第 181 行**

> 打印 fd_table 的前几个槽位用于调试

</details>

**位置**：`kernel/syscall/fs/fd_pool.c:139`

<a id="fn-kernel-syscall-fs-fd_pool.c-198"></a>

### `task_take_fd`

```c
int task_take_fd(task_t *task, int fd)
```

原子地摘走 fd_table[fd], 返回它原来的池索引(-1 = 本来就没开)。

close 路径必须用它而不是"先读 fd_table[fd] 再去做关闭"：同一进程的两个
线程同时 close 同一个 fd 时, 两边都会读到同一个 idx, 于是同一个槽被
关两次, 归还两次 —— 第二次归还时槽可能已经被别的 open 拿走了, 直接把
别人正在用的槽标成 FREE。摘表项这一步是"谁抢到谁负责关"的分界点。

**位置**：`kernel/syscall/fs/fd_pool.c:198`

<a id="fn-kernel-syscall-fs-fd_pool.c-213"></a>

### `task_get_fd`

```c
fd_obj_t *task_get_fd(task_t *task, int fd)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 fd 与表项下标，返回未释放的池对象指针，否则返回 NULL。

**位置**：`kernel/syscall/fs/fd_pool.c:213`

<a id="fn-kernel-syscall-fs-fd_pool.c-225"></a>

### `task_alloc_ion_fd`

```c
int task_alloc_ion_fd(task_t *task, uint32_t handle, const fd_ops_t *ops, void *priv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配池槽并填 ION 类型、句柄与 ops，挂到任务 fd 表，失败逐级回收并返回 -1。

**位置**：`kernel/syscall/fs/fd_pool.c:225`

<a id="fn-kernel-syscall-fs-fd_pool.c-257"></a>

### `task_get_ion_handle`

```c
int task_get_ion_handle(task_t *task, int fd, uint32_t *handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取 fd 对象，确认是 ION 类型后输出句柄，否则返回 -1。

**位置**：`kernel/syscall/fs/fd_pool.c:257`

<a id="fn-kernel-syscall-fs-fd_pool.c-266"></a>

### `task_alloc_dmabuf_fd`

```c
int task_alloc_dmabuf_fd(task_t *task, uint32_t idx, const fd_ops_t *ops, void *priv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配池槽并填 DMABUF 类型、索引与 ops，挂到任务 fd 表，失败逐级回收并返回 -1。

**位置**：`kernel/syscall/fs/fd_pool.c:266`

<a id="fn-kernel-syscall-fs-fd_pool.c-298"></a>

### `task_get_dmabuf_idx`

```c
int task_get_dmabuf_idx(task_t *task, int fd, uint32_t *idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取 fd 对象，确认是 DMABUF 类型后输出索引，否则返回 -1。

**位置**：`kernel/syscall/fs/fd_pool.c:298`

<a id="fn-kernel-syscall-fs-fd_pool.c-307"></a>

### `fd_table_inherit`

```c
void fd_table_inherit(task_t *child, task_t *parent)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

父非用户进程时清空子表；否则逐 fd 跳过 CLOEXEC 与 EPOLL，分配新池槽复制并增引用。

<details>
<summary>函数内备注（2 条）</summary>

**第 309 行**

> 只在内核任务/进程之间谈继承没意义：idle 之类的内核任务一张 fd 表都没有
> (正常全是 -1), 而且它们的 fd_table 一旦忘了初始化就是一片 0 ——
> 而 0 是合法槽位号, 会被当成「256 个 fd」逐个拷贝, 一次就把整个池子
> 吃光(实测过：idle/1 作父进程, 128 个槽瞬间占满, 此后宿主里所有
> open() 都失败)。这里直接按「没有可继承的 fd」处理, 表清成 -1。

**第 323 行**

> execve: FD_CLOEXEC 标记的 fd 不继承

</details>

**位置**：`kernel/syscall/fs/fd_pool.c:307`

<a id="mod-kernel-syscall-fs-fd_pool.h"></a>

## kernel/syscall/fs/fd_pool.h — 全局 fd 对象池声明

`kernel/syscall/fs/fd_pool.h` · 1 个函数 · 1 个有说明 · 89 行

类型, 池数组与池/任务级 fd 操作函数声明。
实现位于 kernel/syscall/fd_pool.c。

<a id="fn-kernel-syscall-fs-fd_pool.h-84"></a>

### `fd_obj_file`

```c
static inline vfs_file_t *fd_obj_file(fd_obj_t *obj)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对象非空则返回其 vfs_file，否则返回 NULL。

**位置**：`kernel/syscall/fs/fd_pool.h:84`

<a id="mod-kernel-syscall-fs-file_io.c"></a>

## kernel/syscall/fs/file_io.c — read / write / writev / lseek / sendfile / fsync / fdatasync

`kernel/syscall/fs/file_io.c` · 11 个函数 · 11 个有说明 · 514 行

由 kernel/syscall/syscall.c 拆出。

<a id="fn-kernel-syscall-fs-file_io.c-36"></a>

### `trace_heavy_task`

```c
static bool trace_heavy_task(task_t *current)
```

本地这份曾经只查范围, 现在统一走 syscall/fs/path.c 的 user_range_accessible：
范围 + 每页映射。理由见那里的注释(writev02 用"合法但已 munmap"的地址
让 copy_*_bytes 里的裸拷贝踩空 → 内核态 #PF → 停机)。

**位置**：`kernel/syscall/fs/file_io.c:36`

<a id="fn-kernel-syscall-fs-file_io.c-51"></a>

### `prw_fd_check`

```c
static int prw_fd_check(fd_obj_t *obj, bool is_write)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 fd 对象与文件，目录、非普通文件或读写方向不符时分别返回错误码。

**位置**：`kernel/syscall/fs/file_io.c:51`

<a id="fn-kernel-syscall-fs-file_io.c-68"></a>

### `copy_iov_from_user`

```c
static int copy_iov_from_user(struct kernel_iovec *dst, const struct kernel_iovec *uiov, int iovcnt)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 iovcnt 与各段用户范围，逐段拷入内核，越界或过长返回 EFAULT 或 EINVAL。

**位置**：`kernel/syscall/fs/file_io.c:68`

<a id="fn-kernel-syscall-fs-file_io.c-87"></a>

### `read_handler`

```c
void read_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

文件 fd 先校验用户缓冲可访问再 vfs_read；stdin 则从 UART 环形缓冲读到换行或读满，遇信号返回 EINTR。

<details>
<summary>函数内备注（4 条）</summary>

**第 103 行**

> read 侧和 write 侧是同一个洞：vfs_read 会直接往用户缓冲里写
> (ext4_fread → memcpy), 未映射的地址就是内核态 #PF → 停机。
> write 侧现在把数据先搬进内核缓冲；read 侧方向相反, 代价最小的做法
> 是先验&quot;这一段每一页都映射着&quot;。校验通过后当前页表就是该进程的
> 用户页表(syscall 期间 CR3 未切走), 直接写是安全的。

**第 128 行**

> stdin 未重定向：通过 UART 环形缓冲区读取

**第 129 行（行尾）**

> !ICANON

**第 130 行（行尾）**

> ICRNL

</details>

**位置**：`kernel/syscall/fs/file_io.c:87`

<a id="fn-kernel-syscall-fs-file_io.c-158"></a>

### `pread64_handler`

```c
void pread64_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验偏移、fd 类型与用户缓冲范围后调用 vfs_pread，返回读取字节数。

**位置**：`kernel/syscall/fs/file_io.c:158`

<a id="fn-kernel-syscall-fs-file_io.c-200"></a>

### `write_handler`

```c
void write_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

文件 fd 分块把用户数据拷入栈缓冲再 vfs_write 并支持部分写，fd 1/2 转 sys_write 输出。

<details>
<summary>函数内备注（3 条）</summary>

**第 216 行**

> 注意： 用户缓冲必须先落进内核, 再把内核地址交给 VFS/ext4/块设备。

> 这里以前是把 buf(用户指针)裸传给 vfs_write, 于是一路穿到
> ramblk_bwrite 的 `memcpy(dst, buf, n)` —— 用户给个不在页表里的
> 地址就是内核态 #PF → platform_panic → 整机停机。

> LTP writev02 正是冲着这个来的, 它的 iovec 第一项就是
> `{(caddr_t)-1, 8192}`：期望内核返回 EFAULT, 而不是崩。
> 实测崩溃现场：CR2=0x50002000, U=0,
> #0 ramblk_bwrite #2 ext4_fwrite #5 write_handler #6 writev_handler

> 用分块而不是一次 kmalloc 整块：count 是用户给的, 可以大到离谱,
> 先分配再校验等于让用户决定内核分配多少；分块则天然有界, 第一块
> 拷不动就直接 EFAULT(copy_from_user_bytes 内部走 user_range_ok)。
> 与 Linux 的&quot;部分写&quot;语义也一致：已经写成功的字节数照常返回。

**第 242 行**

> 一个字节都没写成 → EFAULT；已写成的部分按部分写返回

**第 254 行（行尾）**

> 后端短写, 别硬凑

</details>

**位置**：`kernel/syscall/fs/file_io.c:200`

<a id="fn-kernel-syscall-fs-file_io.c-268"></a>

### `pwrite64_handler`

```c
void pwrite64_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验后分块拷贝用户缓冲，带偏移调用 vfs_pwrite，返回写入字节数并支持部分写。

<details>
<summary>函数内备注（1 条）</summary>

**第 295 行**

> 和 write_handler 同样的理由：用户缓冲必须先拷到内核，不能把用户指针
> 一路交给 ext4_fwrite。分块上限由栈上的 kbuf 决定。

</details>

**位置**：`kernel/syscall/fs/file_io.c:268`

<a id="fn-kernel-syscall-fs-file_io.c-325"></a>

### `readv_handler`

```c
void readv_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拷贝并校验 iovec，逐段调用 read_handler，遇短读或错误立即返回已读总量。

**位置**：`kernel/syscall/fs/file_io.c:325`

<a id="fn-kernel-syscall-fs-file_io.c-375"></a>

### `writev_handler`

```c
void writev_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 fd 与 iovec 后逐段调用 write_handler，遇短写或错误返回已写总量。

**位置**：`kernel/syscall/fs/file_io.c:375`

<a id="fn-kernel-syscall-fs-file_io.c-415"></a>

### `lseek_handler`

```c
void lseek_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取 fd 对象，有文件则调用 vfs_seek 并返回新偏移，否则返回 EBADF。

**位置**：`kernel/syscall/fs/file_io.c:415`

<a id="fn-kernel-syscall-fs-file_io.c-434"></a>

### `sendfile_handler`

```c
void sendfile_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验输入端为普通文件，循环读入栈缓冲再写目标文件或控制台，回写偏移并返回传输总量。

<details>
<summary>函数内备注（3 条）</summary>

**第 444 行**

> Linux sendfile 的 in_fd 必须是&quot;支持 mmap 的普通文件&quot;；pipe/socket/控制台
> 不是。返回 EINVAL 而不是 0 很关键：
> - busybox 的 bb_copyfd_eof() 第一次尝试 sendfile，拿到 EINVAL 才会
> 回退到 read/write；
> - 以前这里对非文件 in_fd 返回 0（&quot;EOF&quot;），cat 便以为读完了 ——
> `echo hi | cat` / `$(cat file)` 全部静默丢数据。

**第 460 行**

> 0/1/2 可能被 dup2 到 pipe/pty/file；没有对象才是隐式控制台

**第 499 行**

> 隐式控制台：klog_write 是写物理 UART 的唯一入口(整段持锁)

</details>

**位置**：`kernel/syscall/fs/file_io.c:434`

<a id="mod-kernel-syscall-fs-file_ops.c"></a>

## kernel/syscall/fs/file_ops.c — openat / close / dup3 / fcntl / pipe2

`kernel/syscall/fs/file_ops.c` · 10 个函数 · 10 个有说明 · 489 行

由 kernel/syscall/syscall.c 拆出。

<a id="fn-kernel-syscall-fs-file_ops.c-28"></a>

### `trace_heavy_task`

```c
static bool trace_heavy_task(task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断是否为用户进程且 heap_end 超过阈值，用于决定是否输出详尽日志。

**位置**：`kernel/syscall/fs/file_ops.c:28`

<a id="fn-kernel-syscall-fs-file_ops.c-51"></a>

### `open_flags_from_user`

```c
static int open_flags_from_user(int flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 aarch64 上去掉并重映射目录相关标志位，其他架构原样返回。

**位置**：`kernel/syscall/fs/file_ops.c:51`

<a id="fn-kernel-syscall-fs-file_ops.c-63"></a>

### `open_flags_to_user`

```c
static int open_flags_to_user(int flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 aarch64 上把内部目录标志位还原为用户位，其他架构原样返回。

**位置**：`kernel/syscall/fs/file_ops.c:63`

<a id="fn-kernel-syscall-fs-file_ops.c-75"></a>

### `openat_handler`

```c
void openat_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析路径并跟随符号链接、按 umask 修正 mode，特判 ptmx/tty/pts，否则 vfs_open 后分配 fd 并记录 CLOEXEC。

<details>
<summary>函数内备注（3 条）</summary>

**第 85 行**

> O_CREAT 的 mode 要按进程 umask 去掉权限位（Linux 语义）。
> O_CREAT 在三架构都是 0100。以前直接把 mode 透传，导致
> umask()/umask01 与 fstat02 都不对。

**第 107 行**

> ── PTY: /dev/ptmx 和 /dev/pts/N(不需要 fd pool 预分配)──

**第 156 行**

> open(O_CLOEXEC)：三架构的 O_CLOEXEC 都是 0x80000。
> 以前这里完全没记这位, 而 execve 现在按位图关闭 CLOEXEC fd —— 不记
> 就等于 O_CLOEXEC 被忽略, fd 会泄漏给 exec 后的新程序。

</details>

**位置**：`kernel/syscall/fs/file_ops.c:75`

<a id="fn-kernel-syscall-fs-file_ops.c-170"></a>

### `close_handler`

```c
void close_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

原子摘取 fd 表项（0-2 直接成功），通知 epoll 后关闭对象并归还池槽。

<details>
<summary>函数内备注（2 条）</summary>

**第 177 行**

> 一次原子摘取同时解决三件事：fd 0/1/2 默认 UART(fd_table 是 -1,
> close 直接成功), 把表项置回 -1, 以及保证同一个 fd 只会有一个
> 线程拿到 idx(否则两个线程都关同一个槽, 第二次归还时槽可能已经被
> 别的 open 拿走)。详见 task_take_fd 的注释。

**第 185 行**

> fd 0/1/2 的 fd_table 初值就是 -1（隐式控制台），close 应当成功；
> fd &gt;= 3 从未打开过/已经关过一次 → Linux 返回 EBADF。
> LTP close02 就是测重复 close。

</details>

**位置**：`kernel/syscall/fs/file_ops.c:170`

<a id="fn-kernel-syscall-fs-file_ops.c-201"></a>

### `dup_to_fd`

```c
static int dup_to_fd(task_t *current, int oldfd, int newfd, int flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验新旧 fd，先关闭目标槽，再复制源对象或为 UART 建空对象并增引用，按 flags 设置 CLOEXEC。

<details>
<summary>函数内备注（1 条）</summary>

**第 234 行**

> vfs_file 走 vfs_ref；ION/dma-buf 走各自 fd_ops-&gt;dup

</details>

**位置**：`kernel/syscall/fs/file_ops.c:201`

<a id="fn-kernel-syscall-fs-file_ops.c-248"></a>

### `dup_handler`

```c
void dup_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验源 fd 后从 3 起找空位调用 dup_to_fd，找不到返回 EMFILE。

**位置**：`kernel/syscall/fs/file_ops.c:248`

<a id="fn-kernel-syscall-fs-file_ops.c-272"></a>

### `dup3_handler`

```c
void dup3_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅允许 O_CLOEXEC 标志且拒绝新旧 fd 相同，然后调用 dup_to_fd 并写回结果。

<details>
<summary>函数内备注（1 条）</summary>

**第 278 行**

> dup3 只认 O_CLOEXEC；其它位（包括 -1）一律 EINVAL。LTP dup3_02

</details>

**位置**：`kernel/syscall/fs/file_ops.c:272`

<a id="fn-kernel-syscall-fs-file_ops.c-292"></a>

### `fcntl_handler`

```c
void fcntl_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 cmd 处理 GET/SETFD、GET/SETFL 与 F_DUPFD(_CLOEXEC)，未支持命令返回 0。

<details>
<summary>函数内备注（4 条）</summary>

**第 302 行**

> fd 0-2 默认是 UART, 但也可能已被 dup2() 重定向到 PTY/pipe/socket。

**第 319 行**

> F_SETFL 只能改这些位

**第 353 行**

> F_DUPFD / F_DUPFD_CLOEXEC: duplicate fd, allocate &gt;= arg

**第 362 行**

> dup UART fd: create a real pool object so dup2(saved, 1) works.

</details>

**位置**：`kernel/syscall/fs/file_ops.c:292`

<a id="fn-kernel-syscall-fs-file_ops.c-428"></a>

### `pipe2_handler`

```c
void pipe2_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验标志后分配管道，把非阻塞位落到 fd 对象、记录 CLOEXEC，再把 fd 号拷回用户，失败时清理。

<details>
<summary>函数内备注（3 条）</summary>

**第 437 行**

> pipe2 认 O_CLOEXEC(0x80000)、O_NONBLOCK(0x800) 和 O_DIRECT。
> O_DIRECT 是 Linux 3.4 起 pipe2 支持的包模式标志(LTP pipe2_01 覆盖)。
> 注意 O_DIRECT 的数值是**架构相关**的：
> x86_64           0x4000
> aarch64/riscv64  0x10000
> 本内核不区分包边界，但 F_GETFL 必须能把它读回来。

**第 458 行**

> 把 flags 落到 fd 对象上：
> - O_NONBLOCK 必须进 obj-&gt;flags / vfs_file-&gt;flags，pipe_read/write
> 才会走非阻塞路径，fcntl(F_GETFL) 也要能看到它(LTP pipe2_01)
> - O_CLOEXEC 记进 fd 位图，execve 时会关闭(LTP pipe2_02)

**第 475 行**

> 用户指针必须经 copy_to_user_bytes（CLAUDE.md 用户指针规则）

</details>

**位置**：`kernel/syscall/fs/file_ops.c:428`

<a id="mod-kernel-syscall-fs-file_stat.c"></a>

## kernel/syscall/fs/file_stat.c — fstat / newfstatat / readlinkat / faccessat / getdents64

`kernel/syscall/fs/file_stat.c` · 8 个函数 · 8 个有说明 · 293 行

由 kernel/syscall/syscall.c 拆出。

<a id="fn-kernel-syscall-fs-file_stat.c-24"></a>

### `trace_heavy_task`

```c
static bool trace_heavy_task(task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断是否为用户进程且 heap_end 超过阈值，用于决定是否输出详尽日志。

**位置**：`kernel/syscall/fs/file_stat.c:24`

<a id="fn-kernel-syscall-fs-file_stat.c-33"></a>

### `stat_copy_out`

```c
static void stat_copy_out(const struct kernel_stat *kst, struct kernel_stat *ust, uint64_t regs[6])
```

填好内核态 stat 后再一次性回写用户。绝不能把用户指针交给
vfs_stat_file/ext4_stat_file 直接写：用户传个未映射地址就是内核态 #PF，
异常处理会停机（和 waitid/ioctl 那几处是同一类问题）。

**位置**：`kernel/syscall/fs/file_stat.c:33`

<a id="fn-kernel-syscall-fs-file_stat.c-41"></a>

### `fstat_handler`

```c
void fstat_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

标准 fd 填字符设备 stat；其他 fd 调用 vfs_stat_file 后拷出，无对象返回 EBADF。

<details>
<summary>函数内备注（2 条）</summary>

**第 52 行**

> (int)：kind 是无符号枚举, 两臂不统一时 ?: 的结果会被推成无符号,
> -1 这个&quot;没有&quot;哨兵于是触发 -Wsign-compare。

**第 62 行（行尾）**

> character device

</details>

**位置**：`kernel/syscall/fs/file_stat.c:41`

<a id="fn-kernel-syscall-fs-file_stat.c-79"></a>

### `newfstatat_handler`

```c
void newfstatat_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空路径按 dirfd 取 stat，否则解析路径并按标志跟随符号链接，stat 失败时再特判 ptmx 与 pts。

<details>
<summary>函数内备注（2 条）</summary>

**第 118 行**

> AT_SYMLINK_NOFOLLOW = 0x100

**第 154 行**

> vfs_stat_path 已按挂载点分发（ext4/pseudofs/FAT）；不能再回退到 ext4，
> 否则 FAT 路径会被错误地拿去查 ext4 根

</details>

**位置**：`kernel/syscall/fs/file_stat.c:79`

<a id="fn-kernel-syscall-fs-file_stat.c-159"></a>

### `proc_self_fd_readlink`

```c
static int proc_self_fd_readlink(task_t *current, int fdnum, char *buf, size_t bufsz)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由 fd 号取对象并调用 vfs_proc_fd_target 生成链接目标内容。

**位置**：`kernel/syscall/fs/file_stat.c:159`

<a id="fn-kernel-syscall-fs-file_stat.c-168"></a>

### `readlinkat_handler`

```c
void readlinkat_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析路径并特判 /proc/self/fd/N，把链接目标读入内核缓冲后一次性拷回用户。

<details>
<summary>函数内备注（5 条）</summary>

**第 172 行（行尾）**

> 用户指针

**第 179 行**

> 必须接返回值：resolve_path_at() 在 dirfd 非法时返回 -EBADF 且根本不写
> abspath**, 忽略它就等于拿未初始化的栈去 vfs_find_mount()。
> (本文件其它几处都写了 `int rpa = ...`, 只有这里漏了。)

**第 194 行**

> 链目标先读进内核缓冲, 最后一次性拷回用户态。

> 原先直接把 lbuf(用户指针)交给 vfs_readlink → pseudo_readlink /
> ext4_readlink, 由它们 memcpy 进去。用户传个非法地址就是内核态写坏
> 地址 → 内核缺页 → handle_exception 的 platform_shutdown() → 整机挂死。
> 这是 CLAUDE.md 里那条规则的又一个漏网处。

**第 211 行**

> /proc/self/fd/N — resolve from task fd table

**第 230 行**

> copy_to_user_bytes() 内部用 user_range_ok() 校验；地址非法时返回负值
> 而不是让内核去踩它。返回 EFAULT 是 readlink(2) 的正确语义。

</details>

**位置**：`kernel/syscall/fs/file_stat.c:168`

<a id="fn-kernel-syscall-fs-file_stat.c-241"></a>

### `faccessat_handler`

```c
void faccessat_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析并跟随路径后仅用 vfs_stat_path 判断存在，成功返回 0，否则返回 ENOENT。

<details>
<summary>函数内备注（3 条）</summary>

**第 245 行（行尾）**

> mode

**第 246 行（行尾）**

> flags

**第 267 行**

> 不再回退到 ext4_inode_exist：FAT 路径必须走上面的 VFS 分发

</details>

**位置**：`kernel/syscall/fs/file_stat.c:241`

<a id="fn-kernel-syscall-fs-file_stat.c-271"></a>

### `getdents64_handler`

```c
void getdents64_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验用户缓冲与 fd，有 vfs 文件则调用 vfs_getdents 读取目录项，否则返回 ENOTDIR。

**位置**：`kernel/syscall/fs/file_stat.c:271`

<a id="mod-kernel-syscall-fs-ioctl.c"></a>

## kernel/syscall/fs/ioctl.c — ioctl 系统调用

`kernel/syscall/fs/ioctl.c` · 2 个函数 · 2 个有说明 · 159 行

由 kernel/syscall/syscall.c 拆出。优先派发给 pseudofs 设备节点；
否则按 TTY 终端 ioctl 处理(TCGETS / TIOCGWINSZ / TIOCGPGRP 等)。

<a id="fn-kernel-syscall-fs-ioctl.c-21"></a>

### `trace_heavy_task`

```c
static bool trace_heavy_task(task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断是否为用户进程且 heap_end 超过阈值，用于决定是否输出详尽日志。

**位置**：`kernel/syscall/fs/ioctl.c:21`

<a id="fn-kernel-syscall-fs-ioctl.c-26"></a>

### `ioctl_handler`

```c
void ioctl_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

处理 FIONBIO 与 termios、winsize、pgrp 请求，设备 fd 转 vfs_ioctl，dma-buf 走专用接口，其余 ENOTTY。

<details>
<summary>函数内备注（8 条）</summary>

**第 29 行**

> 注意： 必须截成 32 位。Linux 的 `SYSCALL_DEFINE3(ioctl, ..., unsigned int,
> cmd, ...)` 就是 32 位, 这里照做。

> 不截会怎样：musl 的声明是 `int ioctl(int, int, ...)`(glibc 是
> `unsigned long`), 于是 `0xc0406400` 这种最高位为 1 的号在用户态被当成
> 负数, 符号扩展到 `0xffffffffc0406400` 才进内核。内核拿它去和 32 位
> 的 _IOC 常量比对, 永远不等 —— 症状是&quot;所有带 &#x27;d&#x27;/&#x27;T&#x27; 这类高位 magic 的
> ioctl 全部走进 default 分支&quot;, 而 glibc 编的程序完全正常。
> 实测：drmtest(musl)报 `未实现的 DRM ioctl req=0xffffffffc0406400`。

> 下面那个 `(uint32_t)req` 的强转在 pseudo_ioctl 里本来就有 —— 说明这个
> 坑以前被踩过, 只是当时只补了一条路径。

**第 46 行**

> 查找 fd 对象(任何 fd, 包括 0/1/2)

**第 65 行（行尾）**

> O_NONBLOCK

**第 74 行**

> VFS-backed fd: devices handle their own ioctl, regular files return ENOSYS.

**第 81 行（行尾）**

> ENOSYS

**第 87 行**

> dma-buf fd 没有 vfs_file —— 它不是一个文件, 是一个缓冲区句柄。
> DMA_BUF_IOCTL_SYNC 打在 fd 自己身上(librknnrt 实测)；参数里的用户
> 指针由 dev.c 的 dma_buf_fd_ioctl 统一走 copy_from_user。

**第 98 行**

> 非默认 UART fd 不支持全局终端 ioctl fallback。

**第 104 行**

> fd 0-2 默认 UART 或无 pool entry：全局 TTY

> 下面全部经 copy_to_user_bytes / copy_from_user_bytes 访问用户指针。
> 以前是裸解引用(如 `*(struct kernel_termios *)argp = g_termios;`),
> 用户传非法地址(例如 (void*)-1)就会让内核态写坏地址 → #PF →
> 异常处理的死循环 → 整机挂死。copy_*_bytes 内部用 user_range_ok()
> 校验, 非法指针返回 -1, 这里转成 -EFAULT。

</details>

**位置**：`kernel/syscall/fs/ioctl.c:26`

<a id="mod-kernel-syscall-fs-path.c"></a>

## kernel/syscall/fs/path.c — 路径规范化, 用户/内核字符串拷贝, stat 填充

`kernel/syscall/fs/path.c` · 11 个函数 · 11 个有说明 · 360 行

由 kernel/syscall/syscall.c 拆出。

<a id="fn-kernel-syscall-fs-path.c-24"></a>

### `user_range_ok`

```c
static bool user_range_ok(const void *ptr, uint64_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

检查指针非空、长度加法不回绕且末字节低于用户地址上限。

**位置**：`kernel/syscall/fs/path.c:24`

<a id="fn-kernel-syscall-fs-path.c-59"></a>

### `user_range_accessible`

```c
bool user_range_accessible(const void *ptr, uint64_t len)
```

user_range_accessible - 用户地址能否安全解引用：范围合法 且每一页都映射着

user_range_ok() 只管前半段。它挡得住 (void*)-1(回绕),
挡不住"范围合法, 但已经 munmap"的地址 —— 那种地址会让后面 copy_*_bytes
里的裸 memcpy 踩空 → 内核态 #PF → platform_panic → 整机停机。

LTP writev02 就是这么打的：它 mmap 一段让内核能写, 然后 munmap,
再把那个地址放进 iovec, 期望内核返回 EFAULT 而不是崩。
实测现场(修复前)：
CR2=0x50002000  Error bits: P=0 W=0 U=0   ← 内核态读不存在的页
#0 copy_from_user_bytes  #1 write_handler  #2 writev_handler

代价：每页一次多级页表遍历。选择"查得到才算合法"而不是 Linux 那套异常表
fixup —— 后者要在三个架构各写一份带 fixup 表的汇编, 改动面大得多。

已知局限：不解决 SMP 下的 TOCTOU —— 查完到这里 memcpy 之间, 别的核
仍可能把这页拆掉。要彻底解决只能上异常表。

<details>
<summary>函数内备注（1 条）</summary>

**第 68 行（行尾）**

> 非用户进程上下文：维持旧行为, 只做范围检查

</details>

**位置**：`kernel/syscall/fs/path.c:59`

<a id="fn-kernel-syscall-fs-path.c-84"></a>

### `resolve_path`

```c
void resolve_path(const char *cwd, const char *path, char *out, int outlen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拼接 cwd 与路径后按段规范化，跳过单点并回退双点，再组装成绝对路径。

<details>
<summary>函数内备注（1 条）</summary>

**第 116 行**

> 规范化：处理 //, ., ..

</details>

**位置**：`kernel/syscall/fs/path.c:84`

<a id="fn-kernel-syscall-fs-path.c-163"></a>

### `resolve_path_at`

```c
int resolve_path_at(task_t *task, int dirfd, const char *pathname, char *abspath, int abspath_len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

绝对路径或 AT_FDCWD 用 cwd 解析，否则取 dirfd 对象并校验是目录后解析相对路径。

<details>
<summary>函数内备注（1 条）</summary>

**第 181 行**

> dirfd 指向普通文件是 ENOTDIR，不是 EBADF（LTP openat01/faccessat02）

</details>

**位置**：`kernel/syscall/fs/path.c:163`

<a id="fn-kernel-syscall-fs-path.c-191"></a>

### `path_is_pseudo`

```c
static bool path_is_pseudo(const char *path)
```

/dev、/proc、/sys 交给 pseudofs；不要在 open 时把 /proc/self/fd/N
之类的“伪链接”解析成目标路径（那会改变现有语义）。

**位置**：`kernel/syscall/fs/path.c:191`

<a id="fn-kernel-syscall-fs-path.c-202"></a>

### `follow_symlinks`

```c
void follow_symlinks(char *out, size_t outsz)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

最多八次用 vfs_readlink 展开链接目标（绝对或相对父目录），伪文件系统停止，结果回写。

<details>
<summary>函数内备注（1 条）</summary>

**第 217 行**

> 按挂载点分发：ext4 走 ext4_readlink，FAT 返回 EINVAL 即停止。
> 不能直接调 ext4_readlink，否则 /boot 下的路径会去查 ext4 根。

</details>

**位置**：`kernel/syscall/fs/path.c:202`

<a id="fn-kernel-syscall-fs-path.c-254"></a>

### `copy_string_from_user`

```c
int copy_string_from_user(const char *ustr, char *kbuf, int maxlen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐字节拷贝并在跨页时检查映射，遇 NUL 提前返回长度，检查失败或超长返回 -1。

<details>
<summary>函数内备注（1 条）</summary>

**第 259 行**

> 映射检查逐页做, 而不是对 [ustr, ustr+maxlen) 整段预检。

> 原因：maxlen 是内核缓冲的大小(调用方给的是 sizeof(kbuf)), 通常远大于
> 字符串本身。整段预检会误杀&quot;字符串贴在页尾, 后面还有半个页没映射&quot;这种
> 完全合法的入参 —— 那是把现有能跑的东西改崩。
> 逐页检查则只在真正要读某个新页之前查那一页, 语义是&quot;读得到的才算数&quot;。

> 进入新页时 i 正好是该页的第一字节((ustr+i) % PAGE_SIZE == 0)。

</details>

**位置**：`kernel/syscall/fs/path.c:254`

<a id="fn-kernel-syscall-fs-path.c-283"></a>

### `copy_string_to_user`

```c
int copy_string_to_user(const char *kstr, char *ubuf, int maxlen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验用户范围后把内核字符串逐字节拷入并补 NUL，返回长度，失败返回 -1。

**位置**：`kernel/syscall/fs/path.c:283`

<a id="fn-kernel-syscall-fs-path.c-296"></a>

### `copy_from_user_bytes`

```c
int copy_from_user_bytes(const void *usrc, void *kdst, uint64_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先用 user_range_accessible 校验用户范围，再 memcpy 到内核，失败返回 -1。

**位置**：`kernel/syscall/fs/path.c:296`

<a id="fn-kernel-syscall-fs-path.c-304"></a>

### `copy_to_user_bytes`

```c
int copy_to_user_bytes(const void *ksrc, void *udst, uint64_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先用 user_range_accessible 校验用户范围，再 memcpy 到用户，失败返回 -1。

**位置**：`kernel/syscall/fs/path.c:304`

<a id="fn-kernel-syscall-fs-path.c-312"></a>

### `fill_stat_from_ext4`

```c
int fill_stat_from_ext4(struct kernel_stat *st, const char *path)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清零并填基础字段，用 ext4 查模式与 inode，普通文件取文件大小，符号链接用 readlink 量目标长度。

<details>
<summary>函数内备注（3 条）</summary>

**第 331 行**

> 普通文件：size 要算上 i_size_hi, 交给 lwext4 的 ext4_fsize

**第 333 行（行尾）**

> O_RDONLY

**第 340 行**

> 符号链接：size = 目标路径的字节数, 正好是 readlink 会写出的长度。

> 原先是漏掉的(只处理了 S_IFREG), 于是符号链接的 st_size 一直是
> memset 之后的 0 —— 直接症状是 `ls -l` 的 size 列对符号链接显示 0
> (应为目标串长度), 另外 coreutils 会拿 st_size 当 readlink 缓冲区
> 的初值, 为 0 时会先按 1 字节读, 再扩容重试。

> 这里不复用上面 ext4_raw_inode_fill 拿到的 raw inode：它给的是
> 未转序的磁盘原始结构(endian 要自己 to_le32), 直接读字段会踩坑。
> 走 ext4_readlink 量一次是公开 API, 语义明确, 代价是 stat 符号链接时
> 多一次 open+read —— 不在热路径上。

</details>

**位置**：`kernel/syscall/fs/path.c:312`

<a id="mod-kernel-syscall-fs-pipe.c"></a>

## kernel/syscall/fs/pipe.c — kernel pipe implementation

`kernel/syscall/fs/pipe.c` · 10 个函数 · 10 个有说明 · 330 行

Ring buffer pipe with blocking read/write via task_block/task_unblock.
Each pipe_alloc() creates one pipe_t and two fd_obj_t entries
(read end + write end).

<a id="fn-kernel-syscall-fs-pipe.c-23"></a>

### `pipe_slot_alloc`

```c
static int pipe_slot_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

线性扫描管道池，返回读写引用计数均为 0 的槽位，无空闲返回 -1。

**位置**：`kernel/syscall/fs/pipe.c:23`

<a id="fn-kernel-syscall-fs-pipe.c-32"></a>

### `pipe_get_by_idx`

```c
static pipe_t *pipe_get_by_idx(int pi)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验下标且读写引用计数非零，返回管道指针，否则返回 NULL。

**位置**：`kernel/syscall/fs/pipe.c:32`

<a id="fn-kernel-syscall-fs-pipe.c-41"></a>

### `pipe_alloc`

```c
int pipe_alloc(task_t *task, int fds_out[2])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配管道槽与两个 fd 池槽，创建读写管道文件并挂到任务 fd 表，任一步失败都回滚清理。

**位置**：`kernel/syscall/fs/pipe.c:41`

<a id="fn-kernel-syscall-fs-pipe.c-125"></a>

### `pipe_interrupts`

```c
static bool pipe_interrupts(void)
```

pipe_interrupts - 这个任务现在会被信号打断吗？

判据必须比 "pending_sigs & ~blocked_sigs" 更严：那个式子会把默认动作
就是忽略的信号也算进来(SIGCHLD 最典型)。shell 读管道时子进程退出会
挂起 SIGCHLD, 用它判就会让 read 平白返 EINTR —— Linux 不会
(prepare_signal() 对忽略类信号不置 TIF_SIGPENDING)。这里照
deliver_pending_signals() 的动作表来判, 保证"会被打断"和"内核真会投递"
是同一套判据。

为什么管道需要它：管道阻塞是真正的 task_block()(不是忙等), 不检查信号
就会在 SIGALRM 到达后重新睡下去 —— alarm() 打断阻塞 read 的语义就不成立。
LTP 每个测例都靠这个看门狗从卡死里脱身。

<details>
<summary>函数内备注（6 条）</summary>

**第 133 行**

> 取最低置位的位序号。不要用 __builtin_ctzll：
> freestanding 构建下它会生成对 libgcc 的 __ctzdi2 调用, 而
> rv64gc 没有 ctz 指令, 链接里也没有 libgcc(-nodefaultlibs)
> → riscv64 `undefined reference to __ctzdi2`(x86_64/aarch64 能
> 内联成 bsf/rbit, 所以只有 riscv64 暴露)。
> 与 vplic.c / hext_run.c 的写法保持一致。

**第 141 行（行尾）**

> 取最低置位

**第 147 行（行尾）**

> 清掉最低置位

**第 155 行（行尾）**

> 显式忽略

**第 157 行（行尾）**

> 装了 handler → 一定会打断

**第 159 行**

> SIG_DFL：默认动作是忽略的那几个不算, 其余(多数是终止)算

</details>

**位置**：`kernel/syscall/fs/pipe.c:125`

<a id="fn-kernel-syscall-fs-pipe.c-176"></a>

### `pipe_read_endpoint`

```c
int pipe_read_endpoint(int pipe_idx, void *buf, size_t count, bool nonblock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环从环形缓冲取数，无数据时按写端已关、非阻塞、有信号分别返回，否则阻塞等待，读后唤醒写者并通知 epoll。

<details>
<summary>函数内备注（5 条）</summary>

**第 191 行**

> 已有数据就先返回数据 —— 信号和数据的优先级上, Linux 也是数据优先

**第 197 行**

> O_NONBLOCK：没数据立刻返回 EAGAIN, 绝不睡。
> 放在 EINTR 判断之前：非阻塞本来就不睡, 没有&quot;被信号打断&quot;可言；
> pending 信号仍会在本次 syscall 返回边界被投递。

**第 202 行（行尾）**

> -EAGAIN

**第 204 行**

> 别带着 pending 的信号睡下去：睡下去只能等下个事件才醒,
> 而信号本身不会唤醒它(除非 itimer 那条路显式 unblock)。

**第 208 行（行尾）**

> -EINTR

</details>

**位置**：`kernel/syscall/fs/pipe.c:176`

<a id="fn-kernel-syscall-fs-pipe.c-226"></a>

### `pipe_write_endpoint`

```c
int pipe_write_endpoint(int pipe_idx, const void *buf, size_t count, bool nonblock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向环形缓冲写入，读端已关发 SIGPIPE 返回 EPIPE，满时支持部分写、非阻塞 EAGAIN 或阻塞等待。

<details>
<summary>函数内备注（3 条）</summary>

**第 234 行（行尾）**

> -EPIPE

**第 261 行**

> O_NONBLOCK：缓冲区满时立刻返回。已经写进去一部分就先返回
> 那部分 —— Linux 对管道也是&quot;部分写&quot;语义, 不是丢弃。

**第 265 行（行尾）**

> -EAGAIN

</details>

**位置**：`kernel/syscall/fs/pipe.c:226`

<a id="fn-kernel-syscall-fs-pipe.c-276"></a>

### `pipe_ref_endpoint`

```c
void pipe_ref_endpoint(int pipe_idx, bool is_write_end)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验管道后按读端或写端增加对应引用计数。

**位置**：`kernel/syscall/fs/pipe.c:276`

<a id="fn-kernel-syscall-fs-pipe.c-287"></a>

### `pipe_close_endpoint`

```c
void pipe_close_endpoint(int pipe_idx, bool is_write_end)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

减少对应端引用计数，归零时唤醒对端阻塞任务并通知 epoll HUP 或 ERR。

**位置**：`kernel/syscall/fs/pipe.c:287`

<a id="fn-kernel-syscall-fs-pipe.c-316"></a>

### `pipe_poll_readable_endpoint`

```c
bool pipe_poll_readable_endpoint(int pipe_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回管道有数据或写端已关闭。

**位置**：`kernel/syscall/fs/pipe.c:316`

<a id="fn-kernel-syscall-fs-pipe.c-324"></a>

### `pipe_poll_writable_endpoint`

```c
bool pipe_poll_writable_endpoint(int pipe_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回管道缓冲未满或读端已关闭。

**位置**：`kernel/syscall/fs/pipe.c:324`

<a id="mod-kernel-syscall-fs-process_fs.c"></a>

## kernel/syscall/fs/process_fs.c — 进程级 fd 表 / cwd（files_struct、fs_struct）

`kernel/syscall/fs/process_fs.c` · 4 个函数 · 4 个有说明 · 94 行

从 task_t 拆出来的两块进程文件系统状态，原因见
docs/task_t_refactor.md。静态池 + 引用计数：
- 任务创建时各取一份（fd 表初始全 -1，cwd 初始 "/"）；
- 当前 clone 语义仍是“fork/线程各复制一份”，所以 refcount 通常为 1，
但模型已经为 CLONE_FILES / CLONE_FS 共享留好接口；
- TCB 回收在锁外 release，最后一个引用归还池槽位。

池大小 = TASK_MAX + AVATAR_MAX_CPUS：除了 g_task_pool 的 64 个槽，
每个 CPU 的 idle 任务也需要一份。

<a id="fn-kernel-syscall-fs-process_fs.c-33"></a>

### `files_alloc`

```c
files_struct_t *files_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁找引用为零的 files 槽，清空 fd 表与 CLOEXEC 位图并置引用为 1 后返回。

**位置**：`kernel/syscall/fs/process_fs.c:33`

<a id="fn-kernel-syscall-fs-process_fs.c-54"></a>

### `files_put`

```c
void files_put(files_struct_t *files)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空指针直接返回，否则持锁把 files 引用计数减一。

**位置**：`kernel/syscall/fs/process_fs.c:54`

<a id="fn-kernel-syscall-fs-process_fs.c-65"></a>

### `fs_alloc`

```c
fs_struct_t *fs_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁找空闲 fs 槽，把 cwd 初始化为根目录并置引用为 1 后返回。

**位置**：`kernel/syscall/fs/process_fs.c:65`

<a id="fn-kernel-syscall-fs-process_fs.c-85"></a>

### `fs_put`

```c
void fs_put(fs_struct_t *fs)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空指针直接返回，否则持锁把 fs 引用计数减一。

**位置**：`kernel/syscall/fs/process_fs.c:85`

<a id="mod-kernel-syscall-fs-pty.c"></a>

## kernel/syscall/fs/pty.c — pseudo-terminal (PTY) implementation

`kernel/syscall/fs/pty.c` · 18 个函数 · 18 个有说明 · 503 行

Provides /dev/ptmx (master) and /dev/pts/N (slave) pairs.
Dropbear SSH opens ptmx, forks, child opens pts/N, exec shell.

<a id="fn-kernel-syscall-fs-pty.c-24"></a>

### `pty_notify_epoll`

```c
static void pty_notify_epoll(int pty_idx, bool notify_master, uint32_t events)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历 fd 池，对匹配该 pty 主端或从端的对象通知等待者事件。

**位置**：`kernel/syscall/fs/pty.c:24`

<a id="fn-kernel-syscall-fs-pty.c-33"></a>

### `ring_read`

```c
static uint32_t ring_read(pty_ring_t *r, void *buf, size_t count)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从环形缓冲逐字节读出至多 count 字节并推进读指针，返回读取数。

**位置**：`kernel/syscall/fs/pty.c:33`

<a id="fn-kernel-syscall-fs-pty.c-45"></a>

### `ring_write`

```c
static uint32_t ring_write(pty_ring_t *r, const void *buf, size_t count)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向环形缓冲逐字节写入直到写满或写完，返回写入数。

**位置**：`kernel/syscall/fs/pty.c:45`

<a id="fn-kernel-syscall-fs-pty.c-57"></a>

### `pty_match_pts_path`

```c
int pty_match_pts_path(const char *path)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

匹配 /dev/pts/N 形式且编号合法、已启用时返回索引，否则返回 -1。

<details>
<summary>函数内备注（1 条）</summary>

**第 61 行**

> /dev/pts/0 .. /dev/pts/7

</details>

**位置**：`kernel/syscall/fs/pty.c:57`

<a id="fn-kernel-syscall-fs-pty.c-76"></a>

### `pty_alloc_master`

```c
int pty_alloc_master(task_t *task)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

找空闲 pty 槽并初始化窗口与 termios，创建 /dev/ptmx 文件、挂 fd，任一步失败回滚。

<details>
<summary>函数内备注（5 条）</summary>

**第 86 行（行尾）**

> EMFILE

**第 100 行（行尾）**

> ECHO|ECHOE|ECHOK|ECHOCTL|ECHOKE|ICANON|ISIG|IEXTEN

**第 101 行（行尾）**

> ICRNL|IXON

**第 102 行（行尾）**

> OPOST|ONLCR

**第 103 行（行尾）**

> CS8|CREAD|HUPCL|B38400

</details>

**位置**：`kernel/syscall/fs/pty.c:76`

<a id="fn-kernel-syscall-fs-pty.c-124"></a>

### `pty_open_slave`

```c
int pty_open_slave(task_t *task, int pty_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验主端已开与锁状态，分配 fd 并创建从端文件，成功后标记从端已开并增加引用。

<details>
<summary>函数内备注（2 条）</summary>

**第 127 行（行尾）**

> ENOENT

**第 132 行（行尾）**

> EACCES

</details>

**位置**：`kernel/syscall/fs/pty.c:124`

<a id="fn-kernel-syscall-fs-pty.c-175"></a>

### `pty_master_read`

```c
int pty_master_read(int pty_idx, void *buf, size_t count, bool nonblock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 s2m 环读取，无数据时按从端未开、非阻塞、阻塞处理，读后唤醒从端并通知 epoll。

**位置**：`kernel/syscall/fs/pty.c:175`

<a id="fn-kernel-syscall-fs-pty.c-201"></a>

### `pty_master_write`

```c
int pty_master_write(int pty_idx, const void *buf, size_t count, bool nonblock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把数据写入 m2s 环并按 ICRNL 转换回车，唤醒从端；满时按从端关闭、非阻塞或阻塞处理。

**位置**：`kernel/syscall/fs/pty.c:201`

<a id="fn-kernel-syscall-fs-pty.c-242"></a>

### `pty_slave_read`

```c
int pty_slave_read(int pty_idx, void *buf, size_t count, bool nonblock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 m2s 环读取，无数据时按主端未开、非阻塞、阻塞处理，读后唤醒主端并通知 epoll。

**位置**：`kernel/syscall/fs/pty.c:242`

<a id="fn-kernel-syscall-fs-pty.c-268"></a>

### `pty_slave_write`

```c
int pty_slave_write(int pty_idx, const void *buf, size_t count, bool nonblock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 s2m 环写入并在 ONLCR 生效时把换行扩成回车换行，唤醒主端；满时按主端关闭、非阻塞或阻塞处理。

**位置**：`kernel/syscall/fs/pty.c:268`

<a id="fn-kernel-syscall-fs-pty.c-319"></a>

### `pty_close_master`

```c
void pty_close_master(int pty_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

减少主端引用，归零时标记关闭、唤醒从端、通知 HUP，并在从端已关时释放槽位。

**位置**：`kernel/syscall/fs/pty.c:319`

<a id="fn-kernel-syscall-fs-pty.c-339"></a>

### `pty_ref_master`

```c
void pty_ref_master(int pty_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验索引后增加主端引用计数。

**位置**：`kernel/syscall/fs/pty.c:339`

<a id="fn-kernel-syscall-fs-pty.c-346"></a>

### `pty_ref_slave`

```c
void pty_ref_slave(int pty_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验索引后增加从端引用计数。

**位置**：`kernel/syscall/fs/pty.c:346`

<a id="fn-kernel-syscall-fs-pty.c-353"></a>

### `pty_close_slave`

```c
void pty_close_slave(int pty_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

减少从端引用，归零时标记关闭、唤醒主端、通知 HUP，并在主端已关时释放槽位。

**位置**：`kernel/syscall/fs/pty.c:353`

<a id="fn-kernel-syscall-fs-pty.c-379"></a>

### `pty_ioctl`

```c
int pty_ioctl(int pty_idx, bool is_master, uint32_t req, void *argp)
```

注意：下面所有 argp 都是【用户指针】, 必须经 copy_to/from_user_bytes
访问。以前是裸解引用(`*(uint32_t *)argp = ...`), 用户传非法地址
(例如 ioctl(fd, TIOCGPTN, (void*)-1))就会让内核态写坏地址 → #PF →
异常处理死循环 → 整机挂死。copy_*_bytes 内部用 user_range_ok() 校验。

<details>
<summary>函数内备注（1 条）</summary>

**第 481 行（行尾）**

> ENOTTY

</details>

**位置**：`kernel/syscall/fs/pty.c:379`

<a id="fn-kernel-syscall-fs-pty.c-485"></a>

### `pty_poll_readable_master`

```c
bool pty_poll_readable_master(int pty_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回主端可读，即 s2m 有数据或从端已关闭。

**位置**：`kernel/syscall/fs/pty.c:485`

<a id="fn-kernel-syscall-fs-pty.c-492"></a>

### `pty_poll_readable_slave`

```c
bool pty_poll_readable_slave(int pty_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回从端可读，即 m2s 有数据或主端已关闭。

**位置**：`kernel/syscall/fs/pty.c:492`

<a id="fn-kernel-syscall-fs-pty.c-499"></a>

### `pty_poll_writable`

```c
bool pty_poll_writable(int pty_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略索引参数，直接返回 true。

**位置**：`kernel/syscall/fs/pty.c:499`

<a id="mod-kernel-syscall-fs-tty.c"></a>

## kernel/syscall/fs/tty.c — 终端状态 + UART 输入环形缓冲区 + signal_check_uart

`kernel/syscall/fs/tty.c` · 7 个函数 · 7 个有说明 · 134 行

由 kernel/syscall/syscall.c 拆出。

<a id="fn-kernel-syscall-fs-tty.c-24"></a>

### `uart_ringbuf_push`

```c
void uart_ringbuf_push(char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁把字符写入 UART 环形缓冲，缓冲将满时丢弃该字符。

**位置**：`kernel/syscall/fs/tty.c:24`

<a id="fn-kernel-syscall-fs-tty.c-36"></a>

### `uart_ringbuf_pop`

```c
int uart_ringbuf_pop(char *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁取一个字符并推进尾指针，缓冲空返回 0，成功返回 1。

**位置**：`kernel/syscall/fs/tty.c:36`

<a id="fn-kernel-syscall-fs-tty.c-50"></a>

### `uart_ringbuf_empty`

```c
int uart_ringbuf_empty(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁比较头尾指针，返回环形缓冲是否为空。

**位置**：`kernel/syscall/fs/tty.c:50`

<a id="fn-kernel-syscall-fs-tty.c-80"></a>

### `signal_check_uart`

```c
void signal_check_uart(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁把硬件 FIFO 抽到本地缓冲，逐个入队；收到 Ctrl-C 且启用 ISIG 时向前台进程组发 SIGINT。

<details>
<summary>函数内备注（1 条）</summary>

**第 85 行**

> 在锁内一次性把硬件 FIFO 抽干到本地缓冲, 缩短临界区并避免与
> 环形缓冲锁/信号投递产生锁嵌套。剩余字节留待下次调用处理。

> 优先用非阻塞接口：ready() 返回 true 后，另一个消费者仍可能抢走
> 最后一个字节，此时阻塞版 getc() 会在关中断持锁的状态下一直等。
> DW 的 getc_nb 在未初始化时返回 false，保留 ready()+getc() 回退。

</details>

**位置**：`kernel/syscall/fs/tty.c:80`

<a id="fn-kernel-syscall-fs-tty.c-122"></a>

### `termios_is_raw`

```c
int termios_is_raw(void)
```

tty 辅助接口：供 pseudofs/tty_read 等调用

**位置**：`kernel/syscall/fs/tty.c:122`

<a id="fn-kernel-syscall-fs-tty.c-126"></a>

### `termios_do_icrnl`

```c
int termios_do_icrnl(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 termios iflag 中 ICRNL 位是否置位。

**位置**：`kernel/syscall/fs/tty.c:126`

<a id="fn-kernel-syscall-fs-tty.c-130"></a>

### `tty_getchar_nb`

```c
int tty_getchar_nb(char *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先抽取 UART 输入，再从环形缓冲尝试取一个字符并返回是否取到。

**位置**：`kernel/syscall/fs/tty.c:130`

<a id="mod-kernel-syscall-io-epoll.c"></a>

## kernel/syscall/io/epoll.c — epoll 子系统实现

`kernel/syscall/io/epoll.c` · 12 个函数 · 12 个有说明 · 489 行

事件驱动 I/O 多路复用。核心机制：
1. epoll_ctl(ADD) 时在目标 fd 的 wq 上注册 waiter
2. fd 状态变化时(pipe write, socket recv 等)调用 fd_notify_waiters()
3. fd_notify_waiters() 将对应 epoll_item 标记为 ready 并唤醒 blocked task
4. epoll_wait 收集就绪项返回用户空间

<a id="fn-kernel-syscall-io-epoll.c-29"></a>

### `epoll_instance_alloc`

```c
static int epoll_instance_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

扫描池找未使用实例，清零并标记使用后返回索引，无空位返回 -1。

**位置**：`kernel/syscall/io/epoll.c:29`

<a id="fn-kernel-syscall-io-epoll.c-41"></a>

### `epoll_instance_free`

```c
static void epoll_instance_free(int idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

下标合法时把该 epoll 实例标记为未使用。

**位置**：`kernel/syscall/io/epoll.c:41`

<a id="fn-kernel-syscall-io-epoll.c-47"></a>

### `epoll_get`

```c
static epoll_instance_t *epoll_get(int idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验下标与使用标志，返回实例指针，否则返回 NULL。

**位置**：`kernel/syscall/io/epoll.c:47`

<a id="fn-kernel-syscall-io-epoll.c-58"></a>

### `wq_register`

```c
static int wq_register(fd_waitqueue_t *wq, epoll_instance_t *ep, int item_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

等待队列未满时追加一条记录 ep 与 item_idx 的表项并返回 0，满则返回 -1。

**位置**：`kernel/syscall/io/epoll.c:58`

<a id="fn-kernel-syscall-io-epoll.c-68"></a>

### `wq_unregister`

```c
static void wq_unregister(fd_waitqueue_t *wq, epoll_instance_t *ep, int item_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历等待队列，匹配 ep 与 item_idx 的项用末尾项覆盖并减少计数。

**位置**：`kernel/syscall/io/epoll.c:68`

<a id="fn-kernel-syscall-io-epoll.c-80"></a>

### `fd_notify_waiters`

```c
void fd_notify_waiters(int pool_idx, uint32_t events)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历该 fd 的等待者，匹配关注事件时标脏、累加就绪数并唤醒阻塞任务。

**位置**：`kernel/syscall/io/epoll.c:80`

<a id="fn-kernel-syscall-io-epoll.c-113"></a>

### `fd_close_notify`

```c
void fd_close_notify(int pool_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐个摘除该 fd 的等待项，从 epoll 实例删除对应 item 并用末尾项填补后重新注册。

**位置**：`kernel/syscall/io/epoll.c:113`

<a id="fn-kernel-syscall-io-epoll.c-149"></a>

### `fd_poll`

```c
uint32_t fd_poll(task_t *task, int fd)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

标准 fd 按 UART 缓冲返回就绪事件，其他先取对象再调用 vfs_poll 得到事件。

**位置**：`kernel/syscall/io/epoll.c:149`

<a id="fn-kernel-syscall-io-epoll.c-179"></a>

### `epoll_create1_handler`

```c
void epoll_create1_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验标志后分配实例与池槽，初始化 EPOLL 对象并分配 fd，失败逐级回滚。

**位置**：`kernel/syscall/io/epoll.c:179`

<a id="fn-kernel-syscall-io-epoll.c-227"></a>

### `epoll_ctl_handler`

```c
void epoll_ctl_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

处理 ADD/DEL/MOD，登记或摘除等待队列并维护 item 表与就绪计数，ADD/MOD 时立即轮询就绪。

**位置**：`kernel/syscall/io/epoll.c:227`

<a id="fn-kernel-syscall-io-epoll.c-384"></a>

### `epoll_pwait_handler`

```c
void epoll_pwait_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环检查信号并重扫所有 item 就绪状态，就绪事件拷出用户，否则按超时阻塞或返回。

<details>
<summary>函数内备注（1 条）</summary>

**第 418 行**

> re-scan: check all items for current readiness

</details>

**位置**：`kernel/syscall/io/epoll.c:384`

<a id="fn-kernel-syscall-io-epoll.c-467"></a>

### `epoll_destroy`

```c
void epoll_destroy(int ep_idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从各 fd 等待队列摘除本项目，唤醒阻塞任务后释放 epoll 实例。

**位置**：`kernel/syscall/io/epoll.c:467`

<a id="mod-kernel-syscall-io-poll.c"></a>

## kernel/syscall/io/poll.c — poll / ppoll 处理器

`kernel/syscall/io/poll.c` · 1 个函数 · 1 个有说明 · 79 行

使用 fd_poll() 统一接口查询 fd 就绪状态。
阻塞模式下使用 task_yield() 让出 CPU。

<a id="fn-kernel-syscall-io-poll.c-19"></a>

### `poll_handler`

```c
void poll_handler(uint64_t regs[6], uint64_t syscall_num, task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环检查信号，逐个 fd 调用 fd_poll 填写 revents，就绪或超时返回，否则让出 CPU。

**位置**：`kernel/syscall/io/poll.c:19`

<a id="mod-kernel-syscall-io-select.c"></a>

## kernel/syscall/io/select.c — select / pselect6 处理器

`kernel/syscall/io/select.c` · 1 个函数 · 1 个有说明 · 117 行

使用 fd_poll() 统一接口查询 fd 就绪状态。

<a id="fn-kernel-syscall-io-select.c-18"></a>

### `select_handler`

```c
void select_handler(uint64_t regs[6], uint64_t syscall_num, task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先拷入 fd 位图，循环逐位调用 fd_poll 生成结果位图，就绪或超时返回，期间让出 CPU。

**位置**：`kernel/syscall/io/select.c:18`

<a id="mod-kernel-syscall-mm-brk.c"></a>

## kernel/syscall/mm/brk.c — sys_brk 实现

`kernel/syscall/mm/brk.c` · 4 个函数 · 4 个有说明 · 157 行

从 kernel/syscall/syscall.c 抽出。语义同 Linux brk(2)：
addr == 0 -> 查询当前堆末尾
addr <  current_brk -> 收缩
addr >  current_brk -> 按页扩展(分配并清零物理页)
失败返回 current_brk(旧值)。

<a id="fn-kernel-syscall-mm-brk.c-29"></a>

### `shared_heap_end`

```c
static uint64_t shared_heap_end(task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取当前进程堆末，并扫描同页表的其他存活用户任务取最大值。

**位置**：`kernel/syscall/mm/brk.c:29`

<a id="fn-kernel-syscall-mm-brk.c-44"></a>

### `sync_shared_heap_end`

```c
static void sync_shared_heap_end(task_t *current, uint64_t heap_end)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把新堆末写入所有共享同一页表的存活用户任务。

**位置**：`kernel/syscall/mm/brk.c:44`

<a id="fn-kernel-syscall-mm-brk.c-55"></a>

### `sys_brk`

```c
void *sys_brk(void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

非用户进程返回 ENOMEM；查询返回当前堆末；缩小则解映射页，扩展则逐页分配映射，最后同步共享堆末。

<details>
<summary>函数内备注（8 条）</summary>

**第 60 行（行尾）**

> ENOMEM

**第 70 行**

> brk(0)：查询当前堆末尾

**第 80 行**

> 缩小或不变：释放高水位以上页后更新

**第 94 行**

> 扩展堆：映射新页

**第 103 行（行尾）**

> 返回旧地址表示失败

**第 107 行**

> Page already mapped (e.g. BSS last page overlap) — skip

**第 113 行**

> x86_64：扩展堆, 使用 g_pmm

**第 128 行（行尾）**

> 已映射：跳过

</details>

**位置**：`kernel/syscall/mm/brk.c:55`

<a id="fn-kernel-syscall-mm-brk.c-141"></a>

### `sys_sbrk`

```c
void *sys_sbrk(int64_t increment)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用静态堆指针记录旧值后加上增量，返回旧值，并未建立实际映射。

<details>
<summary>函数内备注（1 条）</summary>

**第 143 行**

> 简单实现：维护一个简单的堆指针
> TODO: 实现真正的堆管理器

</details>

**位置**：`kernel/syscall/mm/brk.c:141`

<a id="mod-kernel-syscall-mm-mmap.c"></a>

## kernel/syscall/mm/mmap.c — sys_mmap 实现(包含匿名映射与文件映射)

`kernel/syscall/mm/mmap.c` · 9 个函数 · 9 个有说明 · 649 行

从 kernel/syscall/syscall.c 迁出。文件映射通过 VFS 读取文件内容。

<a id="fn-kernel-syscall-mm-mmap.c-44"></a>

### `mmap_prot_is_none`

```c
static bool mmap_prot_is_none(int prot)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断 prot 中读、写、执行三个权限位是否全为 0。

**位置**：`kernel/syscall/mm/mmap.c:44`

<a id="fn-kernel-syscall-mm-mmap.c-49"></a>

### `shared_mmap_next`

```c
static uint64_t shared_mmap_next(task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取当前 mmap_next，并扫描同页表的其他存活用户任务取最大值。

**位置**：`kernel/syscall/mm/mmap.c:49`

<a id="fn-kernel-syscall-mm-mmap.c-64"></a>

### `sync_shared_mmap_next`

```c
static void sync_shared_mmap_next(task_t *current, uint64_t next)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把更大的 mmap_next 同步到所有共享同一页表的存活用户任务。

**位置**：`kernel/syscall/mm/mmap.c:64`

<a id="fn-kernel-syscall-mm-mmap.c-75"></a>

### `mmap_user_range_ok`

```c
static int mmap_user_range_ok(uint64_t start, uint64_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拒绝零长度、地址回绕、起始低于一页或越过用户栈底的区间。

**位置**：`kernel/syscall/mm/mmap.c:75`

<a id="fn-kernel-syscall-mm-mmap.c-88"></a>

### `sync_shared_mmap_next_to`

```c
static void sync_shared_mmap_next_to(task_t *current, uint64_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把所有共享同一页表的存活用户任务的 mmap_next 直接设为给定值。

**位置**：`kernel/syscall/mm/mmap.c:88`

<a id="fn-kernel-syscall-mm-mmap.c-99"></a>

### `sys_munmap`

```c
uint64_t sys_munmap(uint64_t addr, uint64_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验参数后解映射指定范围，若触及顶端则向下探测已映射页并收缩共享 mmap_next。

<details>
<summary>函数内备注（4 条）</summary>

**第 108 行**

> vm_unmap_user_range 的副作用是必须的, 只是返回值目前只有下面那条
> 被注释掉的调试日志在用 —— 所以标 UNUSED 而不是删掉整个调用。

**第 115 行**

> KLOG_SYSCALL(&quot;[munmap] pid=%d addr=0x%llx len=0x%llx size=0x%llx top=0x%llx next=0x%llx freed=%llu\n&quot;,
> current-&gt;id, addr, len, size, top, next, freed);

**第 121 行**

> 回退下限是本进程 mmap 区的基址, 不是 heap_end。

> 原来写的是 heap_end(~0x153000), 比 mmap 基址(PIE 0x50000000)
> 低得多 —— 把一个进程的 mmap 页全部 munmap 掉之后, mmap_next 会
> 一路缩到 heap 里面去, 下一次 mmap 就在堆区分配, 和 brk 打架。
> mmap_user_range_ok() 只挡栈顶和 0 页, 拦不住这种分配。

> 顺带这也是 fork 拷贝 mmap 区的前提：[mmap_base, mmap_next) 要
> 能覆盖该进程全部 mmap 映射, 就得保证 mmap_next 永不小于基址。

**第 140 行**

> KLOG_SYSCALL(&quot;[munmap] pid=%d shrink mmap_next: 0x%llx -&gt; 0x%llx lo=0x%llx\n&quot;,
> current-&gt;id, next, new_next, lo);

</details>

**位置**：`kernel/syscall/mm/mmap.c:99`

<a id="fn-kernel-syscall-mm-mmap.c-162"></a>

### `map_contiguous_buf`

```c
static int map_contiguous_buf(task_t *current, void *pgd, uint64_t map_addr, uint64_t map_size, uint64_t pa, uint64_t flags, const char *tag, int fd, uint64_t handle)
```

把一段物理连续的缓冲区逐页映射进用户地址空间。

ION 与 dma-buf 的句柄不同(一个是 ion handle, 一个是 dma_buf 表下标),
但 fd 的 get_phys 只交出 (pa, size)，"逐页 mmap"这一段对二者完全一样。
尤其 riscv 那个 PTE_NOFREE 位：漏掉一处就会出现「能读, 但页随时会被
回收」的怪象，两处各写一遍迟早漏一个。

返回 0 表示映射完成；否则返回要原样交给用户态的负 errno。
注意 -1 与 MMAP_FAILED 是同一个值(syscall_internal.h:179),
所以调用方 `return (uint64_t)(int64_t)rc;` 对两种情况都对。

<details>
<summary>函数内备注（1 条）</summary>

**第 176 行**

> 标成&quot;设备缓冲&quot;：这一页归 dma_buf / ION 的句柄所有, 不归这个映射。

> 不标的话 munmap 会把它释放回 PMM, 而句柄仍然指着它 —— 随后
> close(fd) 再释放一次(双重释放), 中间那段时间这些页随时会被
> 分配给别的用途：对 NPU 缓冲区来说就是&quot;设备往别人的内存里写&quot;。
> 实测症状是满屏 `PMM: partial free detected: ... actually_freed=0`。

> 这里不能用 NOFREE 顶替：NOFREE 的语义是&quot;最后一个映射退出
> 就释放&quot;, 而设备缓冲归句柄所有 —— 只要句柄还开着, 任何映射退出
> 都不该释放。详见 include/arch/aarch64/mmu.h 里 PTE_KEEP 的说明。

</details>

**位置**：`kernel/syscall/mm/mmap.c:162`

<a id="fn-kernel-syscall-mm-mmap.c-212"></a>

### `sys_mprotect`

```c
uint64_t sys_mprotect(uint64_t addr, uint64_t len, int prot)
```

sys_mprotect - 够用的页保护实现

本内核没有完整的 VMA / PTE 权限模型，但两个真实用例必须支持：
1. musl 的 pthread 栈：mmap(PROT_NONE) 保留地址区间，然后
mprotect(栈区, PROT_READ|PROT_WRITE) —— 必须把中间那段补成可用
的零页，否则任何 pthread_create 都会在第一次写栈时 SIGSEGV。
2. 以 PROT_NONE 当"坏地址"的 EFAULT 检查（LTP tst_get_bad_addr）：
mmap 时就不建 PTE，mprotect 保持 PROT_NONE 时继续无映射。

语义边界：
- prot == PROT_NONE：解除映射（之后用户访问 fault，内核 copy_* 返回 EFAULT）
- prot 非 NONE：缺页补零页；已映射的页保持原样（不做真正的只读/可执行
细分，内核目前也没有权限位可改）
- 地址范围必须落在 mmap 区（沿用 mmap_user_range_ok 的检查）

**位置**：`kernel/syscall/mm/mmap.c:212`

<a id="fn-kernel-syscall-mm-mmap.c-250"></a>

### `sys_mmap`

```c
uint64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

处理文件映射，校验地址区间与偏移后经设备 get_phys 或 rtctl、rknpu 令牌查物理页并建立映射，失败返回错误码。

<details>
<summary>函数内备注（15 条）</summary>

**第 252 行**

> 保护属性本身尚未实现；prot 目前只用于调试日志。

**第 260 行**

> ── 文件映射(ld.so 用于映射 .so 段)─────────────────────

**第 291 行**

> PROT_NONE：保留 VA 区间，不建映射（见文件头说明）

**第 326 行**

> DRM 渲染节点的假偏移：RKNPU_MEM_MAP 回一个不透明的 offset,
> 用户态拿它 mmap(fd, ..., offset)。这是 DRM GEM 的老约定
> (offset 不是文件偏移, 是驱动自定义的令牌)。

> 只有 offset 非 0 才走这里 —— offset 0 落回下面&quot;不是普通文件&quot;的
> 拒绝路径, 语义不变。

**第 336 行**

> /dev/rtctl 的共享页：mmap(..., fd, worker_id &lt;&lt; PAGE_SHIFT)。
> 页面归 worker 槽位长期持有（PTE_KEEP），worker destroy 不清
> 物理页，只把 magic/state 清掉；旧映射不会指向已归还 PMM 的页。

**第 345 行**

> offset 是页对齐的令牌：worker_id &lt;&lt; PAGE_SHIFT。
> musl 的 mmap()  wrapper 会先检查 offset 页对齐，非对齐值
> 连 syscall 都到不了，所以不能直接用 worker id。

**第 418 行**

> 先 ref 再映射：ref 失败(共享页表满)时, 这次映射必须
> 在页表里留下零个 NOFREE 痕迹, 否则 unmap 找不到账本。

**第 438 行**

> 从文件读取对应区间的数据, 剩余已由 memset 清零

**第 448 行**

> 新页在映射前已经 ref 过；MAP_FIXED 复用旧页时才在这里
> 补一次。ref 失败时不要改 PTE —— 旧映射保持原样。

**第 473 行**

> KLOG_SYSCALL(&quot;[mmap] file path=%s req=0x%llx 0x%llx-0x%llx prot=0x%x flags=0x%x fd=%d off=0x%llx\n&quot;,
> fobj-&gt;path, addr, map_addr, map_addr + map_size,
> prot, flags, fd, offset);

**第 531 行**

> KLOG_SYSCALL(&quot;[mmap] req: addr=0x%llx len=0x%llx flags=0x%x fd=%d off=0x%llx -&gt; base=0x%llx size=0x%llx\n&quot;,
> addr, len, flags, fd, offset, map_addr, size);

**第 535 行**

> PROT_NONE 匿名映射：只占地址区间，不分配物理页/不建 PTE

**第 569 行**

> 先 ref 再映射：失败时页表里不能出现没有账本的 NOFREE 页。

**第 620 行**

> 先 ref 再映射：失败时页表里不能出现没有账本的 NOFREE 页。

**第 647 行**

> KLOG_SYSCALL(&quot;[mmap] 0x%llx - 0x%llx (len=0x%llx)\n&quot;, map_addr, map_addr + size, len);

</details>

**位置**：`kernel/syscall/mm/mmap.c:250`

<a id="mod-kernel-syscall-mm-pmap_compat.c"></a>

## kernel/syscall/mm/pmap_compat.c — 进程地址空间相关的兼容/小型 syscall 处理器

`kernel/syscall/mm/pmap_compat.c` · 4 个函数 · 4 个有说明 · 91 行

- arch_prctl_handler  (x86 only) — ARCH_SET_FS / ARCH_GET_FS
- getrandom_handler  — musl stack canary 等需要的伪随机
- x86_write_msr / x86_write_fs_base — MSR 写入辅助(x86)

<a id="fn-kernel-syscall-mm-pmap_compat.c-19"></a>

### `x86_write_msr`

```c
void x86_write_msr(uint32_t msr, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 wrmsr 指令把 64 位值拆成高低 32 位写入指定 MSR。

**位置**：`kernel/syscall/mm/pmap_compat.c:19`

<a id="fn-kernel-syscall-mm-pmap_compat.c-26"></a>

### `x86_write_fs_base`

```c
void x86_write_fs_base(uint64_t fs_base)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 x86_write_msr 把给定值写入 IA32_FS_BASE。

**位置**：`kernel/syscall/mm/pmap_compat.c:26`

<a id="fn-kernel-syscall-mm-pmap_compat.c-31"></a>

### `arch_prctl_handler`

```c
void arch_prctl_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

SET_FS 保存并写 MSR，GET_FS 把保存值写到用户指针，其他命令返回 EINVAL。

**位置**：`kernel/syscall/mm/pmap_compat.c:31`

<a id="fn-kernel-syscall-mm-pmap_compat.c-53"></a>

### `getrandom_handler`

```c
void getrandom_handler(uint64_t regs[6])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以时间播种的 LCG 分块生成随机字节拷到用户，长度为 0 直接返回 0。

<details>
<summary>函数内备注（2 条）</summary>

**第 66 行**

> LCG 伪随机, 以启动时间戳为种子。不是密码学安全的,
> 但每次调用输出不同序列, 满足 musl stack canary 等基本需求。

**第 73 行**

> 不能把用户地址直接交给内核写：分块生成到栈上再 copy_to_user_bytes。

</details>

**位置**：`kernel/syscall/mm/pmap_compat.c:53`

<a id="mod-kernel-syscall-net-ksocket.c"></a>

## kernel/syscall/net/ksocket.c — kernel socket layer over lwIP raw API

`kernel/syscall/net/ksocket.c` · 28 个函数 · 28 个有说明 · 774 行

Provides blocking socket semantics using task_block/task_unblock.
lwIP runs in NO_SYS=1 mode (no OS integration); all lwIP calls
happen from the net-poll task context via callbacks.

<a id="fn-kernel-syscall-net-ksocket.c-72"></a>

### `ksock_ref`

```c
void ksock_ref(int si)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验索引与状态后增加套接字引用计数。

**位置**：`kernel/syscall/net/ksocket.c:72`

<a id="fn-kernel-syscall-net-ksocket.c-78"></a>

### `ksock_alloc`

```c
static int ksock_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

线性扫描套接字池，返回空闲槽索引，无空闲返回 -1。

**位置**：`kernel/syscall/net/ksocket.c:78`

<a id="fn-kernel-syscall-net-ksocket.c-87"></a>

### `ksock_get`

```c
static ksock_t *ksock_get(int si)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验索引且非空闲状态，返回套接字指针，否则返回 NULL。

**位置**：`kernel/syscall/net/ksocket.c:87`

<a id="fn-kernel-syscall-net-ksocket.c-96"></a>

### `ksock_unblock`

```c
static void ksock_unblock(ksock_t *sk)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若有被阻塞任务，清空指针并调用 task_unblock 唤醒。

**位置**：`kernel/syscall/net/ksocket.c:96`

<a id="fn-kernel-syscall-net-ksocket.c-105"></a>

### `ksock_notify_epoll`

```c
static void ksock_notify_epoll(int si, uint32_t events)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历 fd 池，对匹配该套接字的文件通知 epoll 等待者事件。

**位置**：`kernel/syscall/net/ksocket.c:105`

<a id="fn-kernel-syscall-net-ksocket.c-121"></a>

### `ksock_tcp_recv_cb`

```c
static err_t ksock_tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

出错时置错误码唤醒等待者；空 pbuf 视为 EOF；否则按队列上限挂接数据、确认接收并通知 EPOLLIN。

<details>
<summary>函数内备注（1 条）</summary>

**第 131 行（行尾）**

> EIO

</details>

**位置**：`kernel/syscall/net/ksocket.c:121`

<a id="fn-kernel-syscall-net-ksocket.c-166"></a>

### `ksock_tcp_accept_cb`

```c
static err_t ksock_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

队列或槽位不足时 abort 新连接，否则初始化新套接字、设回调并入 accept 队列后唤醒。

**位置**：`kernel/syscall/net/ksocket.c:166`

<a id="fn-kernel-syscall-net-ksocket.c-207"></a>

### `ksock_tcp_connected_cb`

```c
static err_t ksock_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

成功则标记已连接并记录对端，失败置 ECONNREFUSED，随后唤醒等待者并通知 EPOLLOUT 或 EPOLLERR。

<details>
<summary>函数内备注（1 条）</summary>

**第 219 行（行尾）**

> ECONNREFUSED

</details>

**位置**：`kernel/syscall/net/ksocket.c:207`

<a id="fn-kernel-syscall-net-ksocket.c-226"></a>

### `ksock_tcp_err_cb`

```c
static void ksock_tcp_err_cb(void *arg, err_t err)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清空 pcb 指针并按错误类型映射 errno，标记 EOF 后唤醒等待者并通知 EPOLLERR 与 HUP。

<details>
<summary>函数内备注（3 条）</summary>

**第 236 行（行尾）**

> ECONNRESET

**第 239 行（行尾）**

> ECONNABORTED

**第 242 行（行尾）**

> EIO

</details>

**位置**：`kernel/syscall/net/ksocket.c:226`

<a id="fn-kernel-syscall-net-ksocket.c-249"></a>

### `ksock_tcp_sent_cb`

```c
static err_t ksock_tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

发送完成后唤醒阻塞任务并通知 EPOLLOUT，返回 ERR_OK。

**位置**：`kernel/syscall/net/ksocket.c:249`

<a id="fn-kernel-syscall-net-ksocket.c-263"></a>

### `ksock_udp_recv_cb`

```c
static void ksock_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

队列未满时记录来源地址端口，把 pbuf 链入接收队列，唤醒等待者并通知 EPOLLIN。

**位置**：`kernel/syscall/net/ksocket.c:263`

<a id="fn-kernel-syscall-net-ksocket.c-293"></a>

### `ksock_create`

```c
int ksock_create(int domain, int type, int protocol)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅支持 AF_INET，分配槽后按 SOCK_STREAM 或 SOCK_DGRAM 创建 TCP/UDP 控制块并设回调，返回索引。

<details>
<summary>函数内备注（7 条）</summary>

**第 296 行（行尾）**

> AF_INET

**第 297 行（行尾）**

> EAFNOSUPPORT

**第 301 行（行尾）**

> EMFILE

**第 308 行（行尾）**

> SOCK_STREAM

**第 313 行（行尾）**

> ENOBUFS

**第 319 行（行尾）**

> SOCK_DGRAM

**第 328 行（行尾）**

> ENOTSUP

</details>

**位置**：`kernel/syscall/net/ksocket.c:293`

<a id="fn-kernel-syscall-net-ksocket.c-335"></a>

### `ksock_bind`

```c
int ksock_bind(int si, uint32_t addr, uint16_t port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按协议调用 TCP 或 UDP bind，地址占用返回 EADDRINUSE，成功后记录本地地址端口并置 BOUND。

<details>
<summary>函数内备注（3 条）</summary>

**第 339 行（行尾）**

> EBADF

**第 352 行（行尾）**

> EADDRINUSE

**第 355 行（行尾）**

> EINVAL

</details>

**位置**：`kernel/syscall/net/ksocket.c:335`

<a id="fn-kernel-syscall-net-ksocket.c-364"></a>

### `ksock_listen`

```c
int ksock_listen(int si, int backlog)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅对 TCP 生效，已监听直接成功，规范化 backlog 后建监听控制块、设 accept 回调并置 LISTENING。

<details>
<summary>函数内备注（4 条）</summary>

**第 373 行（行尾）**

> EINVAL

**第 384 行（行尾）**

> EADDRINUSE

**第 386 行（行尾）**

> ENOBUFS

**第 387 行（行尾）**

> EINVAL

</details>

**位置**：`kernel/syscall/net/ksocket.c:364`

<a id="fn-kernel-syscall-net-ksocket.c-397"></a>

### `ksock_accept`

```c
int ksock_accept(int si, uint32_t *out_addr, uint16_t *out_port, int flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

队列空时按非阻塞返回 EAGAIN 或阻塞等待，取出新套接字、回填对端地址端口后返回其索引。

<details>
<summary>函数内备注（1 条）</summary>

**第 408 行（行尾）**

> EAGAIN

</details>

**位置**：`kernel/syscall/net/ksocket.c:397`

<a id="fn-kernel-syscall-net-ksocket.c-438"></a>

### `ksock_connect`

```c
int ksock_connect(int si, uint32_t addr, uint16_t port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

TCP 发起连接并阻塞等待回调唤醒，失败返回错误码；UDP 仅设置对端并标记已连接。

<details>
<summary>函数内备注（1 条）</summary>

**第 452 行（行尾）**

> ENETUNREACH

</details>

**位置**：`kernel/syscall/net/ksocket.c:438`

<a id="fn-kernel-syscall-net-ksocket.c-472"></a>

### `ksock_send`

```c
int ksock_send(int si, const void *buf, size_t len, int flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

TCP 在发送缓冲剩余空间内循环 tcp_write 并 output，空间不足时非阻塞 EAGAIN 或阻塞；UDP 转 sendto。

<details>
<summary>函数内备注（2 条）</summary>

**第 482 行（行尾）**

> ENOTCONN

**第 494 行（行尾）**

> EAGAIN

</details>

**位置**：`kernel/syscall/net/ksocket.c:472`

<a id="fn-kernel-syscall-net-ksocket.c-516"></a>

### `ksock_recv`

```c
int ksock_recv(int si, void *buf, size_t len, int flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

TCP 无数据时按 EOF、错误、非阻塞、阻塞分别处理，用 pbuf 拷贝数据并推进偏移，读尽释放；UDP 转 recvfrom。

<details>
<summary>函数内备注（2 条）</summary>

**第 531 行（行尾）**

> EAGAIN

**第 542 行**

> DEBUG: 显示读取的字节 (仅对1字节读取打印前32次)

</details>

**位置**：`kernel/syscall/net/ksocket.c:516`

<a id="fn-kernel-syscall-net-ksocket.c-569"></a>

### `ksock_sendto`

```c
int ksock_sendto(int si, const void *buf, size_t len, int flags, uint32_t addr, uint16_t port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

TCP 转 send，UDP 分配 pbuf 拷贝数据后 udp_sendto，成功返回长度，失败返回 -ENOBUFS。

**位置**：`kernel/syscall/net/ksocket.c:569`

<a id="fn-kernel-syscall-net-ksocket.c-592"></a>

### `ksock_recvfrom`

```c
int ksock_recvfrom(int si, void *buf, size_t len, int flags, uint32_t *out_addr, uint16_t *out_port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

TCP 转 recv，UDP 无数据时阻塞或 EAGAIN，拷出队首 pbuf 数据并出队释放，回填来源地址端口。

<details>
<summary>函数内备注（1 条）</summary>

**第 607 行（行尾）**

> EAGAIN

</details>

**位置**：`kernel/syscall/net/ksocket.c:592`

<a id="fn-kernel-syscall-net-ksocket.c-631"></a>

### `ksock_close`

```c
int ksock_close(int si)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

减少引用，归零时关闭监听子套接字、注销并关闭 tcp/udp 控制块、释放接收队列后清空结构。

<details>
<summary>函数内备注（2 条）</summary>

**第 640 行**

> close pre-allocated sockets in accept queue

**第 660 行**

> free recv queue

</details>

**位置**：`kernel/syscall/net/ksocket.c:631`

<a id="fn-kernel-syscall-net-ksocket.c-673"></a>

### `ksock_shutdown`

```c
int ksock_shutdown(int si, int how)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对已连接的 TCP 按 how 调用 tcp_shutdown 关闭接收或发送方向，始终返回 0。

**位置**：`kernel/syscall/net/ksocket.c:673`

<a id="fn-kernel-syscall-net-ksocket.c-687"></a>

### `ksock_setsockopt`

```c
int ksock_setsockopt(int si, int level, int optname, const void *optval, uint32_t optlen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅处理 TCP_NODELAY 开关 Nagle，其他选项一律返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 695 行（行尾）**

> EINVAL

</details>

**位置**：`kernel/syscall/net/ksocket.c:687`

<a id="fn-kernel-syscall-net-ksocket.c-715"></a>

### `ksock_getsockopt`

```c
int ksock_getsockopt(int si, int level, int optname, void *optval, uint32_t *optlen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

有输出缓冲时写入 last_err 的负值或 0 并把长度设为 4，返回 0。

**位置**：`kernel/syscall/net/ksocket.c:715`

<a id="fn-kernel-syscall-net-ksocket.c-730"></a>

### `ksock_getsockname`

```c
int ksock_getsockname(int si, uint32_t *addr, uint16_t *port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字后输出本地地址与端口，套接字无效返回 EBADF。

**位置**：`kernel/syscall/net/ksocket.c:730`

<a id="fn-kernel-syscall-net-ksocket.c-742"></a>

### `ksock_getpeername`

```c
int ksock_getpeername(int si, uint32_t *addr, uint16_t *port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

非已连接状态返回 ENOTCONN，否则输出对端地址与端口。

**位置**：`kernel/syscall/net/ksocket.c:742`

<a id="fn-kernel-syscall-net-ksocket.c-756"></a>

### `ksock_poll_readable`

```c
bool ksock_poll_readable(int si)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

监听态返回 accept 队列是否有连接，否则返回是否有接收数据或已到 EOF。

**位置**：`kernel/syscall/net/ksocket.c:756`

<a id="fn-kernel-syscall-net-ksocket.c-766"></a>

### `ksock_poll_writable`

```c
bool ksock_poll_writable(int si)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

TCP 有控制块时返回发送缓冲是否大于 0，其他协议总是可写。

**位置**：`kernel/syscall/net/ksocket.c:766`

<a id="mod-kernel-syscall-net-sock_syscall.c"></a>

## kernel/syscall/net/sock_syscall.c — socket syscall handlers

`kernel/syscall/net/sock_syscall.c` · 15 个函数 · 15 个有说明 · 381 行

Translates Linux syscall ABI (sockaddr_in, etc.) into
kernel socket API (ksocket.h) calls.

<a id="fn-kernel-syscall-net-sock_syscall.c-35"></a>

### `ntohs_val`

```c
static uint16_t ntohs_val(uint16_t x)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

交换 16 位值的高低字节完成网络字节序到主机字节序转换。

**位置**：`kernel/syscall/net/sock_syscall.c:35`

<a id="fn-kernel-syscall-net-sock_syscall.c-40"></a>

### `htons_val`

```c
static uint16_t htons_val(uint16_t x)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

交换 16 位值的高低字节完成主机字节序到网络字节序转换。

**位置**：`kernel/syscall/net/sock_syscall.c:40`

<a id="fn-kernel-syscall-net-sock_syscall.c-45"></a>

### `socket_handler`

```c
void socket_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

创建套接字，分配池槽与 vfs 套接字文件、按 SOCK_NONBLOCK 记录非阻塞并分配 fd，失败逐级回收。

**位置**：`kernel/syscall/net/sock_syscall.c:45`

<a id="fn-kernel-syscall-net-sock_syscall.c-85"></a>

### `get_sock_idx`

```c
static int get_sock_idx(task_t *current, int fd)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由 fd 取对象并返回其 vfs 套接字索引，无对象返回 -1。

**位置**：`kernel/syscall/net/sock_syscall.c:85`

<a id="fn-kernel-syscall-net-sock_syscall.c-93"></a>

### `bind_handler`

```c
void bind_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引并校验地址指针，端口转主机序后调用 ksock_bind 并写回结果。

**位置**：`kernel/syscall/net/sock_syscall.c:93`

<a id="fn-kernel-syscall-net-sock_syscall.c-112"></a>

### `listen_handler`

```c
void listen_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引后调用 ksock_listen，返回错误码或 0。

**位置**：`kernel/syscall/net/sock_syscall.c:112`

<a id="fn-kernel-syscall-net-sock_syscall.c-127"></a>

### `accept_handler`

```c
void accept_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取监听索引与阻塞标志调用 ksock_accept，为新连接分配 fd 并回填对端 sockaddr 与长度。

**位置**：`kernel/syscall/net/sock_syscall.c:127`

<a id="fn-kernel-syscall-net-sock_syscall.c-194"></a>

### `connect_handler`

```c
void connect_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引并校验地址指针，端口转换后调用 ksock_connect 并写回结果。

**位置**：`kernel/syscall/net/sock_syscall.c:194`

<a id="fn-kernel-syscall-net-sock_syscall.c-213"></a>

### `sendto_handler`

```c
void sendto_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引，有目的地址则调用 sendto，否则调用 send，返回字节数或错误码。

**位置**：`kernel/syscall/net/sock_syscall.c:213`

<a id="fn-kernel-syscall-net-sock_syscall.c-236"></a>

### `recvfrom_handler`

```c
void recvfrom_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引，有来源地址时调用 recvfrom 并回填 sockaddr 与长度，否则调用 recv。

**位置**：`kernel/syscall/net/sock_syscall.c:236`

<a id="fn-kernel-syscall-net-sock_syscall.c-270"></a>

### `setsockopt_handler`

```c
void setsockopt_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引后调用 ksock_setsockopt，返回 0 或错误码。

**位置**：`kernel/syscall/net/sock_syscall.c:270`

<a id="fn-kernel-syscall-net-sock_syscall.c-288"></a>

### `getsockopt_handler`

```c
void getsockopt_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引后调用 ksock_getsockopt，返回 0 或错误码。

**位置**：`kernel/syscall/net/sock_syscall.c:288`

<a id="fn-kernel-syscall-net-sock_syscall.c-306"></a>

### `getsockname_handler`

```c
void getsockname_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引与本地名，回填 sockaddr_in 和地址长度后返回 0。

**位置**：`kernel/syscall/net/sock_syscall.c:306`

<a id="fn-kernel-syscall-net-sock_syscall.c-337"></a>

### `getpeername_handler`

```c
void getpeername_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引与对端名，回填 sockaddr_in 和地址长度后返回 0。

**位置**：`kernel/syscall/net/sock_syscall.c:337`

<a id="fn-kernel-syscall-net-sock_syscall.c-368"></a>

### `shutdown_handler`

```c
void shutdown_handler(uint64_t regs[6], task_t *current)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取套接字索引后调用 ksock_shutdown 关闭连接方向，返回 0 或错误码。

**位置**：`kernel/syscall/net/sock_syscall.c:368`

<a id="mod-kernel-syscall-syscall.c"></a>

## kernel/syscall/syscall.c

`kernel/syscall/syscall.c` · 13 个函数 · 13 个有说明 · 1667 行

<a id="fn-kernel-syscall-syscall.c-57"></a>

### `test_completion_get`

```c
static completion_t *test_completion_get(uint64_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

id 越界时返回 NULL，否则在自旋锁保护下首次调用时初始化完成量数组，返回对应槽位指针。

**位置**：`kernel/syscall/syscall.c:57`

<a id="fn-kernel-syscall-syscall.c-83"></a>

### `notify_parent_wait`

```c
static void notify_parent_wait(task_t *child)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

扫描任务池找匹配 wait_pid 且正在等待的父任务，清等待标志并解除阻塞；否则仅打印父任务状态日志。

**位置**：`kernel/syscall/syscall.c:83`

<a id="fn-kernel-syscall-syscall.c-117"></a>

### `notify_parent_wait_from_task`

```c
void notify_parent_wait_from_task(task_t *child)
```

Called from task.c's task_exit() to notify waiting parent

**位置**：`kernel/syscall/syscall.c:117`

<a id="fn-kernel-syscall-syscall.c-150"></a>

### `x86_translate_syscall`

```c
static void x86_translate_syscall(uint64_t *nr, uint64_t regs[9])
```

Linux x86_64 → 内部统一系统调用号翻译

busybox-x86_64 使用 Linux x86_64 syscall 号, 而下面的 switch 分支
使用 AArch64/RISC-V 的通用 Linux syscall 号(两者已在 AARCH64/RISC-V
上共享)。这个翻译层把 x86_64 号映射到对应分支, 同时对参数布局不同
的 syscall(open/stat/access/readlink 等)原地调整 regs[]。

<details>
<summary>函数内备注（144 条）</summary>

**第 153 行**

> ── 基础 I/O ──

**第 156 行（行尾）**

> read

**第 159 行（行尾）**

> write

**第 160 行（行尾）**

> open(path,flags,mode) → openat(AT_FDCWD,path,flags,mode)

**第 169 行（行尾）**

> close

**第 170 行（行尾）**

> stat(path,buf) → newfstatat(AT_FDCWD,path,buf,0)

**第 179 行（行尾）**

> fstat

**第 180 行（行尾）**

> lstat(path,buf) → newfstatat(AT_FDCWD,path,buf,0)

**第 189 行（行尾）**

> poll

**第 192 行（行尾）**

> lseek

**第 195 行（行尾）**

> mmap

**第 198 行（行尾）**

> mprotect

**第 201 行（行尾）**

> munmap

**第 204 行（行尾）**

> brk

**第 207 行（行尾）**

> rt_sigaction

**第 210 行（行尾）**

> rt_sigprocmask

**第 213 行（行尾）**

> rt_sigreturn

**第 216 行（行尾）**

> ioctl

**第 219 行（行尾）**

> pread64

**第 222 行（行尾）**

> pwrite64

**第 225 行（行尾）**

> readv

**第 228 行（行尾）**

> writev

**第 229 行（行尾）**

> access(path,mode) → faccessat(AT_FDCWD,path,mode,0)

**第 237 行**

> pipe(fds) → pipe2(fds, 0)。第二个寄存器是用户残留值，必须显式清零：
> pipe2 现在会校验 flags，不清零会被当成非法 flags 返回 EINVAL。

**第 244 行（行尾）**

> select(nfds,r,w,e,timeval*)

**第 247 行（行尾）**

> sched_yield

**第 250 行（行尾）**

> msync → stub 0

**第 251 行（行尾）**

> dup(oldfd): x86_64 → 内部 dup 实现（与 aarch64/riscv 的 23 同）

**第 254 行（行尾）**

> dup2(oldfd,newfd) → dup3(oldfd,newfd,0)

**第 260 行（行尾）**

> nanosleep

**第 261 行**

> getitimer(36) / setitimer(38)：x86_64 原来没有这两条翻译, 于是落到
> default: 返回 ENOSYS —— 主 switch 里那个 case 103 桩对 x86_64 根本
> 不可达。转到内部编号(= aarch64/riscv64 的号)去交给同一份实现。
> alarm(37) 不映射：musl 的 alarm() 本身就是走 setitimer 实现的。

**第 269 行（行尾）**

> getitimer → 内部 __NR_getitimer

**第 272 行（行尾）**

> setitimer → 内部 __NR_setitimer

**第 275 行（行尾）**

> getpid

**第 278 行（行尾）**

> sendfile / sendfile64 (x86_64)

**第 281 行（行尾）**

> sendfile64 (x86_64 compat)

**第 284 行（行尾）**

> socket

**第 287 行（行尾）**

> connect

**第 290 行（行尾）**

> accept

**第 293 行（行尾）**

> sendto

**第 296 行（行尾）**

> recvfrom

**第 299 行（行尾）**

> sendmsg

**第 302 行（行尾）**

> recvmsg

**第 305 行（行尾）**

> shutdown

**第 308 行（行尾）**

> bind

**第 311 行（行尾）**

> listen

**第 314 行（行尾）**

> getsockname

**第 317 行（行尾）**

> getpeername

**第 320 行（行尾）**

> socketpair

**第 323 行（行尾）**

> setsockopt

**第 326 行（行尾）**

> getsockopt

**第 327 行（行尾）**

> clone: x86_64 ABI 顺序 flags,stack,ptid,ctid,tls
> 内部 ABI 顺序 flags,stack,ptid,tls,ctid
> 需要交换 arg3↔arg4

**第 338 行（行尾）**

> fork(): musl 直接 syscall, 不传参数 → rdi/rsi/... 是垃圾。
> 必须显式构造 flags=SIGCHLD (17), 其余清零。

**第 341 行（行尾）**

> SIGCHLD

**第 349 行（行尾）**

> vfork(): 同理, 构造 CLONE_VM|CLONE_VFORK|SIGCHLD。
> 但当前内核把 CLONE_VM 当线程走, 会出错。
> 为兼容 busybox sh 的 fork+exec, 这里退化为普通 fork。

**第 353 行（行尾）**

> SIGCHLD: 走 fork 路径(拷贝地址空间)

**第 363 行（行尾）**

> execve

**第 366 行（行尾）**

> exit

**第 369 行（行尾）**

> wait4

**第 372 行（行尾）**

> kill

**第 375 行（行尾）**

> uname

**第 378 行（行尾）**

> fcntl

**第 381 行（行尾）**

> fsync

**第 384 行（行尾）**

> fdatasync

**第 387 行（行尾）**

> ftruncate

**第 390 行（行尾）**

> getcwd

**第 393 行（行尾）**

> chdir

**第 394 行（行尾）**

> rename(old,new) → renameat(AT_FDCWD,old,AT_FDCWD,new)

**第 401 行（行尾）**

> mkdir(path,mode) → mkdirat(AT_FDCWD,path,mode)

**第 407 行（行尾）**

> rmdir(path) → unlinkat(AT_FDCWD,path,AT_REMOVEDIR)

**第 413 行（行尾）**

> creat(path,mode) → openat(AT_FDCWD,path,O_CREAT|O_WRONLY|O_TRUNC,mode)

**第 420 行（行尾）**

> link(old,new) → stub 0 (not critical)

**第 423 行（行尾）**

> unlink(path) → unlinkat(AT_FDCWD,path,0)

**第 431 行（行尾）**

> chmod → stub 0

**第 434 行（行尾）**

> fchmod → stub 0

**第 437 行（行尾）**

> chown → stub 0

**第 440 行（行尾）**

> fchown → stub 0

**第 441 行（行尾）**

> readlink(path,buf,bufsiz) → readlinkat(AT_FDCWD,path,buf,bufsiz)

**第 450 行（行尾）**

> umask

**第 453 行（行尾）**

> getrlimit

**第 456 行（行尾）**

> getrusage

**第 459 行（行尾）**

> sysinfo

**第 462 行（行尾）**

> times

**第 465 行（行尾）**

> getuid

**第 468 行（行尾）**

> getgid

**第 471 行（行尾）**

> setuid

**第 474 行（行尾）**

> setgid

**第 477 行（行尾）**

> geteuid

**第 480 行（行尾）**

> getegid

**第 483 行（行尾）**

> setpgid

**第 486 行（行尾）**

> getppid

**第 487 行（行尾）**

> getpgrp() → getpgid(0)

**第 493 行（行尾）**

> setsid

**第 496 行（行尾）**

> getgroups

**第 499 行（行尾）**

> setgroups

**第 500 行**

> x86_64 的 setresuid/getresuid/setresgid/getresgid = 117..120，
> 内部用 aarch64/riscv64 的 147..150。之前没映射 →
> musl 的 seteuid()/seteuid() 全落 ENOSYS（LTP setgroups03 在这里
> TBROK）。

**第 508 行（行尾）**

> setresuid

**第 511 行（行尾）**

> getresuid

**第 514 行（行尾）**

> setresgid

**第 517 行（行尾）**

> getresgid

**第 520 行（行尾）**

> getpgid

**第 523 行（行尾）**

> getsid

**第 526 行（行尾）**

> personality

**第 529 行（行尾）**

> rt_sigpending

**第 532 行（行尾）**

> sigaltstack → stub 0

**第 535 行（行尾）**

> prctl

**第 538 行（行尾）**

> arch_prctl

**第 541 行（行尾）**

> setrlimit

**第 544 行（行尾）**

> getrusage(again)

**第 547 行（行尾）**

> gettid

**第 548 行（行尾）**

> x86_64 __NR_tkill；内部号 130（200 本身是 BIND，不能直落）

**第 553 行（行尾）**

> futex

**第 554 行（行尾）**

> epoll_create(size) → epoll_create1(0)

**第 560 行（行尾）**

> getdents64

**第 563 行（行尾）**

> set_tid_address

**第 566 行（行尾）**

> clock_gettime

**第 569 行（行尾）**

> exit_group

**第 570 行（行尾）**

> x86_64 __NR_getcpu → 通用号 168

**第 573 行（行尾）**

> epoll_wait(epfd,events,max,timeout) → epoll_pwait(sigmask=NULL)

**第 580 行（行尾）**

> epoll_ctl

**第 583 行（行尾）**

> tgkill

**第 586 行（行尾）**

> waitid

**第 589 行（行尾）**

> openat

**第 592 行（行尾）**

> mkdirat

**第 595 行（行尾）**

> newfstatat

**第 598 行（行尾）**

> fchownat → stub 0

**第 601 行（行尾）**

> unlinkat

**第 604 行（行尾）**

> renameat

**第 607 行（行尾）**

> readlinkat

**第 610 行（行尾）**

> fchmodat → stub 0

**第 613 行（行尾）**

> faccessat

**第 616 行（行尾）**

> pselect6

**第 619 行（行尾）**

> ppoll

**第 622 行（行尾）**

> set_robust_list

**第 625 行（行尾）**

> dup3

**第 628 行（行尾）**

> pipe2

**第 631 行（行尾）**

> accept4

**第 634 行（行尾）**

> epoll_create1

**第 636 行**

> utimensat(dirfd,path,times,flags)：内部 88 是现有的 0 值桩。
> busybox mv 的跨文件系统回退会调它；不映射会在日志里刷
> &quot;Unknown syscall: 280&quot;。

**第 643 行（行尾）**

> epoll_pwait

**第 646 行（行尾）**

> prlimit64

**第 649 行（行尾）**

> getrandom

**第 652 行（行尾）**

> rseq

**第 653 行**

> 其他：保持原号(会落到 default: 返回 ENOSYS)

</details>

**位置**：`kernel/syscall/syscall.c:150`

<a id="fn-kernel-syscall-syscall.c-664"></a>

### `syscall_handler`

```c
void syscall_handler(trap_frame_t *frame)
```

Main syscall dispatcher

<details>
<summary>函数内备注（73 条）</summary>

**第 674 行**

> 翻译前的原始号, 只有下面那条 stub 日志要看 —— 所以声明也留在这个
> 块里。放外面的话, 其它架构上它就是一条 -Wunused-variable。

**第 678 行**

> 将 Linux x86_64 syscall 号翻译为下方 switch 使用的内部编号

**第 680 行**

> 0x7FFFFFFEULL 是 stub 标记：直接返回 0

**第 694 行**

> 常开记录：每条 syscall 往本 CPU 的环形缓冲写一条(入口填参数, 出口回填
> 返回值)。不碰 UART, 不加锁 —— 崩溃是事后才知道的, &quot;补录&quot;不成立,
> 所以这里不能做成开关。回看用 syscall_trace_dump() / /proc/syscalls。

**第 708 行**

> 每次 syscall 入口 poll UART：弥补关中断期间 timer 无法触发的窗口

**第 711 行**

> 开始 syscall stime 计时

**第 715 行**

> Dropbear 调试：pid&gt;=7 所有 syscall 都打

**第 716 行**

> if (current &amp;&amp; current-&gt;id &gt;= 7)

**第 719 行**

> ── 自定义系统调用(向后兼容)──────────────────────────

**第 770 行**

> ── Linux AArch64 标准系统调用 ───────────────────────────────

**第 772 行**

> --- 进程管理 ---

**第 794 行**

> waitid 与 wait4 的参数布局不同：
> wait4 (pid,  status,  options, rusage)
> waitid(idtype, id,    infop,   options, rusage)
> 以前两者共用 wait_handler, 于是 wait_handler 把 waitid 的
> regs[1](id, 一个小整数)当成 int *wstatus 去写 —— 写地址 0x4
> → #PF → 异常处理死循环 → 整机挂死。

> 按 waitid 语义要填 siginfo_t, 本内核未实现, 这里明确返回
> ENOSYS。宁可&quot;未实现&quot;, 也不要崩。

**第 867 行**

> --- 文件描述符 ---

**第 914 行**

> mkdirat(dirfd, pathname, mode)

**第 922 行**

> 用户指针先落进内核；resolve_path_at 会直接解引用

**第 941 行**

> 走 VFS 分发：/boot 下的路径属于 FAT，不能再直接调 ext4_dir_mk。
> mode 按 umask 去掉权限位(Linux 语义)。

**第 1001 行（行尾）**

> ION/dma-buf 等非 VFS fd：维持原来的“成功”语义

**第 1019 行**

> Linux ftruncate 的错误语义：
> - 负长度 / 非普通文件（目录、socket、pipe...）→ EINVAL
> - 普通文件但 fd 是 O_RDONLY → EINVAL
> - fd 无效 → EBADF（上面已处理）

**第 1038 行**

> --- 目录操作 ---

**第 1047 行**

> --- ioctl ---

**第 1062 行**

> --- epoll ---

**第 1075 行**

> --- 信号系统(实现位于 kernel/syscall/signal.c)---

**第 1092 行**

> --- 系统信息 ---

**第 1094 行**

> 先在内核里构造完整结构再一次性 copy_to_user：
> - 坏指针返回 EFAULT（以前逐个 copy_string_to_user 并忽略返回值，
> uname((void*)-1) 会&quot;成功&quot;）
> - 整个结构 memset 过，字段 NUL 之后的填充字节不会被泄漏
> （LTP uname04 专门查这个）

**第 1103 行**

> 注意：这里是**内核**结构体，不能再用 copy_string_to_user
> （它把目标当用户指针做 user_range_ok 校验，内核栈地址会被
> 拒绝，字段保持全 0 —— uname01 于是报 sysname 不是 Linux）。

**第 1164 行**

> getcpu(cpu*, node*, tcache) —— 用户态观测当前任务落在哪颗逻辑 CPU。

> musl 的 sched_getcpu() 就是这条；也是 task 基础设施回归(多核分发,
> 跨核唤醒)唯一不依赖内核日志的判据。node 恒 0(本内核无 NUMA)。

**第 1194 行**

> 用户指针必须校验后再写；x86 上未映射的 raw syscall 号可能
> 误落到这里（例如 setreuid=113），直接 ts-&gt;tv_sec 会写坏地址。

**第 1235 行（行尾）**

> TASK_INFRA_TEST

**第 1249 行**

> RUSAGE_SELF=0 / RUSAGE_CHILDREN=-1 / RUSAGE_THREAD=1

**第 1258 行**

> 用户指针必须经 copy_to_user_bytes, 不能裸解引用

**第 1259 行（行尾）**

> sizeof(struct rusage) == 144

**第 1262 行**

> 用与 /proc 统计一致的&quot;墙钟 - 内核时间&quot;算活着的 utime；
> 直接读 utime_ns 只有 task_exit 才会填。

**第 1384 行**

> membarrier (283 on riscv64/aarch64, 324 on x86_64):
> musl calls MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED at pthread_create.
> We run on a simple uniprocessor kernel with coherent shared memory,
> so just acknowledge without doing anything.

**第 1389 行（行尾）**

> riscv64 / aarch64 __NR_sync

**第 1390 行（行尾）**

> x86_64  __NR_sync

**第 1391 行（行尾）**

> riscv64 / aarch64 __NR_membarrier

**第 1392 行（行尾）**

> x86_64  __NR_membarrier

**第 1396 行（行尾）**

> riscv64/aarch64 __NR_linkat

**第 1397 行（行尾）**

> x86_64 __NR_linkat

**第 1401 行（行尾）**

> riscv64/aarch64 __NR_fchmodat

**第 1402 行（行尾）**

> riscv64/aarch64 __NR_fchownat

**第 1403 行（行尾）**

> riscv64/aarch64 __NR_fchown

**第 1407 行（行尾）**

> riscv64/aarch64 __NR_setitimer

**第 1408 行（行尾）**

> riscv64/aarch64 __NR_getitimer

**第 1409 行**

> ITIMER_REAL(alarm/setitimer)。实现见 kernel/time/itimer.c。

> 以前这里是纯桩：把 old_value 清零, 返回 0, 从不真正装定时器。
> 后果不只是&quot;少个功能&quot;—— LTP 每个测例都用 alarm() 当看门狗来打断
> 阻塞 read, 桩吃掉它之后卡住的测例会永久挂死整个套件。

> 参数：
> setitimer(which, new_value, old_value)  regs[1]=new regs[2]=old
> getitimer(which, curr_value)            regs[1]=curr

> struct itimerval 在 64 位下 32 字节：
> [+0] it_interval.tv_sec   [+8]  it_interval.tv_usec
> [+16] it_value.tv_sec     [+24] it_value.tv_usec

**第 1433 行**

> 只支持 ITIMER_REAL：本内核没有虚拟/统计 CPU 时间。明确报 EINVAL,
> 不假装成功 —— 装一个永远不会响的定时器比报错更难查。

**第 1436 行（行尾）**

> ITIMER_REAL

**第 1444 行**

> setitimer：先读用户传的新值并装载, 再回填旧值(Linux 语义：
> old_value 拿到的正是本次替换掉的那一份)。

**第 1457 行**

> new_value == NULL：取消定时器(Linux 同样语义)

**第 1469 行**

> 用户指针一律经 copy_to_user_bytes, 禁止裸解引用(见 CLAUDE.md)

**第 1480 行（行尾）**

> riscv64/aarch64 __NR_sched_setaffinity

**第 1481 行（行尾）**

> riscv64/aarch64 __NR_sched_getaffinity

**第 1496 行（行尾）**

> riscv64/aarch64 __NR_utimensat

**第 1497 行（行尾）**

> riscv64/aarch64 __NR_msync

**第 1501 行（行尾）**

> riscv64 __NR_riscv_hwprobe: let libc fall back quietly.

**第 1505 行（行尾）**

> riscv64 __NR_riscv_flush_icache

**第 1509 行（行尾）**

> riscv64/aarch64 __NR_statfs

**第 1510 行（行尾）**

> riscv64/aarch64 __NR_fstatfs

**第 1511 行（行尾）**

> x86_64 __NR_statfs

**第 1512 行（行尾）**

> x86_64 __NR_fstatfs

**第 1519 行（行尾）**

> f_type = EXT4_SUPER_MAGIC

**第 1520 行（行尾）**

> f_bsize

**第 1521 行（行尾）**

> f_blocks (1GB)

**第 1522 行（行尾）**

> f_bfree

**第 1523 行（行尾）**

> f_bavail

**第 1524 行（行尾）**

> f_files

**第 1525 行（行尾）**

> f_ffree

**第 1526 行（行尾）**

> f_namelen

**第 1527 行（行尾）**

> f_frsize

**第 1533 行**

> 连分类都没有 —— 够得上 ERROR。但用户态探测(busybox 启动时会试一批
> 调号)会让它刷屏, 所以只说一次。

**第 1542 行**

> 结束 syscall stime 计时(EXIT 类 syscall 不会到达这里)

**第 1551 行**

> 回填环形缓冲里的返回值。不返回的 syscall(execve/exit)走不到这里,
> 那条记录会一直保持&quot;待返回&quot;哨兵 —— dump 时显示成 &quot;= ?&quot;, 本身就是信息。

**第 1563 行**

> 在返回用户态前投递 pending 信号
> 注意：必须在 syscall_abi_set_ret 之后调用,
> 这样 sigframe 里保存的帧已含有正确的 syscall 返回值。

</details>

**位置**：`kernel/syscall/syscall.c:664`

<a id="fn-kernel-syscall-syscall.c-1576"></a>

### `sys_write`

```c
int64_t sys_write(const char *str, uint64_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验并截断长度，经 copy_from_user_bytes 拷入内核缓冲后调用 klog_write 输出，返回写入字节数。

<details>
<summary>函数内备注（1 条）</summary>

**第 1585 行**

> 注意： str 来自 regs[], 是用户指针 —— 必须先经 copy_from_user_bytes
> 落进内核缓冲再输出, 绝不能裸解引用(用户传个非法地址就是内核态
> #PF → 异常处理的 while(1) hlt → 整机挂死, LTP 实测踩过,
> 见 CLAUDE.md「用户指针」那条规则)。

> 输出走 klog_write()：它是往物理 UART 写的唯一入口, 整段持锁,
> 所以这一整条 write 不会与别的核的日志逐字符插花。

</details>

**位置**：`kernel/syscall/syscall.c:1576`

<a id="fn-kernel-syscall-syscall.c-1602"></a>

### `sys_yield`

```c
int64_t sys_yield(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 task_yield 让出 CPU 后返回 0。

**位置**：`kernel/syscall/syscall.c:1602`

<a id="fn-kernel-syscall-syscall.c-1608"></a>

### `sys_getpid`

```c
int64_t sys_getpid(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取当前任务并返回其 id。

**位置**：`kernel/syscall/syscall.c:1608`

<a id="fn-kernel-syscall-syscall.c-1614"></a>

### `sys_sleep`

```c
int64_t sys_sleep(uint64_t ms)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

毫秒数为 0 时立即返回，否则调用 timer_delay_ms 延时指定毫秒后返回 0。

**位置**：`kernel/syscall/syscall.c:1614`

<a id="fn-kernel-syscall-syscall.c-1623"></a>

### `sys_read`

```c
int64_t sys_read(char *buf, uint64_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

缓冲区为空或长度为 0 时返回 -1，否则直接返回 0，未做任何读取。

**位置**：`kernel/syscall/syscall.c:1623`

<a id="fn-kernel-syscall-syscall.c-1631"></a>

### `sys_open`

```c
int64_t sys_open(const char *pathname, int flags, int mode)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略参数，用 WARN_ONCE 记录一次未实现告警并返回 -1。

<details>
<summary>函数内备注（1 条）</summary>

**第 1635 行**

> 返回失败是&quot;未实现&quot;的正确行为, 不是错误；但&quot;缺哪些&quot;正是 bring-up
> 想知道的清单, 而 shell 会高频调用它 —— 所以 WARN + 只说一次。

</details>

**位置**：`kernel/syscall/syscall.c:1631`

<a id="fn-kernel-syscall-syscall.c-1642"></a>

### `sys_close`

```c
int64_t sys_close(int fd)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 WARN_ONCE 记录一次未实现告警并返回 -1。

**位置**：`kernel/syscall/syscall.c:1642`

<a id="fn-kernel-syscall-syscall.c-1648"></a>

### `sys_gettimeofday`

```c
int64_t sys_gettimeofday(struct timeval *tv, void *tz)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

tv 非空时用 kernel_get_ns 换算秒与微秒填入，返回 0，否则返回 -1。

<details>
<summary>函数内备注（1 条）</summary>

**第 1652 行**

> tv 是用户指针，必须经 copy_to_user_bytes 写回 —— 直接 tv-&gt;tv_sec
> 会让用户传 (void *)-1 时内核态 #PF，整机挂死(见 CLAUDE.md 的用户
> 指针规则)。tv == NULL 是合法调用(只查询时区)；本内核没有时区。

</details>

**位置**：`kernel/syscall/syscall.c:1648`

<a id="mod-kernel-syscall-syscall_internal.h"></a>

## kernel/syscall/syscall_internal.h — 内核 syscall 实现内部共享头

`kernel/syscall/syscall_internal.h` · 1 个函数 · 1 个有说明 · 488 行

仅供 kernel/syscall 目录下的 C 文件使用。包含：
- Linux/x86 syscall 号常量
- ioctl request 码
- POSIX errno 值
- 与 musl/glibc 用户空间对齐的 kernel-side struct
- CLONE/FUTEX/MAP/SIG_* 标志

不要在驱动或用户接口中包含。

<a id="fn-kernel-syscall-syscall_internal.h-332"></a>

### `kernel_get_ns`

```c
static inline uint64_t kernel_get_ns(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接返回 timer_get_ns() 的纳秒计数。

**位置**：`kernel/syscall/syscall_internal.h:332`

<a id="mod-kernel-syscall-trace.c"></a>

## kernel/syscall/trace.c — 系统调用环形缓冲的实现

`kernel/syscall/trace.c` · 10 个函数 · 10 个有说明 · 489 行

设计要点见 trace.h。这里只补充三个实现上的选择：

1. 一条 syscall 只占一个槽位：入口写满参数, 返回值留哨兵, 出口回填。
如果入口/出口各写一条, 环形缓冲的有效深度直接减半, 而且崩在 syscall
中间时会剩一个配不上对的孤立入口行 —— 那正是最需要看清楚的情况。

2. 无锁：syscall 路径本来就关中断, 且不迁移, 所以同一个 CPU 只有
它自己在写自己的环。跨 CPU 的先后顺序靠 at_ms + seq 在 dump 时归并。

3. 抓路径走 copy_string_from_user()：来自 regs[] 的用户指针禁止裸解引用
(用户传个非法地址就会让内核态读到坏地址 → #PF → 整机挂死, LTP 实测过)。

<a id="fn-kernel-syscall-trace.c-36"></a>

### `trace_snprintf`

```c
static int trace_snprintf(char *buf, int size, const char *fmt, ...)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 va_list 包装 my_vsnprintf 格式化字符串并返回其长度。

**位置**：`kernel/syscall/trace.c:36`

<a id="fn-kernel-syscall-trace.c-56"></a>

### `trace_cpu`

```c
static inline uint32_t trace_cpu(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取 klog_cpu_id，超过最大 CPU 数时归零返回。

**位置**：`kernel/syscall/trace.c:56`

<a id="fn-kernel-syscall-trace.c-62"></a>

### `syscall_trace_count`

```c
uint32_t syscall_trace_count(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

累加各 CPU 环形缓冲的条数（单个上限为缓冲深度）并返回总数。

**位置**：`kernel/syscall/trace.c:62`

<a id="fn-kernel-syscall-trace.c-75"></a>

### `syscall_trace_name`

```c
const char *syscall_trace_name(uint32_t nr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 switch 把 syscall 号映射成名字，未知号返回问号字符串。

<details>
<summary>函数内备注（5 条）</summary>

**第 78 行**

> 文件

**第 140 行**

> 进程 / 内存

**第 200 行**

> 信号

**第 216 行**

> 网络

**第 250 行**

> 多路复用

</details>

**位置**：`kernel/syscall/trace.c:75`

<a id="fn-kernel-syscall-trace.c-270"></a>

### `path_arg_of`

```c
static const char *path_arg_of(uint32_t nr, const uint64_t *a)
```

路径型 syscall：哪个参数是用户态路径串。
判断错的后果是往环里存一段无意义的字节, 所以宁可少认几个, 不要乱认。

<details>
<summary>函数内备注（2 条）</summary>

**第 273 行**

> at 家族：a0 是 dirfd, 路径在 a1

**第 285 行**

> 路径直接在 a0

</details>

**位置**：`kernel/syscall/trace.c:270`

<a id="fn-kernel-syscall-trace.c-297"></a>

### `syscall_trace_enter`

```c
void syscall_trace_enter(uint16_t pid, uint32_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在当前 CPU 的环形槽记录序号、参数、时间与 pid，尝试拷入路径，推进写指针并置待完成标志。

**位置**：`kernel/syscall/trace.c:297`

<a id="fn-kernel-syscall-trace.c-328"></a>

### `syscall_trace_exit`

```c
void syscall_trace_exit(int64_t ret)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若存在待完成记录，回填返回值并置已返回标志，同时清除待完成状态。

**位置**：`kernel/syscall/trace.c:328`

<a id="fn-kernel-syscall-trace.c-359"></a>

### `render_one`

```c
static int render_one(char *out, int len, const syscall_trace_rec_t *r)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按是否带路径拼接 pid、时间、syscall 名与参数，再追加返回值或没有返回的提示文本。

**位置**：`kernel/syscall/trace.c:359`

<a id="fn-kernel-syscall-trace.c-393"></a>

### `syscall_trace_render`

```c
int syscall_trace_render(uint16_t pid_filter, uint32_t max, char *out, int len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

为各 CPU 建归并游标，按时间与序号挑选最旧记录渲染，支持 pid 过滤并受 max 限制。

<details>
<summary>函数内备注（4 条）</summary>

**第 403 行**

> 初始化每个 CPU 的游标：指向它自己&quot;最近 DEPTH 条&quot;里最旧的那条

**第 409 行**

> 没绕圈时从 0 开始；绕过后最旧的一条在 head 处

**第 425 行**

> 被过滤掉的也要推进游标, 否则死循环

**第 428 行（行尾）**

> 重新看这个 CPU 的下一条

</details>

**位置**：`kernel/syscall/trace.c:393`

<a id="fn-kernel-syscall-trace.c-456"></a>

### `syscall_trace_dump`

```c
void syscall_trace_dump(uint16_t pid_filter, uint32_t max)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把记录渲染到静态缓冲，统计行数后按行以 KLOG_WARN 输出。

<details>
<summary>函数内备注（2 条）</summary>

**第 464 行**

> 报实际条数而不是 max —— 程序只跑了 3 个 syscall 时,
> 说&quot;最后 48 条&quot;会让人以为中间丢了记录。

**第 477 行**

> buf 里已经是多行文本；逐行走 klog, 让每行都带上级别前缀和 cpu/文件信息

</details>

**位置**：`kernel/syscall/trace.c:456`

<a id="mod-kernel-task-aarch64-cpu_startup.c"></a>

## kernel/task/aarch64/cpu_startup.c — AArch64 BSP / 次级核启动

`kernel/task/aarch64/cpu_startup.c` · 8 个函数 · 2 个有说明 · 205 行

通用 CPU 管理留在 kernel/task/cpu.c；这里只放架构相关部分：
PSCI CPU_ON 拉起、GIC per-CPU 初始化、MPIDR→hw_id 推导，
以及 SMP 自检里 AArch64 专有的诊断输出。

<a id="fn-kernel-task-aarch64-cpu_startup.c-36"></a>

### `arch_cpu_bsp_hw_id`

```c
uint64_t arch_cpu_bsp_hw_id(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/aarch64/cpu_startup.c:36`

<a id="fn-kernel-task-aarch64-cpu_startup.c-59"></a>

### `cpu_secondary_bootstrap`

```c
void cpu_secondary_bootstrap(uint32_t cpu_id)
```

cpu_secondary_bootstrap - 次级核 C 入口

由 _secondary_start 在以下条件成立后调用：
- EL2/VHE 已配置(与 BSP 一致)
- MMU 已开启, 共享 BSP 的内核 PGD
- VBAR_EL1 已指向 exception_vector_base
- DAIF.I 仍为关闭

启动步骤：
1. 安装 per-CPU 指针 + GIC CPU interface
2. 建立本核 idle 任务(current_task = idle_task)
3. 启用本地 timer PPI(CNTP_CTL_EL0)
4. 通知 BSP online, 开 IRQ, 进入 idle 循环

run_queue 为空时 sched_schedule 会一直选回自己的 idle；跨核 enqueue
与 resched IPI 会把本核唤醒去跑别的任务。

<details>
<summary>函数内备注（9 条）</summary>

**第 63 行**

> 每核独立寄存器：必须在跑任何 FP/SIMD 之前打开 CPACR_EL1.FPEN,
> 否则 EL0 上 musl 启动早期访问 NEON 会触发 EC=0x7 trap。
> BSP 在 kernel_main 入口已做同样操作；AP 必须在此处补上。

**第 69 行**

> 安装 TPIDR_EL1, 使 cpu_current() 在本核返回正确指针

**第 73 行**

> 控制器 per-CPU 初始化(GIC: PMR/CTLR/GICR)

**第 76 行**

> 必须在开中断之前装好 current_task, 否则首个 timer ISR 进入
> sched_check_and_yield 会读到 NULL。

**第 82 行**

> 启用本地 timer PPI(CNTP_TVAL/CTL 与 GICD ISENABLER0)。

**第 85 行**

> resched IPI：必须在 online 之前装好, 否则 BSP 发来的 kick 会丢。

**第 88 行**

> 通知 BSP 本核就绪。wmb 确保前面所有写对其他核可见。

**第 92 行**

> 开 IRQ；之后 timer / resched IPI 会驱动本核的 sched_check_and_yield。

**第 95 行**

> Idle 循环：主动 yield 一次再 wfi。yield 在 run_queue 空时是空操作
> (sched_schedule 选回自己)；远端入队会先置 need_resched 并打断 wfi，
> 下一轮循环即可拿到新任务。

</details>

**位置**：`kernel/task/aarch64/cpu_startup.c:59`

<a id="fn-kernel-task-aarch64-cpu_startup.c-106"></a>

### `cpu_bring_up_all`

```c
void cpu_bring_up_all(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（6 条）</summary>

**第 112 行**

> _secondary_start 链接在高 VA；PSCI 需要物理入口地址。

**第 123 行**

> RK3588: Aff0 恒为 0, 各核用 Aff1 区分, MPIDR = cpu_idx &lt;&lt; 8

**第 126 行（行尾）**

> QEMU virt: MPIDR.Aff0 = CPU 序号

**第 134 行（行尾）**

> mpidr=

**第 135 行**

> context_id=

**第 142 行**

> 自旋等待该核置 online；带超时避免死等无效 mpidr。

</details>

**位置**：`kernel/task/aarch64/cpu_startup.c:106`

<a id="fn-kernel-task-aarch64-cpu_startup.c-165"></a>

### `arch_cpu_smp_test_begin`

```c
void arch_cpu_smp_test_begin(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 167 行**

> 该函数在 task_switch_to_idle_stack() 之前被调用, 此时 DAIF.I=1。
> 不先开 IRQ, BSP 就永远收不到 timer 中断, local_ticks 一定为 0。

</details>

**位置**：`kernel/task/aarch64/cpu_startup.c:165`

<a id="fn-kernel-task-aarch64-cpu_startup.c-175"></a>

### `dbg_read_cntp_ctl`

```c
static inline uint64_t dbg_read_cntp_ctl(void)
```

诊断：读 DAIF, CNTP_CTL_EL0, CNTPCT_EL0

**位置**：`kernel/task/aarch64/cpu_startup.c:175`

<a id="fn-kernel-task-aarch64-cpu_startup.c-182"></a>

### `dbg_read_cntpct`

```c
static inline uint64_t dbg_read_cntpct(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/aarch64/cpu_startup.c:182`

<a id="fn-kernel-task-aarch64-cpu_startup.c-187"></a>

### `arch_cpu_smp_test_diag_begin`

```c
void arch_cpu_smp_test_diag_begin(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 189 行（行尾）**

> 统一的中断状态读取

**第 195 行**

> 控制器相关的诊断寄存器由后端自己 dump，内核不再知道是 GICv2 还是 GICv3

</details>

**位置**：`kernel/task/aarch64/cpu_startup.c:187`

<a id="fn-kernel-task-aarch64-cpu_startup.c-199"></a>

### `arch_cpu_smp_test_diag_round`

```c
void arch_cpu_smp_test_diag_round(uint32_t round)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/aarch64/cpu_startup.c:199`

<a id="mod-kernel-task-aarch64-sched.c"></a>

## kernel/task/aarch64/sched.c — AArch64 调度路径的架构部分

`kernel/task/aarch64/sched.c` · 2 个函数 · 0 个有说明 · 26 行

AArch64 用 TTBR0/TTBR1 硬件分割用户/内核地址空间，页表切换由
arch_task_switch(switch.S) 负责，C 层没有额外的 MMU/TLS 准备工作。

<a id="fn-kernel-task-aarch64-sched.c-18"></a>

### `arch_sched_prev_pgd_slot`

```c
uint64_t **arch_sched_prev_pgd_slot(task_t *prev)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/aarch64/sched.c:18`

<a id="fn-kernel-task-aarch64-sched.c-23"></a>

### `arch_sched_next_pgd`

```c
uint64_t *arch_sched_next_pgd(task_t *next)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/aarch64/sched.c:23`

<a id="mod-kernel-task-completion.c"></a>

## kernel/task/completion.c — SMP 安全的一次性事件等待

`kernel/task/completion.c` · 5 个函数 · 5 个有说明 · 117 行

关键点(也是它存在的理由)：

1. 条件检查与入等待队列在同一把锁下完成 —— 唤醒方不可能落在
「检查完, 还没挂上」的缝隙里。
2. 唤醒走 task_unblock() → sched_unblock()：如果等待者还在这颗核上
执行(已置 BLOCKED, 尚未切出), 只置 wake_pending, 绝不提前入队。
3. done 先置位再唤醒；等待者醒来重新检查 done, 因此不会丢事件。

<a id="fn-kernel-task-completion.c-25"></a>

### `completion_init`

```c
void completion_init(completion_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化自旋锁与等待链表并把完成标志清零，参数为空直接返回。

**位置**：`kernel/task/completion.c:25`

<a id="fn-kernel-task-completion.c-34"></a>

### `completion_wait`

```c
void completion_wait(completion_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环把当前任务挂入等待链表并调度让出，直到完成标志置位；非任务上下文报错返回。

<details>
<summary>函数内备注（1 条）</summary>

**第 54 行**

> 等待者可能在这一刻被 complete：sched_unblock() 看到 on_cpu=true,
> 只置 READY+wake_pending；本核的 sched_schedule() 在切出时兑现。

</details>

**位置**：`kernel/task/completion.c:34`

<a id="fn-kernel-task-completion.c-65"></a>

### `completion_complete_all`

```c
void completion_complete_all(completion_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁置完成标志，并逐个摘除等待链表中的任务并唤醒。

<details>
<summary>函数内备注（1 条）</summary>

**第 79 行**

> 锁序：completion.lock → 目标核 rq_lock。反方向不存在
> (sched_schedule / sched_unblock 都不会回头拿 completion.lock)。

</details>

**位置**：`kernel/task/completion.c:65`

<a id="fn-kernel-task-completion.c-88"></a>

### `completion_done`

```c
int completion_done(completion_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁读取完成标志返回，参数为空返回 0。

**位置**：`kernel/task/completion.c:88`

<a id="fn-kernel-task-completion.c-102"></a>

### `completion_reset`

```c
int completion_reset(completion_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁在无等待者时清零完成标志，否则返回 -1。

**位置**：`kernel/task/completion.c:102`

<a id="mod-kernel-task-cpu.c"></a>

## kernel/task/cpu.c — Per-CPU 控制块 + 通用 CPU 管理

`kernel/task/cpu.c` · 8 个函数 · 8 个有说明 · 245 行

跨架构共用部分：
- g_cpus / g_num_cpus
- 跨核 resched IPI(注册、发送)
- BSP 初始化
- 次级核 idle 任务的构造
- SMP timer 健康自检的通用流程

架构差异已经收敛到：
- kernel/task/<arch>/cpu_startup.c — 次级核拉起、BSP hw_id、自检诊断
- include/arch/<arch>/cpu_impl.h       — per-CPU 寄存器、idle 等待指令

三架构的次级核拉起都收敛到 cpu_secondary_bootstrap()：
- aarch64: PSCI CPU_ON
- riscv64: SBI HSM hart_start(不支持的平台回退到 release 表 + IPI)
- x86_64 : INIT-SIPI + AP trampoline

<a id="fn-kernel-task-cpu.c-54"></a>

### `resched_ipi_entry`

```c
static void resched_ipi_entry(void *frame, void *ctx)
```

resched_ipi_handler — 收到 IPI 只做一件事：置本核 need_resched

真正的切换发生在中断返回路径(sched_check_and_yield_from_trap)或
vCPU 主循环顶部(sched_check_and_yield)。在 ISR 里直接切任务是禁止的,
见 docs/INTERRUPT_CONTEXT_SWITCH.md。

RISC-V 的 SSIP 是电平型 pending 位、x86 的 LAPIC 需要 EOI，两者都由
控制器后端的 dispatch 收尾(PLIC 清 SSIP / LAPIC 写 EOI)，这里只置位。

**位置**：`kernel/task/cpu.c:54`

<a id="fn-kernel-task-cpu.c-63"></a>

### `cpu_ipi_init`

```c
void cpu_ipi_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向中断控制器注册重调度 IPI 处理函数，失败时打印警告。

<details>
<summary>函数内备注（1 条）</summary>

**第 65 行**

> IPI 号/向量由控制器决定，内核不再关心 SGI 15 / scause 1 / 向量 0xF0

</details>

**位置**：`kernel/task/cpu.c:63`

<a id="fn-kernel-task-cpu.c-70"></a>

### `cpu_send_ipi`

```c
void cpu_send_ipi(uint32_t cpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验目标 CPU 编号后，按硬件 ID 发送重调度 IPI。

<details>
<summary>函数内备注（1 条）</summary>

**第 77 行**

> hw_id: aarch64=MPIDR, riscv=hartid, x86=LAPIC ID

</details>

**位置**：`kernel/task/cpu.c:70`

<a id="fn-kernel-task-cpu.c-87"></a>

### `klog_cpu_id`

```c
uint32_t klog_cpu_id(void)
```

klog_cpu_id - 给 klog.h 宏使用, 返回当前逻辑 CPU 编号。

早期 boot(BSP 还没装 per-CPU 寄存器)arch_cpu_self_get 返回 0,
cpu_current() 退化到 &g_cpus[0], cpu_id=0(BSS 默认值), 正确。

**位置**：`kernel/task/cpu.c:87`

<a id="fn-kernel-task-cpu.c-96"></a>

### `cpu_bump_local_ticks`

```c
void cpu_bump_local_ticks(void)
```

cpu_bump_local_ticks - timer ISR 每次调用, 让本核累计 tick 计数器 +1。
用于 SMP 健康检查：cpu_smp_timer_test 会读所有核的计数器, 确认都在动。

**位置**：`kernel/task/cpu.c:96`

<a id="fn-kernel-task-cpu.c-102"></a>

### `cpu_init_bsp`

```c
void cpu_init_bsp(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化 BSP 控制块与运行队列、安装 per-CPU 寄存器，把在线核数置一并注册 IPI。

<details>
<summary>函数内备注（3 条）</summary>

**第 108 行**

> 硬件 ID 的读法按架构不同：aarch64=MPIDR, riscv=boot hartid,
> x86=LAPIC ID。差异收在 arch_cpu_bsp_hw_id()。

**第 119 行**

> 安装 per-CPU 寄存器(TPIDR_EL1 / tp / GS_BASE)

**第 124 行**

> 本核的 resched IPI 就绪；必须在 online 置位(任何人都可能开始 kick)之前完成。

</details>

**位置**：`kernel/task/cpu.c:102`

<a id="fn-kernel-task-cpu.c-148"></a>

### `cpu_setup_secondary_idle_task`

```c
void cpu_setup_secondary_idle_task(cpu_t *c)
```

cpu_setup_secondary_idle_task - 初始化次级核 idle 任务控制块

复用 secondary_boot_stacks[cpu_id] 作为 idle 的内核栈(次级核此刻
的 SP 就指向那块 SECONDARY_STACK_SIZE 大小(16KB)的栈；切换到自己时
只是恢复同一区域)。

必须在第一次允许中断 / 第一次调用 sched_schedule 之前完成,
否则 sched_check_and_yield 会读到 NULL current_task 而崩溃。

<details>
<summary>函数内备注（4 条）</summary>

**第 154 行（行尾）**

> 次级核此刻就在 idle 上执行

**第 155 行（行尾）**

> 用高位段避免与 g_task_id_cnt 冲突

**第 162 行**

> idle 也需要 files/fs/mm。files_alloc() 会把 fd 表显式填成 -1 ——
> 不能靠 memset 归零：0 是合法的 fd 池槽位号，fd_table_inherit()
> 会把它当成「256 个都指向槽位 0 的 fd」，一次继承就把
> FD_POOL_SIZE(128) 个槽全部吃光（实测过：之后宿主里所有 open()
> 都失败）。CPU0 的 idle 走 task.c 的 g_idle_task，同样在
> task_init() 里分配。

**第 174 行**

> 名字：&quot;idle/N&quot;

</details>

**位置**：`kernel/task/cpu.c:148`

<a id="fn-kernel-task-cpu.c-202"></a>

### `cpu_smp_timer_test`

```c
void cpu_smp_timer_test(uint32_t rounds, uint32_t ms_per_round)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印定时器与中断诊断后按轮延时比较各核 local_ticks 增量，任一核不增长即判失败。

<details>
<summary>函数内备注（1 条）</summary>

**第 204 行**

> 即使单核也跑诊断：先确认 BSP 自己的 timer 是否在 tick。

</details>

**位置**：`kernel/task/cpu.c:202`

<a id="mod-kernel-task-elf_loader.c"></a>

## kernel/task/elf_loader.c — ELF 程序加载器(文件 I/O 层)

`kernel/task/elf_loader.c` · 4 个函数 · 4 个有说明 · 337 行

职责：从 ext4 文件系统读取 ELF 文件, 然后交由 task_execve 处理。
ELF 格式解析/段加载在 mm/elf_image.c, 进程创建与 exec 提交在 task/exec.c。

<a id="fn-kernel-task-elf_loader.c-27"></a>

### `read_file_fully`

```c
static int read_file_fully(ext4_file *file, uint8_t *buf, uint64_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按块循环读取直到读满指定大小，任一次短读或出错返回 EIO。

**位置**：`kernel/task/elf_loader.c:27`

<a id="fn-kernel-task-elf_loader.c-52"></a>

### `resolve_symlink`

```c
static int resolve_symlink(const char *path, char *out, size_t outsz)
```

resolve_symlink - 跟随符号链接, 最多 8 层, 将最终真实路径写入 out。
若路径不是符号链接, 直接把 path 复制到 out 返回 0。
支持相对符号链接(相对于当前目录)和绝对符号链接。
返回 0 表示成功, -1 表示循环太深。

<details>
<summary>函数内备注（6 条）</summary>

**第 67 行**

> 不是符号链接(或 readlink 失败)：cur 就是最终路径

**第 78 行**

> 组合新路径

**第 81 行**

> 绝对符号链接

**第 89 行**

> 相对符号链接：取 cur 的目录部分拼上 target

**第 103 行**

> next → cur, 进入下一轮

**第 111 行**

> 符号链接层数超限

</details>

**位置**：`kernel/task/elf_loader.c:52`

<a id="fn-kernel-task-elf_loader.c-120"></a>

### `elf_loader_load_from_file`

```c
int elf_loader_load_from_file(const char *pathname, char **argv, char **envp, trap_frame_t *frame)
```

elf_loader_load_from_file - 从文件系统加载并执行 ELF 程序

**位置**：`kernel/task/elf_loader.c:120`

<a id="fn-kernel-task-elf_loader.c-125"></a>

### `load_from_file_depth`

```c
static int load_from_file_depth(const char *pathname, char **argv, char **envp, trap_frame_t *frame, int depth)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 ext4 读取可执行文件，解析符号链接与 #!/PT_INTERP（递归深度受限），把文件/解释器缓冲交给 task_execve 原地 exec；失败返回负 errno。

<details>
<summary>函数内备注（19 条）</summary>

**第 135 行**

> 复制路径名

**第 145 行**

> 解析符号链接(busybox applet 等均为 symlink)

**第 155 行**

> 打开文件

**第 159 行（行尾）**

> lwext4 errno == Linux errno (ENOENT=2, etc.)

**第 162 行**

> 获取文件大小

**第 170 行（行尾）**

> -ENOEXEC

**第 175 行**

> 分配内存

**第 181 行（行尾）**

> -ENOMEM

**第 186 行**

> 读取文件

**第 191 行（行尾）**

> -EIO

**第 198 行**

> ── Shebang (#!) 脚本处理 ───────────────────────────────────────

**第 203 行（行尾）**

> -ENOEXEC

**第 206 行**

> 解析 &quot;#!&lt;interpreter&gt; [optional-arg]\n&quot; — 只看前 256 字节

**第 226 行（行尾）**

> -ENOEXEC

**第 229 行**

> 可选参数(如 &quot;#!/usr/bin/env sh&quot; 中的 &quot;sh&quot;)

**第 248 行**

> 构建新 argv: [interp, interp_arg?, script_path, original_argv[1..], NULL]

**第 266 行**

> ── 检查 PT_INTERP(动态连接器路径)─────────────────────────────

**第 284 行**

> interpreter 路径也可能是符号链接

**第 326 行**

> 加载并执行 ELF(原地 exec)。
> 传 resolved 而不是 path_buf：/proc/self/exe 和 comm 用 Linux 语义的
> 真实可执行文件路径, 而不是用户给的、可能带符号链接的路径。

> 缓冲区所有权从这里转交给 task_execve: 无论成功失败都由它释放,
> 本函数返回后不得再碰 file_data / interp_data(以前这里再 kfree 一次,
> 在原地 exec 语义下会 double free)。

</details>

**位置**：`kernel/task/elf_loader.c:125`

<a id="mod-kernel-task-exec.c"></a>

## kernel/task/exec.c — execve 进程执行语义(Linux 风格: 原地替换映像)

`kernel/task/exec.c` · 6 个函数 · 6 个有说明 · 531 行

职责：
1. 新用户页表初始化(PGD 分配 + 内核映射复制)
2. Linux ABI 初始栈构建(argc/argv/envp/auxv)
3. 新映像的段/解释器加载 + 新用户栈映射
4. 原地提交：换页表/入口, 保留 PID、fd、信号掩码等进程属性
5. 失败时旧映像保持原样(Linux execve 语义)

与旧实现的区别：以前是"新建任务 + 调用者阻塞当 wrapper, 等子进程退出,
再以相同状态退出"。那样每次 exec 都会在任务表里留下一个同名的阻塞空壳
(ps 里能直接看到), 而且被 exec 程序的 PID 会变。现在直接替换当前任务的
地址空间和 trap_frame：pid/tgid 不变, 没有 wrapper, ps 的父子关系与
Linux 一致。

<a id="fn-kernel-task-exec.c-83"></a>

### `elf_setup_stack`

```c
static int elf_setup_stack(void *pgd, uint64_t stack_top, uint64_t entry, const char *pathname, char **argv, char **envp, uint64_t *out_sp, uint64_t phdr_uaddr, uint16_t phnum, uint16_t phent, uint64_t at_base)
```

elf_setup_stack - 在新用户栈最高页写入 Linux ABI 初始栈
@pgd:       用户页表(内核虚拟地址)
@stack_top: 用户栈顶(USER_STACK_TOP)
@entry:     ELF 入口点
@pathname:  程序路径名
@argv:      来自 execve 的 argv(可为 NULL)
@envp:      来自 execve 的 envp(可为 NULL)
@out_sp:    输出：新的用户栈指针
@phdr_uaddr: 程序头的用户虚拟地址(AT_PHDR auxv)
@phnum:     e_phnum
@phent:     e_phentsize

构建布局(从高地址向低)：
strings (argv[0..n], envp[0..m])
8-byte aligned
auxv: AT_PHDR, AT_PHENT, AT_PHNUM, AT_ENTRY, AT_PAGESZ, AT_RANDOM, AT_NULL
envp ptrs + NULL
argv ptrs + NULL
argc
<- new sp

<details>
<summary>函数内备注（9 条）</summary>

**第 87 行（行尾）**

> 0x6ffff000

**第 137 行**

> AT_RANDOM 需要的 16 字节随机区(glibc/musl 启动必需)

**第 157 行（行尾）**

> AT_RANDOM

**第 159 行（行尾）**

> AT_PAGESZ

**第 161 行（行尾）**

> AT_ENTRY

**第 164 行（行尾）**

> AT_BASE: interpreter 加载基址(静态连接时为 0)

**第 168 行（行尾）**

> AT_PHNUM

**第 170 行（行尾）**

> AT_PHENT

**第 172 行（行尾）**

> AT_PHDR

</details>

**位置**：`kernel/task/exec.c:83`

<a id="fn-kernel-task-exec.c-195"></a>

### `exec_free_loader_buffer`

```c
static void exec_free_loader_buffer(const void *data, uint64_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按页对齐大小换算页数，将缓冲区虚拟地址转物理地址后归还 PMM。

**位置**：`kernel/task/exec.c:195`

<a id="fn-kernel-task-exec.c-210"></a>

### `exec_close_cloexec_fds`

```c
static void exec_close_cloexec_fds(task_t *t)
```

exec_close_cloexec_fds - 关闭带 FD_CLOEXEC 的 fd(其余保留)

旧实现是"新任务继承 fd 后把 wrapper 的全部 fd 关掉"; 原地 exec 保留
当前 fd 表, 只按 Linux 语义清理 CLOEXEC 项。

**位置**：`kernel/task/exec.c:210`

<a id="fn-kernel-task-exec.c-235"></a>

### `exec_reset_signal_state`

```c
static void exec_reset_signal_state(task_t *t)
```

exec_reset_signal_state - Linux exec 的信号语义

- 被 caught 的信号处理函数重置为 SIG_DFL
- SIG_IGN 原样保留(这是 glibc/musl 在 exec 后仍能忽略 SIGCHLD 等的前提)
- 信号掩码 blocked_sigs 保留(POSIX: 跨 exec 不变)
- 正在执行的 handler 栈帧作废(从 handler 里 execve 的极端场景)

pending_sigs 不清：Linux 也没有在 execve 里清 pending 信号。

<details>
<summary>函数内备注（1 条）</summary>

**第 237 行**

> signal_struct 可能被线程共享，exec 会先 unshare；SIG_IGN 保留。

</details>

**位置**：`kernel/task/exec.c:235`

<a id="fn-kernel-task-exec.c-249"></a>

### `exec_set_name`

```c
static void exec_set_name(task_t *t, mm_struct_t *mm, const char *pathname)
```

exec_set_name - 更新 comm(/proc/<pid>/stat 的 Name)和 /proc/self/exe

Linux 的 comm 是可执行文件的 basename(15 字节), /proc/self/exe 是
解析过符号链接的绝对路径。以前两者都是加载时的原始入参。

**位置**：`kernel/task/exec.c:249`

<a id="fn-kernel-task-exec.c-286"></a>

### `task_execve`

```c
int task_execve(const char *pathname, uint8_t *file_data, uint64_t file_size, uint8_t *interp_data, uint64_t interp_size, char **argv, char **envp, trap_frame_t *frame)
```

task_execve - 把 ELF 映像原地装进当前进程并执行

@frame: 用户进程从 syscall 进来时传入 trap_frame(返回时改它);
内核线程(开机 busybox_loader)传 NULL, 走 arch_enter_user。

成功时：用户进程返回 0(syscall 返回路径随后把控制权交给新程序);
内核线程不返回, 直接进入用户态。
失败时：返回负错误码, 当前进程的旧映像/fd/信号状态完全不变。

<details>
<summary>函数内备注（25 条）</summary>

**第 294 行（行尾）**

> 仅当旧 mm 仍被线程共享时才新分配

**第 303 行**

> 1. 分配并清零新用户页表 —— 旧映像此时完全不受影响

**第 314 行**

> 2. 注入架构要求的内核映射(AArch64 为空操作)

**第 317 行**

> 3. 加载主程序 ELF 段 + RELA 重定位

**第 324 行**

> 3b. 加载动态连接器(如果有)——加载到同一 PGD 的独立地址区

**第 325 行（行尾）**

> 静态连接时：直接跳入主程序

**第 328 行**

> interp 区的末端, 记到 task-&gt;mm-&gt;interp_end 供 fork 复制用。
> 0 = 本进程是静态链接的(绝大多数程序), fork 时不必扫这一带。

**第 340 行（行尾）**

> 动态连接：跳入 ld-musl

**第 341 行（行尾）**

> AT_BASE = interpreter 实际加载基址

**第 350 行**

> 4. 分配并映射用户栈

**第 369 行**

> 4b. 映射信号返回蹦床页(x86_64 用户栈 NX, 不能放 trampoline 在栈上)

**第 375 行（行尾）**

> int3 填充(安全兜底)

**第 376 行**

> mov $15, %eax; syscall  — rt_sigreturn on x86_64

**第 389 行**

> 5. 构建 Linux ABI 初始栈

**第 398 行**

> 6. 计算堆/ mmap 基址(与原实现一致)

**第 404 行**

> 新映像已经完整可用, loader 缓冲不再需要。从这里开始进入提交阶段,
> 没有失败路径。缓冲归本函数所有: elf_loader 在 task_execve 返回后
> 不得再释放它们。

**第 412 行**

> 选出 exec 之后的 mm：
> - 旧 mm 只被当前任务引用（refcount==1）→ 直接复用；
> - 仍与其它线程共享 → 新分配一份，旧 mm 留给线程组。
> 新 mm 必须在关中断前准备好（mm_alloc 会拿池锁）；池大小保证
> 共享场景下一定有空槽（共享意味着 struct 数 &lt; 任务数）。

**第 432 行**

> ── 提交点 ──────────────────────────────────────────────────
> 关中断的原因：改 mm-&gt;pgd 与真正切换 MMU 之间如果被抢占,
> aarch64 的 arch_task_switch 会把&quot;当前 TTBR0&quot;(还是旧页表)存回
> prev-&gt;mm-&gt;pgd, 把刚写进去的新页表指针覆盖掉。

**第 448 行**

> 换映像。PID/tgid、parent_id、优先级、cwd、uid/gid、pgid/sid、
> blocked_sigs、累计 CPU 时间都保留(Linux 语义)。

**第 468 行**

> exec 重置 robust_list（Linux 语义）；pdeathsig/umask/groups/rlimits 保留

**第 473 行**

> 7. 把本 CPU 切到新页表; 必须早于销毁旧页表

**第 477 行**

> 返回用户态不再经过调度器(sched.c 里的 CR3/TSS/FS 设置), 这里补上:
> - 内核任务 exec 时 TSS.RSP0 还指着别人/初始值
> - exec 不继承 TLS, FS_BASE 必须清零

**第 490 行**

> 用户进程：改写 trap frame, 按原路(syscall 返回)回到新程序的入口。
> 内核任务：没有 trap frame, 直接进入用户态, 不返回。

**第 498 行**

> 复用的 mm：旧 pgd 已经不在任何人的地址空间里，直接销毁。
> 新分配的 mm：旧 mm 仍归其它线程，mm_put 只在最后一个引用
> 退出时才销毁页表。

**第 514 行**

> 内核线程 exec：没有 trap frame，释放旧 mm 后直接进入用户态。

</details>

**位置**：`kernel/task/exec.c:286`

<a id="mod-kernel-task-mutex.c"></a>

## kernel/task/mutex.c — 睡眠锁(Mutex)实现

`kernel/task/mutex.c` · 6 个函数 · 6 个有说明 · 169 行

<a id="fn-kernel-task-mutex.c-21"></a>

### `mutex_init`

```c
void mutex_init(mutex_t *mutex)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化内部自旋锁与等待链表，并清零持锁标志、持有者和递归计数。

**位置**：`kernel/task/mutex.c:21`

<a id="fn-kernel-task-mutex.c-37"></a>

### `mutex_lock`

```c
void mutex_lock(mutex_t *mutex)
```

条件检查、入等待队列与置 BLOCKED 全在 mutex->lock 下完成。
这是修掉旧实现丢唤醒的关键：旧代码先恢复中断、再 task_block()，
解锁如果落在两者之间，等待者会睡进一把已经释放的锁。

<details>
<summary>函数内备注（4 条）</summary>

**第 47 行（行尾）**

> 首次持有, 计数为 1

**第 51 行**

> 可重入：同一任务再次加锁, 直接递增计数

**第 61 行**

> 入队与 BLOCKED 已经发布，解锁方不会漏掉这次唤醒。
> 若在真正切出前就被唤醒，sched_unblock 只置 wake_pending，
> 由 sched_schedule() 在切出时补入队列（见 task/completion.c）。

**第 69 行**

> 被唤醒后重新检查：可能已被别的任务抢到

</details>

**位置**：`kernel/task/mutex.c:37`

<a id="fn-kernel-task-mutex.c-76"></a>

### `mutex_unlock`

```c
void mutex_unlock(mutex_t *mutex)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验持有者并递减递归计数，归零时清持有者、释放锁并唤醒队首等待任务。

<details>
<summary>函数内备注（5 条）</summary>

**第 84 行**

> 检查是否是锁的持有者

**第 91 行**

> 可重入：计数仍大于 1, 只递减, 不真正释放

**第 97 行**

> 先从等待队列中取出第一个任务

**第 103 行**

> 释放锁

**第 108 行**

> 锁序：mutex-&gt;lock → 目标核 rq_lock（task_unblock 内部）。
> 反方向不存在；completion.c 用同一顺序。

</details>

**位置**：`kernel/task/mutex.c:76`

<a id="fn-kernel-task-mutex.c-120"></a>

### `mutex_trylock`

```c
bool mutex_trylock(mutex_t *mutex)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未加锁时立即取得锁，已由当前任务持有时递增递归计数，否则返回失败。

<details>
<summary>函数内备注（1 条）</summary>

**第 134 行**

> 可重入：已是持有者, 递增计数

</details>

**位置**：`kernel/task/mutex.c:120`

<a id="fn-kernel-task-mutex.c-147"></a>

### `mutex_is_locked`

```c
bool mutex_is_locked(mutex_t *mutex)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁读取持锁标志后返回。

**位置**：`kernel/task/mutex.c:147`

<a id="fn-kernel-task-mutex.c-160"></a>

### `mutex_holder`

```c
task_t *mutex_holder(mutex_t *mutex)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁读取并返回当前持锁任务指针。

**位置**：`kernel/task/mutex.c:160`

<a id="mod-kernel-task-preempt.c"></a>

## kernel/task/preempt.c

`kernel/task/preempt.c` · 4 个函数 · 4 个有说明 · 74 行

<a id="fn-kernel-task-preempt.c-19"></a>

### `preempt_disable`

```c
void preempt_disable(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

当前任务存在时把其抢占计数加一。

<details>
<summary>函数内备注（1 条）</summary>

**第 23 行**

> 在 preempt_disable 里取返回地址，得到的是它的调用者（spin_lock
> 内联后就是真正持锁的函数），而不是 rt_latency_preempt_off_begin
> 内部看到的本函数地址。

</details>

**位置**：`kernel/task/preempt.c:19`

<a id="fn-kernel-task-preempt.c-35"></a>

### `preempt_enable`

```c
void preempt_enable(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

递减抢占计数，在计数归零、无中断嵌套且有待调度标志时清标志并调用调度器。

<details>
<summary>函数内备注（1 条）</summary>

**第 52 行**

> 如果当前仍处于 irqsave 临界区(中断被屏蔽), 不能在这里切换。
> 判断交给统一的 arch_irq_is_enabled(), 不再按架构各写一遍。

</details>

**位置**：`kernel/task/preempt.c:35`

<a id="fn-kernel-task-preempt.c-64"></a>

### `preemptible`

```c
bool preemptible(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

中断嵌套深度为零且当前任务抢占计数为零时返回可抢占。

**位置**：`kernel/task/preempt.c:64`

<a id="fn-kernel-task-preempt.c-71"></a>

### `in_irq_context`

```c
bool in_irq_context(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

当前 CPU 的中断嵌套深度非零时返回真。

**位置**：`kernel/task/preempt.c:71`

<a id="mod-kernel-task-riscv64-cpu_startup.c"></a>

## kernel/task/riscv64/cpu_startup.c — RISC-V BSP / 次级核启动

`kernel/task/riscv64/cpu_startup.c` · 6 个函数 · 0 个有说明 · 143 行

通用 CPU 管理留在 kernel/task/cpu.c；这里只放架构相关部分：
SBI HSM hart_start(不支持时回退到 release 表 + IPI)、hartid 推导，
以及 SMP 自检的 RISC-V 部分。

<a id="fn-kernel-task-riscv64-cpu_startup.c-37"></a>

### `arch_cpu_bsp_hw_id`

```c
uint64_t arch_cpu_bsp_hw_id(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/riscv64/cpu_startup.c:37`

<a id="fn-kernel-task-riscv64-cpu_startup.c-42"></a>

### `cpu_secondary_bootstrap`

```c
void cpu_secondary_bootstrap(uint32_t cpu_id)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 47 行（行尾）**

> SUM

**第 53 行**

> resched IPI：必须在 online 之前装好, 否则 BSP 发来的 kick 会丢。

</details>

**位置**：`kernel/task/riscv64/cpu_startup.c:42`

<a id="fn-kernel-task-riscv64-cpu_startup.c-66"></a>

### `cpu_bring_up_all`

```c
void cpu_bring_up_all(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/riscv64/cpu_startup.c:66`

<a id="fn-kernel-task-riscv64-cpu_startup.c-131"></a>

### `arch_cpu_smp_test_begin`

```c
void arch_cpu_smp_test_begin(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 133 行**

> 与 AArch64 同理：调用点还在关中断的 boot 上下文里。

</details>

**位置**：`kernel/task/riscv64/cpu_startup.c:131`

<a id="fn-kernel-task-riscv64-cpu_startup.c-137"></a>

### `arch_cpu_smp_test_diag_begin`

```c
void arch_cpu_smp_test_diag_begin(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/riscv64/cpu_startup.c:137`

<a id="fn-kernel-task-riscv64-cpu_startup.c-140"></a>

### `arch_cpu_smp_test_diag_round`

```c
void arch_cpu_smp_test_diag_round(uint32_t round)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/riscv64/cpu_startup.c:140`

<a id="mod-kernel-task-riscv64-sched.c"></a>

## kernel/task/riscv64/sched.c — RISC-V 调度路径的架构部分

`kernel/task/riscv64/sched.c` · 2 个函数 · 0 个有说明 · 42 行

RISC-V 的 satp 切换由 arch_task_switch(switch.S) 完成，C 层负责
选出"要装入的 satp 根表"以及"要不要把当前 satp 存回 prev->mm->pgd"。

<a id="fn-kernel-task-riscv64-sched.c-19"></a>

### `arch_sched_prev_pgd_slot`

```c
uint64_t **arch_sched_prev_pgd_slot(task_t *prev)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 21 行**

> RISC-V 用户进程：pgd 是创建时固定的物理地址, 不需要由 arch_task_switch
> 动态保存。若传 &amp;prev-&gt;mm-&gt;pgd, 当用户进程在 arch_switch_to_user 切换 satp
> 之前被抢占(satp 仍为内核页表)时, prev-&gt;mm-&gt;pgd 会被覆写为内核页表地址,
> 导致下次调度回来时以错误的 satp 进入 U-mode → VA 0x10000 不在内核页表 → Inst PF。
> 因此对用户进程传 NULL(跳过 satp 保存), 保持 pgd 不变。

</details>

**位置**：`kernel/task/riscv64/sched.c:19`

<a id="fn-kernel-task-riscv64-sched.c-31"></a>

### `arch_sched_next_pgd`

```c
uint64_t *arch_sched_next_pgd(task_t *next)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 34 行**

> 首次进入用户进程：延迟 satp 切换, 由 arch_switch_to_user 负责

**第 38 行**

> 切换到内核任务：恢复内核根页表

</details>

**位置**：`kernel/task/riscv64/sched.c:31`

<a id="mod-kernel-task-riscv64-user_entry.c"></a>

## kernel/task/riscv64/user_entry.c — RISC-V 用户态入口钩子

`kernel/task/riscv64/user_entry.c` · 1 个函数 · 0 个有说明 · 27 行

arch_user_entry_debug 由 kernel/task/riscv64/switch.S 在首次跳转
U-mode 前调用；目前唯一的职责是把 user_started 置位，让调度器区分
"已经真正进过 U-mode"和"首次进入、satp 还没切"。

<a id="fn-kernel-task-riscv64-user_entry.c-17"></a>

### `arch_user_entry_debug`

```c
void arch_user_entry_debug(uint64_t user_entry, uint64_t user_sp, uint64_t kernel_sp, uint64_t user_pgd)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/riscv64/user_entry.c:17`

<a id="mod-kernel-task-sched.c"></a>

## kernel/task/sched.c — 内核调度器(per-CPU 轮转 + RT 优先级类)

`kernel/task/sched.c` · 26 个函数 · 26 个有说明 · 914 行

设计：
- 每核一个就绪队列(cpu_t::run_queue / rt_queue)，FIFO 顺序
- 每核一个 idle 任务(cpu_t::idle_task)，队列为空时的回退任务, 永不入队
- sched_schedule() 关中断保护调度决策和上下文切换
- 跨核入队/唤醒持目标核 rq_lock；必要时立刻发 resched IPI

调度语义(以 sched_schedule 为核心)：
1. 关中断, 保存当前中断标志(flags)
2. 若当前任务 state == RUNNING 且不是 idle, 则将其改为 READY 并入队尾
3. 从队头取下一任务；队空则选 idle
4. 若 next == prev, 则恢复 RUNNING 状态, 开中断, 返回(无切换)
5. 将 next 设为 RUNNING, 更新本 CPU current_task
6. arch_task_switch() — 切换上下文；此行 "返回" 时已是 prev 再次被调度
7. arch_irq_restore(flags) — 恢复 prev 进入调度时的中断状态

关于中断状态的正确性：
- 若 prev 是被 timer ISR 抢占：flags = "关中断"；恢复后仍关,
然后通过 ISR 的 eret/sret/iretq 还原到被中断前(开中断)的状态。
- 若 prev 是主动 yield：flags = "开中断"；恢复后直接开中断。
- 新任务(首次运行)：arch_task_switch 跳转到 task_trampoline,
trampoline 调用 arch_irq_enable() 显式开中断。

<a id="fn-kernel-task-sched.c-54"></a>

### `x86_write_msr`

```c
static inline void x86_write_msr(uint32_t msr, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 64 位值拆成高低 32 位后执行 wrmsr 写 MSR。

**位置**：`kernel/task/sched.c:54`

<a id="fn-kernel-task-sched.c-61"></a>

### `x86_write_fs_base`

```c
static inline void x86_write_fs_base(uint64_t fs_base)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

通过写 IA32_FS_BASE MSR 设置 FS 段基址。

**位置**：`kernel/task/sched.c:61`

<a id="fn-kernel-task-sched.c-80"></a>

### `sched_rq_init`

```c
void sched_rq_init(cpu_t *c)
```

── Scheduler state ─────────────────────────────────────── *

调度状态全部在 cpu_t 里, 本文件不持有全局队列：
- run_queue / idle_task / need_resched 都从 cpu_current() 读取
- current_task 是 per-CPU 状态, 不再维护全局镜像

单核(g_num_cpus == 1)时关中断即可互斥本核 ISR；多核场景每个 CPU
独立持有自己的就绪队列，跨核入队/唤醒必须持目标核的 rq_lock
(见 sched_enqueue / sched_unblock)。

**位置**：`kernel/task/sched.c:80`

<a id="fn-kernel-task-sched.c-90"></a>

### `rt_prio_of`

```c
static inline uint32_t rt_prio_of(const task_t *t)
```

有效 RT 优先级 = max(基础 RT 优先级, rt_mutex 继承优先级)。

**位置**：`kernel/task/sched.c:90`

<a id="fn-kernel-task-sched.c-95"></a>

### `rt_bitmap_set`

```c
static inline void rt_bitmap_set(cpu_t *c, uint32_t p)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把优先级 p 对应的位在 RT 优先级位图中置一。

**位置**：`kernel/task/sched.c:95`

<a id="fn-kernel-task-sched.c-100"></a>

### `rt_bitmap_clear`

```c
static inline void rt_bitmap_clear(cpu_t *c, uint32_t p)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把优先级 p 对应的位在 RT 优先级位图中清掉。

**位置**：`kernel/task/sched.c:100`

<a id="fn-kernel-task-sched.c-106"></a>

### `rt_highest_prio`

```c
static int rt_highest_prio(cpu_t *c)
```

最高非空 RT 优先级；没有返回 -1。RISC-V 上不用 __builtin_clzll（libgcc）。

**位置**：`kernel/task/sched.c:106`

<a id="fn-kernel-task-sched.c-141"></a>

### `rq_enqueue_locked`

```c
static void rq_enqueue_locked(cpu_t *c, task_t *t)
```

持 rq_lock 调用：按调度类插入就绪队列。

<details>
<summary>函数内备注（1 条）</summary>

**第 145 行**

> 注意：不要把有效优先级写回 rt_priority —— 那是基础优先级，
> PI 提升期间写回会把临时值变成永久值。

</details>

**位置**：`kernel/task/sched.c:141`

<a id="fn-kernel-task-sched.c-157"></a>

### `rq_remove_locked`

```c
static bool rq_remove_locked(cpu_t *c, task_t *t)
```

持 rq_lock 调用：从 NORMAL/RT 队列摘除。

<details>
<summary>函数内备注（1 条）</summary>

**第 171 行**

> policy 在排队期间被改过的兜底（当前不开放运行时改策略）。

</details>

**位置**：`kernel/task/sched.c:157`

<a id="fn-kernel-task-sched.c-184"></a>

### `rq_node_to_task`

```c
static task_t *rq_node_to_task(cpu_t *c, list_node_t *node)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由运行队列节点反推任务指针，越出任务池范围则打印错误并返回 NULL。

**位置**：`kernel/task/sched.c:184`

<a id="fn-kernel-task-sched.c-217"></a>

### `sched_kick_after_enqueue`

```c
static void sched_kick_after_enqueue(cpu_t *c, task_t *t)
```

入队后是否需要立刻抢占目标核。
- 远端：沿用旧行为，总发 resched IPI（唤醒延迟不依赖 tick）；
- 本核：只有更高优先级的 RT 任务（或当前是 idle）才置 need_resched。

本核只置标志还不够：need_resched 只在 trap 返回点 / syscall 返回边界
被消费。正在跑纯计算循环的 NORMAL 内核线程没有任何返回点，只能等
下一次 timer tick（最长一个 tick，默认 10ms；RT_DEMO 在 100Hz 下实测平均 9.9ms）。所以本核在
"真实任务被更高优先级 RT 抢占"时再补一个 self-IPI：中断入口本身
就是 trap，它的返回路径会立刻消费 need_resched，把抢占延迟压到
µs 级（见 docs/realtime/RT_DEMO_RESULTS.md）。

注意：
- idle/boot 上下文不发：idle 不在就绪队列里，它自己的 trap/yield
路径会消费标志；boot 阶段发 IPI 还会提前打断内核初始化。
- 中断上下文里不发：本轮 ISR 的返回路径一定会消费 need_resched，
再发一个 IPI 只是多一次嵌套中断。
- 本核是用户进程时仍然发：用户态需要立即被打断；如果它正在
syscall 内核侧，trap_preempt_kernel_side() 会拒绝在这里切换，
延迟到 syscall 返回边界，代价只是多一次中断。

<details>
<summary>函数内备注（1 条）</summary>

**第 230 行（行尾）**

> NORMAL 任务不打断 NORMAL/RT；等 tick 或返回边界

</details>

**位置**：`kernel/task/sched.c:217`

<a id="fn-kernel-task-sched.c-250"></a>

### `sched_set_pi_boost`

```c
void sched_set_pi_boost(task_t *t, uint8_t prio)
```

sched_set_pi_boost — rt_mutex 优先级继承的调度侧入口

prio == 0 表示清除继承。调用者不得持有 rq_lock。
如果任务当前 READY 且不在 CPU 上，它已经在某个 RT/NORMAL 队列里，
必须在同一把 rq_lock 下先摘除、改优先级、再按新类/新优先级入队；
否则 rq_remove 会按新优先级去旧队列里找，找不到就泄漏一个节点。
RUNNING / BLOCKED 的任务不需要搬队列：后者下次 sched_unblock 时会按
新优先级入队。

**位置**：`kernel/task/sched.c:250`

<a id="fn-kernel-task-sched.c-282"></a>

### `sched_init`

```c
void sched_init(task_t *idle_task)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把本核 idle 任务设为当前任务并标记其正在 CPU 上。

<details>
<summary>函数内备注（1 条）</summary>

**第 284 行**

> run_queue 已由 cpu_init_bsp() 初始化为空链表, 这里只装 idle。

</details>

**位置**：`kernel/task/sched.c:282`

<a id="fn-kernel-task-sched.c-304"></a>

### `sched_resched_cpu`

```c
void sched_resched_cpu(uint32_t cpu_id)
```

sched_resched_cpu — 让目标核立刻重新调度

只置标志 + 发 IPI, 不直接操作目标核的队列：真正的切换由目标核在
「中断返回路径」或「vCPU 主循环顶部」完成。目标正在跑 guest 时,
IPI 会被硬件路由成一次 VM-exit / HS trap, VMM 退出后自然回到
sched_check_and_yield()。

这是「一颗核让另一颗核上的 vCPU 任务停下来」的唯一正确姿势 ——
不要从远端直接改 task->state 或调用 task_exit()。

<details>
<summary>函数内备注（1 条）</summary>

**第 316 行（行尾）**

> 标志必须先于 IPI 对目标核可见

</details>

**位置**：`kernel/task/sched.c:304`

<a id="fn-kernel-task-sched.c-322"></a>

### `task_kick`

```c
void task_kick(task_t *task)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅对运行态且亲和核合法的任务，向其所绑定的 CPU 请求重调度。

**位置**：`kernel/task/sched.c:322`

<a id="fn-kernel-task-sched.c-336"></a>

### `sched_enqueue`

```c
void sched_enqueue(task_t *task)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

为任务选定目标 CPU 并加锁入其运行队列，随后触发重调度，运行中的任务被拒绝入队。

<details>
<summary>函数内备注（4 条）</summary>

**第 338 行**

> 防御：正在运行的任务不在任何队列里, 把它再挂进队列会让它被两个核
> 同时执行(任务栈/状态互相踩)。合法的调用者只会入队 READY 任务
> —— 走到这里说明有人破坏了「谁在哪」的不变量, 宁可漏入队
> (任务下次被唤醒时还会再试)也不要写坏运行队列。

**第 369 行**

> 中断上下文里绝不要调 arch_irq_save()/arch_irq_restore()。
> 那一对是用「整字写回 sstatus」实现关/开中断的；在 ISR 里写回
> sstatus 会破坏它的状态, 实测症状是 S 模式中断从此不再投递 ——
> 定时器停摆, 串口无响应, 被唤醒的任务再也跑不起来。

> 设备 ISR 里唤醒任务是常规操作(收包中断叫醒 net-poll 就是),
> 而中断本来就是关的, 直接插队列即可, 不需要再动 sstatus。

**第 391 行**

> 跨核入队：持目标核 rq_lock。

> 中断上下文里不要用 spin_lock_irqsave：RISC-V 的 arch_irq_save/
> restore 是整字写 sstatus, 在 ISR 里写回会破坏中断状态(见本函数
> 单核分支的注释)。中断本来就已经关了, 用裸 spin_lock 即可。

**第 410 行**

> 远端入队后立刻 kick；本核只在更高优先级 RT 任务入队时置
> need_resched。没有这一步, 任务要等目标核下一次 timer tick
> (最长一个 tick，默认 10ms)才被看到；对 vCPU/RT 的唤醒来说这个延迟不可接受。

</details>

**位置**：`kernel/task/sched.c:336`

<a id="fn-kernel-task-sched.c-436"></a>

### `sched_unblock`

```c
void sched_unblock(task_t *task)
```

sched_unblock — 唤醒 BLOCKED 任务, 且保证「同一任务不会同时在跑又在队列」

旧实现是 `state = READY; sched_enqueue()`。SMP 下有一个真实窗口：
目标任务在另一颗核上刚置了 BLOCKED, 还没执行到 sched_schedule 的切出点；
此时唤醒方把它入队, 第三颗核就可能把它挑走 —— 同一个任务被两颗核执行。

现在的规则：
- on_cpu == true  → 只置 READY + wake_pending, 由它自己的调度器在切出
的那一刻补入队列；
- on_cpu == false → 直接入队；
- 入队到远端核时发 resched IPI。

调用方必须保证目标任务确实在等待(例如已挂在 wait queue 上), 否则
state 检查会让这次唤醒变成 no-op。

<details>
<summary>函数内备注（3 条）</summary>

**第 453 行**

> 中断上下文：本核 IRQ 已关, 不能动 sstatus

**第 471 行（行尾）**

> 记录 wake→run 的起点

**第 474 行**

> 还在目标核上：切出时由 sched_schedule() 补入队列

</details>

**位置**：`kernel/task/sched.c:436`

<a id="fn-kernel-task-sched.c-494"></a>

### `sched_dequeue`

```c
void sched_dequeue(task_t *task)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次在亲和核与所有核的运行队列中加锁尝试摘除该任务。

<details>
<summary>函数内备注（1 条）</summary>

**第 496 行**

> 任务以其 cpu_affinity 为准处于某核 rq。先查 affinity 对应核,
> 找不到再退一步遍历所有核(防 affinity 字段与实际不一致)。

</details>

**位置**：`kernel/task/sched.c:494`

<a id="fn-kernel-task-sched.c-535"></a>

### `kill_task_on_cpu`

```c
static bool kill_task_on_cpu(uint32_t cpu_id, task_t *task)
```

kill_task_on_cpu - 在指定核的 rq_lock 下发现并终结 task

返回 true 表示找到了它的调度上下文（正在该核运行，或在该核就绪队列里）。

**位置**：`kernel/task/sched.c:535`

<a id="fn-kernel-task-sched.c-569"></a>

### `sched_kill_task`

```c
void sched_kill_task(task_t *task)
```

sched_kill_task - 从远端把一个任务标成 DEAD 并移出调度队列

exit_group 终止兄弟线程时用。不能只写 state = DEAD 再 sched_dequeue：
目标任务可能正在自己的 sched_schedule() 里，后者会在 rq_lock 下把
state 从 DEAD 覆盖回 READY/RUNNING，任务就活下来了（LTP exit_group01
的偶发失败：父进程已 wait 到 group leader，但某个兄弟线程还在跑）。

这里持目标核的 rq_lock 完成「摘链 + 置 DEAD」，和目标任务的
sched_schedule()/sched_unblock() 对 state 的写入串行化。正在目标核上
运行的任务还会收到一次 resched IPI，尽快在 trap 返回/调度点停下。

只管停止任务，不负责回收 pgd/内核栈 —— 回收仍走 task_exit 的
switch_done 路径（见 docs/bugfix/SMP_TASK_EXIT_STACK_REUSE.md）。

<details>
<summary>函数内备注（3 条）</summary>

**第 577 行**

> 先按 affinity 找。入队时 CPU_AFFINITY_ANY 已经被固定成实际 CPU。

**第 581 行**

> 兜底：affinity 可能刚被 task_set_cpu_affinity 改过，扫一遍所有核。

**第 589 行**

> 不在 current_task、也不在任何就绪队列上：多半是 BLOCKED。用和
> sched_unblock() 相同的锁（目标核 rq_lock）写 DEAD，防止并发唤醒把
> state 覆盖成 READY。

</details>

**位置**：`kernel/task/sched.c:569`

<a id="fn-kernel-task-sched.c-603"></a>

### `pick_next`

```c
static task_t *pick_next(cpu_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

优先取最高优先级 RT 队列任务并维护位图，否则取普通队列首个，队空返回 idle。

<details>
<summary>函数内备注（3 条）</summary>

**第 605 行**

> 1. 最高优先级的 RT 队列

**第 615 行**

> 2. NORMAL 轮转队列

**第 621 行（行尾）**

> 队列为空, 回退到本核 idle

</details>

**位置**：`kernel/task/sched.c:603`

<a id="fn-kernel-task-sched.c-626"></a>

### `sched_schedule`

```c
void sched_schedule(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

关中断后重新入队当前任务并选出下一个，处理唤醒窗口后切换页表与上下文，最后恢复中断。

<details>
<summary>函数内备注（27 条）</summary>

**第 630 行**

> 关中断, 保存当前中断标志

**第 633 行**

> 缓存本核指针：irq_save 之后 cpu 不会变, 避免重复读 per-CPU 寄存器。

**第 636 行**

> prev 一律从 per-CPU 的 c-&gt;current_task 取。

**第 639 行**

> 持本核 rq_lock 期间操作 run_queue。外层已 arch_irq_save 关本核 IRQ,
> 这里用不带 irqsave 的 spin_lock —— 中断状态由外层管理, 不能用
> spin_unlock_irqrestore 提前把 IRQ 打开。
> 跨核 sched_enqueue 持的是同一把锁, 所以 list 操作原子。
> (以前这里要把 spinlock_noirq_t* 强转成 spinlock_t* 才能用 raw 变体；
> 锁类型收敛后不需要了。)

**第 650 行**

> 若当前任务仍在运行且不是 idle, 则按调度类重新入队

**第 659 行**

> 防御 exit_group：sched_kill_task() 会在目标核 rq_lock 下把 state
> 置成 DEAD。如果被标 DEAD 的任务恰好又被 pick_next 选回来，绝不能
> 走下面的&quot;无需切换&quot;分支把它写回 RUNNING —— 那会让兄弟线程继续跑，
> 正是 LTP exit_group01 的偶发失败。强制切到 idle。

**第 668 行**

> 切出 prev 之前收口「on_cpu 期间被唤醒」的窗口。

> waker 看到 on_cpu=true 时只置 wake_pending, 不会入队(否则任务会
> 被第二颗核抢走)。这里在 rq_lock 下把 on_cpu 清掉, 并兑现那次唤醒。
> 之后 waker 再看到的就是 on_cpu=false, 会直接入队 —— 两边持同一把
> 锁, 不存在「清了 on_cpu 但没人入队」的缝隙。

**第 680 行**

> DEAD 任务不能被这次&quot;补入队&quot;复活：waker 可能在 exit_group
> 标 DEAD 之前就看到了 on_cpu=true 并留下 wake_pending。

**第 687 行**

> 这次唤醒是在 prev 还 on_cpu 时发生的, waker 没有发 IPI
> (`sched_unblock` 的 enqueued=false 分支)。现在 prev 已经
> 入队, 但接下来要切到的可能是 idle —— 不置 need_resched 的话
> idle 会直接 wfi/hlt, 任务要等下一次 tick 才被看到；tick 若
> 异常就是永久睡死。本核自己入队, 不需要 IPI。

**第 698 行**

> state/on_cpu 也在同一把锁下发布, 避免 waker 看到中间态

**第 701 行**

> 迁移判定：任务上次在另一颗 CPU 上运行。首次运行不算迁移；
> 任务在 enqueue 后由 task_set_cpu_affinity 改核时，这里会记一次。

**第 707 行**

> 任务每次被切入时清掉上一次切出的完成标记。退出路径的
> switch_done 只在任务真正下 CPU 后由 arch_task_switch 置 1，
> 所以不能残留上一轮的 1。

**第 714 行**

> 无需切换时也要在 rq_lock 内恢复 RUNNING：sched_kill_task() 用
> 同一把锁串行化 state 写入；放到锁外会留下「锁内判完、锁外写回
> RUNNING」的窗口，把刚标 DEAD 的任务复活。

**第 728 行**

> 无需切换(唯一任务或队空只有 idle)

**第 734 行（行尾）**

> 确保 state 在 current_task 之前完成

**第 735 行（行尾）**

> per-CPU 主存储

**第 737 行（行尾）**

> 确保 current_task 在 arch_task_switch 之前完成

**第 740 行**

> x86_64：通过 CR3 切换页表
> - 用户任务：切换到用户页表(已包含内核高半区映射)
> - 内核任务：切换回内核页表

**第 745 行**

> 切换到用户页表

**第 748 行**

> 更新 TSS.RSP0 为当前任务的内核栈顶

**第 752 行**

> 恢复该用户任务的 TLS 基址(fs:offset)

**第 756 行**

> 从用户任务切换到内核任务：恢复内核页表

**第 759 行**

> 用户→用户切换, 已在上面处理；内核→内核切换, 页表不变

**第 762 行**

> 切换上下文。
> 对 prev：保存被调用者寄存器 + SP 到 prev-&gt;sp, 然后跳走。
> 当 prev 再次被调度时, arch_task_switch 从这里&quot;返回&quot;。
> 此时 flags 在 prev 的栈帧中, 中断仍关闭。

> 同时切换页表(如果任务有独立页表)。

**第 772 行**

> 注意：之前 AArch64 使用 sp=-1 标记切换到 idle, 但这导致
> .Lswitch_to_idle 没有设置栈指针, 造成潜在的栈溢出问题。
> 现在所有架构统一：idle 有专用栈, 正常返回即可。

**第 777 行**

> 页表参数由架构决定(RISC-V 的首次用户进入/内核任务特例在 arch 实现里)

**第 782 行**

> prev 被恢复后恢复其中断状态

</details>

**位置**：`kernel/task/sched.c:626`

<a id="fn-kernel-task-sched.c-788"></a>

### `sched_tick`

```c
void sched_tick(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按调度策略决定是否置重调度标志（idle 与普通置位、RR 扣时间片、FIFO 与提升者不抢占），并做 idle 记账与 itimer 检查。

<details>
<summary>函数内备注（4 条）</summary>

**第 790 行**

> 由 timer ISR 调用。
> 只设置需要重调度的标志, 实际切换在异常返回前进行。
> 这样可以避免在 IRQ 处理程序中直接切换上下文。

> 抢占语义按调度类区分：
> - NORMAL / idle：tick 触发轮转；
> - FIFO：tick 不抢占（只有更高优先级 RT 入队/阻塞事件才切）；
> - RR：时间片用完才置 need_resched。

**第 808 行**

> 被 PI 提升的持有者按 FIFO 对待：tick 不能让中等优先级任务
> 抢走它，否则优先级继承白做了。

**第 819 行**

> idle 记账。挂在 tick 上而不是各个 idle 循环里：g_tick_cb 每个核都会调,
> 一处就覆盖了 task_idle_loop 和三个架构各自的次级核 idle 循环。
> /proc/uptime 的第二列靠它, 否则永远是 0(top/vmstat 会当成&quot;从不空闲&quot;)。

**第 827 行**

> 每任务的 ITIMER_REAL 到期检查也挂在这个 tick 上(周期由 timer.tick_ms
> 配置, 默认 10ms)。放在这里而不是另开一个回调：g_tick_cb 只有一个,
> 直接接在 sched_tick 后面即可。注意 sched_tick 是 per-CPU 回调，
> 多核下 itimer_tick 会被每个核各调一次(限制见 kernel/time/itimer.c)。

</details>

**位置**：`kernel/task/sched.c:788`

<a id="fn-kernel-task-sched.c-843"></a>

### `sched_check_and_yield`

```c
bool sched_check_and_yield(void)
```

由异常返回路径、AP idle 循环和 vCPU 主循环调用, 检查是否需要重新调度。
如果需要, 执行任务切换。
返回 true 表示发生了切换, false 表示没有。

<details>
<summary>函数内备注（1 条）</summary>

**第 849 行**

> 注意：不再限制仅用户进程可被抢占。idle 任务(is_user_process=false)
> 也需要被 timer 抢占, 以便在就绪队列中出现任务时立即切换。
> 若 pick_next() 返回 idle 自身, sched_schedule() 会跳过切换。

</details>

**位置**：`kernel/task/sched.c:843`

<a id="fn-kernel-task-sched.c-875"></a>

### `trap_preempt_kernel_side`

```c
static bool trap_preempt_kernel_side(void)
```

trap_preempt_kernel_side - 从【内核态】陷入时的抢占判断

三个架构共用。用户进程的内核侧(syscall / 缺页处理, 跑在它自己的内核栈上)
不在这里切换：那会让任意 syscall 代码点被交错执行, 产生大面积数据竞争；
它的抢占推迟到 syscall 返回边界。内核线程(含 idle)没有这个问题, 可以在
这里被 timer 抢占 —— 否则纯内核态的循环会独占 CPU。

preemptible() 内含 irq_depth == 0 判断, 嵌套中断里不会误切。

**位置**：`kernel/task/sched.c:875`

<a id="fn-kernel-task-sched.c-907"></a>

### `sched_check_and_yield_from_trap`

```c
bool sched_check_and_yield_from_trap(void *frame_ptr)
```

sched_check_and_yield_from_trap - trap 返回前的调度检查

三个架构的异常/中断返回路径都只调这一个函数, 所以这里就是【每个架构唯一的
timer 抢占点】。frame_ptr 用来判断这次 trap 来自用户态还是内核态：

AArch64   spsr[3:0] == 0 (EL0t)   → 用户态；!= 0 (EL1h) → 内核态
x86_64    CS.RPL     == 3        → 用户态
RISC-V    sstatus.SPP == 0       → 用户态

来自用户态时直接 sched_check_and_yield()：被中断的是用户代码, 异常现场已
完整保存在 trap frame 里, 在它自己的内核栈上切换没有任何副作用。

注意：调用本函数时 handle_exception 已经做过 irq_depth--, 因此
sched_schedule() 开头的 assert_always(!in_irq_context()) 不会触发。

<details>
<summary>函数内备注（1 条）</summary>

**第 912 行**

> 来自用户态：异常现场完整保存在 trap frame 里, 直接调度即可

</details>

**位置**：`kernel/task/sched.c:907`

<a id="mod-kernel-task-task.c"></a>

## kernel/task/task.c — 内核任务生命周期管理

`kernel/task/task.c` · 32 个函数 · 32 个有说明 · 1073 行

提供：
task_init()    — 初始化任务子系统(boot 上下文 → idle 任务)
task_create()  — 从静态池分配任务并加入调度队列
task_yield()   — 主动让出 CPU
task_exit()    — 终止当前任务
task_current() — 返回当前运行任务
task_trampoline() — 新任务的启动入口(汇编通过 ret 跳转到此)

<a id="fn-kernel-task-task.c-67"></a>

### `task_alloc_id`

```c
uint32_t task_alloc_id(void)
```

任务 id 分配。原来是无保护的 `g_task_id_cnt++`, 两核同时 fork 会撞 id。

**位置**：`kernel/task/task.c:67`

<a id="fn-kernel-task-task.c-85"></a>

### `slot_is_reapable_orphan_locked`

```c
static bool slot_is_reapable_orphan_locked(uint32_t idx)
```

slot_is_reapable_orphan_locked - 持锁调用：这个 DEAD 槽现在能收吗？

光看 state == TASK_DEAD 是不够的 —— 僵尸要留给父进程 wait4 取退出码,
谁都能收就把退出码丢了(实测：父进程 waitpid 自己的直接子进程拿到
ECHILD, SMP=1 下都有 8%)。只有"没人会再来 wait 它"的才收：

- parent_id == 0：压根没有父进程(idle / 内核线程)
- 父进程已经不在了：槽被释放, 或父进程自己也 DEAD

其余情况等它爹来收。这也顺带修掉 fork 路径那个"回收任意 DEAD 槽"的
循环 —— 它让 A 进程的 fork 把 B 进程还没 wait 的僵尸收走。

<details>
<summary>函数内备注（3 条）</summary>

**第 90 行**

> DEAD 只代表“不在调度队列里”，不代表已经离开自己的内核栈/页表/TCB。
> 等 arch_task_switch 把 switch_done 置 1 之后才能收。没置位时先跳过，
> 以后某次 task_alloc_slot() 再看 —— 不能在这里等，调用方持有池锁。

**第 103 行**

> 找到父进程：它只要还活着(非 DEAD), 就轮不到我们收

**第 106 行（行尾）**

> 父进程的槽已经没了

</details>

**位置**：`kernel/task/task.c:85`

<a id="fn-kernel-task-task.c-115"></a>

### `slot_take_and_free`

```c
static void slot_take_and_free(uint32_t idx)
```

slot_take_and_free - 收掉一个已经被占住的槽(state == TASK_ALLOCATING)

调用者必须已经在锁内把它标成 TASK_ALLOCATING(占位),
这里在锁外做重活, 最后回锁内释放槽位。

<details>
<summary>函数内备注（1 条）</summary>

**第 123 行**

> 锁外释放进程级对象：signal_struct 可能被线程组共享；mm 的最后一个
> 引用会销毁用户页表（几百页，不能占着全池的锁）。

</details>

**位置**：`kernel/task/task.c:115`

<a id="fn-kernel-task-task.c-145"></a>

### `task_alloc_slot`

```c
task_t *task_alloc_slot(void)
```

task_alloc_slot - 认领一个空闲任务槽(已清零, 栈已刷图案)

fork 路径也走这里(以前它内联抄了一份自己的循环, 于是漏掉了
cpu_affinity / preempt_count 的初始化 —— 那两个字段是复用的 TCB 里
上一个占位者的残值, preempt_count 非 0 会让新任务永久不可抢占)。
清零的代价(sizeof(task_t) + 16KB 栈)相比 fork 要拷的几百页可以忽略。

<details>
<summary>函数内备注（16 条）</summary>

**第 147 行**

> 先试着回收&quot;没人会来 wait&quot;的僵尸槽

**第 155 行（行尾）**

> 占住

**第 176 行**

> 槽已归本调用者, 锁外清零

**第 180 行**

> 0 是合法 CPU；用哨兵区分“从未运行过”，避免首次运行被算成迁移。

**第 183 行**

> Linux 进程属性的默认值。clone_handler 会把父进程的
> umask/groups/rlimits/affinity 覆盖成继承值。

**第 190 行（行尾）**

> RLIM_INFINITY

**第 193 行**

> 与 Linux 常见默认值一致；LTP 的 rlimit 用例依赖 NOFILE 的具体值

**第 194 行（行尾）**

> RLIMIT_STACK

**第 195 行（行尾）**

> RLIMIT_CORE

**第 196 行（行尾）**

> RLIMIT_NOFILE

**第 197 行（行尾）**

> 无 /proc/sys/fs/nr_open 时的上界

**第 198 行（行尾）**

> RLIMIT_NICE

**第 200 行（行尾）**

> RLIMIT_RTPRIO

**第 202 行**

> 把整块栈刷成已知图案：配合 task_stack_used() 量&quot;这个任务实际用掉
> 多少栈&quot;。arch_init_task_stack() 稍后会在栈顶写好初始帧, 其余部分
> 保持图案 —— 高水位就是&quot;从栈底往上第一处非图案的位置&quot;。

> 为什么值得留：内核任务栈是静态数组挨着放的(g_task_stacks[N][...]),
> 溢出会直接踩坏邻居任务的栈/TCB, 症状是&quot;隔壁任务莫名其妙崩&quot;,
> 而不是栈溢出的任务自己崩 —— 实测宿主 busybox 就是这么被写坏的
> (见 vcpu_task_fn 里那段说明)。有数字才能定 size, 不然只能靠翻倍赌。

**第 213 行**

> 进程级对象（fd 表 / cwd / mm / signal 由 task_signal_* 懒分配）。
> 静态池大小 = TASK_MAX + AVATAR_MAX_CPUS，认领到一个任务槽就
> 必然有一个对象槽；失败只可能是池管理 bug，按 ENOMEM 处理。

**第 226 行**

> 默认 affinity = ANY：让 sched_enqueue round-robin 分发到所有核。
> 调用方(如 vcpu_task_create)可在 enqueue 前覆盖。

</details>

**位置**：`kernel/task/task.c:145`

<a id="fn-kernel-task-task.c-243"></a>

### `task_reap_dead`

```c
void task_reap_dead(task_t *task)
```

task_reap_dead - 回收一个已退出任务的资源

调用者(wait4 / exec wrapper)已经读过它的退出码, 所以这里不做
"父进程还在不在"的判断 —— 但会防重入：先把 state 从 DEAD 改成
ALLOCATING 占住, 并发的第二个调用者就进不来了。

<details>
<summary>函数内备注（4 条）</summary>

**第 257 行（行尾）**

> 别人已经收过了

**第 259 行**

> DEAD 之后、arch_task_switch 完成之前，本任务还可能在使用
> 自己的 pgd / 内核栈 / TCB。必须等 switch_done（release store）
> 再销毁资源；否则 wait4 会把还在运行的退出任务连页表一起拆掉。

> 调用者是 wait4 / exec 包装进程，不在 IRQ 上下文，可以 yield。
> 等待期间任务 id 若变了说明槽已经被别人回收并复用（ABA），直接退出。

**第 276 行（行尾）**

> 占住, 防双重释放

**第 280 行（行尾）**

> 等的时候被别人收掉/复用了

</details>

**位置**：`kernel/task/task.c:243`

<a id="fn-kernel-task-task.c-291"></a>

### `task_stack_used`

```c
size_t task_stack_used(const task_t *task)
```

task_stack_used — 该任务栈的高水位(字节, 含初始帧)

从栈底往上找第一处非 TASK_STACK_MAGIC 的字节, 它到栈顶的距离就是峰值用量。
任务还没跑过时返回 0。只用于诊断, 不要在热路径里调(O(栈大小))。

**位置**：`kernel/task/task.c:291`

<a id="fn-kernel-task-task.c-306"></a>

### `free_task_slot`

```c
static void free_task_slot(task_t *task)
```

创建失败时的回滚：把刚认领的槽还回去。同样要走槽表的锁,
否则和并发认领者之间又是一个"两个人都以为槽是空的"窗口。

<details>
<summary>函数内备注（1 条）</summary>

**第 308 行**

> 先把 files/fs/mm/signal 引用还掉；mm 销毁页表必须在池锁外。

</details>

**位置**：`kernel/task/task.c:306`

<a id="fn-kernel-task-task.c-326"></a>

### `task_objects_alloc`

```c
bool task_objects_alloc(task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次分配 files、fs、mm，任一失败就回滚已分配对象并返回 false。

**位置**：`kernel/task/task.c:326`

<a id="fn-kernel-task-task.c-340"></a>

### `task_release_objects`

```c
void task_release_objects(task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

释放信号结构，并对 files、fs、mm 逐个减引用后清空指针。

<details>
<summary>函数内备注（1 条）</summary>

**第 345 行**

> signal_struct 可能被线程组共享，只有最后一个引用归还池槽位。

</details>

**位置**：`kernel/task/task.c:340`

<a id="fn-kernel-task-task.c-382"></a>

### `task_trampoline`

```c
void task_trampoline(void)
```

task_trampoline - 内核线程首次被调度时的落地点

新内核线程的栈帧由 arch_init_task_stack 伪造, x30(LR)=task_trampoline,
首次被调度时 arch_task_switch 的 "ret" 跳到这里。

触发来源：其他任务主动 task_yield(), 或 timer 中断返回路径上
sched_check_and_yield() 触发重调度。注意切换不在 ISR 内进行——
sched_schedule() 开头断言 !in_irq_context()。

为何必须在此显式开中断：内核线程全程在 EL1 运行, 需开中断以可被
timer 抢占(否则会独占 CPU, 饿死其他任务)。而首次运行不经过
sched_schedule() 末尾的 arch_irq_restore()——前驱在 arch_irq_save()
里替它关了中断, 却没有人替它开。因此这里必须 arch_irq_enable()。

对比：EL0 任务(用户进程)走 task_trampoline_user / arch_fork_resume_user,
那里不显式开中断——中断由 eret 跨入 EL0 时按 SPSR 恢复。区别在于
本函数的 entry() 在 EL1 运行, 而那两者最终下到 EL0。

<details>
<summary>函数内备注（1 条）</summary>

**第 391 行**

> entry 函数返回时自动退出

</details>

**位置**：`kernel/task/task.c:382`

<a id="fn-kernel-task-task.c-408"></a>

### `task_init`

```c
void task_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

重置任务池并初始化 idle 任务、保存内核页表基址后启动调度，同时启用可睡眠 fs 锁与异步日志。

<details>
<summary>函数内备注（8 条）</summary>

**第 410 行**

> 清空静态池

**第 416 行**

> 初始化 idle 任务(boot 上下文, 使用 idle 专栈)

**第 420 行（行尾）**

> 最低优先级

**第 425 行**

> idle 也要一份空的 files/fs/mm（内核任务没有用户 pgd）。

**第 430 行**

> 手动 strcpy(避免依赖外部库)

**第 437 行**

> 初始化调度器, 传入 idle 任务

**第 440 行**

> fs_init 阶段已经过去；之后 ext4/FatFs 调用可以睡眠等待全局锁，
> 不再整段关本核 IRQ。

**第 446 行**

> 启动异步日志线程；此后 klog_write 不再整段持 IRQ-off 写 UART。

</details>

**位置**：`kernel/task/task.c:408`

<a id="fn-kernel-task-task.c-462"></a>

### `task_switch_to_idle_stack`

```c
void task_switch_to_idle_stack(void)
```

task_switch_to_idle_stack - 切换到 idle 专用栈

必须在 task_init() 返回后, 进入 idle 循环前调用。
在 task_init() 内部切换会导致返回地址丢失。

x86_64 特殊处理：需要保存返回地址和帧指针, 然后在新栈上恢复,
否则函数返回时会出现 General Protection Fault。

<details>
<summary>函数内备注（1 条）</summary>

**第 467 行**

> 切到 idle 栈后永不返回：架构实现直接在 inline asm 里跳到 idle
> 主循环, 跳过编译器生成的 epilogue(否则会从未初始化的新栈读
> LR/RBP 导致跳到 0)。实现见 include/arch/&lt;arch&gt;/cpu_impl.h。

</details>

**位置**：`kernel/task/task.c:462`

<a id="fn-kernel-task-task.c-482"></a>

### `task_idle_loop`

```c
__attribute__((noreturn)) void task_idle_loop(void)
```

task_idle_loop - idle 主循环

由 task_switch_to_idle_stack 在切栈后直接跳入, 永不返回。
启用中断后循环执行 task_yield + 体系结构等待指令。

<details>
<summary>函数内备注（1 条）</summary>

**第 484 行**

> idle 是内核线程, 全程在 EL1, 开中断(既为可被抢占, 也因 wfi
> 需中断唤醒)。同 task_trampoline：首次进入不经过 arch_irq_restore,
> 故在此显式开。

</details>

**位置**：`kernel/task/task.c:482`

<a id="fn-kernel-task-task.c-502"></a>

### `task_set_cpu_affinity`

```c
void task_set_cpu_affinity(task_t *task, uint32_t cpu_id)
```

── task_set_cpu_affinity ───────────────────────────────────
把 task 移到指定 cpu 的运行队列。供 vcpu/EL2 等不能跨核迁移的
任务在 enqueue 后立即重新绑定。

假设：task 当前已经被 sched_enqueue 过(在某个核的 rq 中)。
若尚未入队(READY 之前), 可直接设置 cpu_affinity 字段后再 enqueue。

**位置**：`kernel/task/task.c:502`

<a id="fn-kernel-task-task.c-523"></a>

### `process_create`

```c
task_t *process_create(const char *name, uint64_t user_entry, uint64_t user_code_size, uint64_t user_sp, uint8_t priority)
```

process_create - 创建用户进程
@name:       进程名称
@user_entry: 用户态入口点(虚拟地址)
@user_sp:    用户栈指针(虚拟地址)
@priority:   优先级

创建独立地址空间的用户进程。

<details>
<summary>函数内备注（6 条）</summary>

**第 540 行**

> 标记为用户进程

**第 551 行**

> 创建独立的用户页表；内核映射注入在 vm_create_user_process 内部完成

**第 566 行**

> 复制任务名称

**第 574 行**

> 使用用户进程专用的栈初始化函数(注意：使用调整后的虚拟地址)

**第 578 行**

> 初始化完成后才发布为 READY, 避免查找/信号路径看到半初始化 TCB。

**第 581 行**

> 加入就绪队列

</details>

**位置**：`kernel/task/task.c:523`

<a id="fn-kernel-task-task.c-600"></a>

### `process_create_with_pgd`

```c
task_t *process_create_with_pgd(const char *name, uint64_t user_entry, uint64_t user_sp, uint8_t priority, uint64_t pgd_phys, uint64_t heap_end_val, uint64_t mmap_next_val)
```

process_create_with_pgd - 使用调用方已构建好的用户页表创建进程

跳过 vm_create_user_process, 直接使用 pgd_phys。
适用于 ELF 加载器：加载器已自行完成段映射和栈映射,
不需要再做一次拷贝。user_entry 直接作为用户虚拟入口, 不做转换。

<details>
<summary>函数内备注（8 条）</summary>

**第 627 行**

> 初始化进程文件系统相关字段(继承父进程 cwd)

**第 657 行**

> === 信号字段初始化 ===

**第 661 行（行尾）**

> 默认：自成一组

**第 662 行（行尾）**

> 默认：自成会话

**第 665 行**

> task-&gt;sig == NULL 表示全部 SIG_DFL，无需初始化 2 KiB 的 action 表。

**第 677 行**

> 在入队之前设置堆和 mmap 地址, 避免调度器过早切换到该任务时看到 0

**第 680 行**

> 初值即基址：exec 传进来的 mmap_next_val 就是本进程 mmap 区的起点
> (task_execve 里算出来的 mmap_base)。fork 拷贝地址空间时用它当
> 扫描下界, 见 task.h 里 mmap_base 的注释。

**第 690 行**

> 这里故意不入队：调用方(execve)在新任务创建之后还要填 fd 表,
> pgid/sid/uid, 信号掩码等状态, 而 TCB 里 fd_table 的初值是 -1
> —— 那意味着 fd 0/1/2 退回控制台。如果在入队之后才填, SMP 下别的核
> 可能抢先把这个新任务调度上去, 让它带着未初始化的状态跑。

> 实测症状(`ls /bin | grep bin` 这类管道)：新任务抢先跑起来时
> fd 0/1/2 还是控制台, 于是 ls 把目录列表直接打到串口, grep 去读控制台
> 并永久阻塞, shell 卡死在 wait 上 —— 表现为&quot;管道没生效 / 挂死&quot;。
> 上面提前设 heap_end/mmap_next 是同一个道理, 只是那次只管了这两个字段。
> 状态齐了由调用方调 process_start() 入队。

</details>

**位置**：`kernel/task/task.c:600`

<a id="fn-kernel-task-task.c-715"></a>

### `process_start`

```c
void process_start(task_t *task)
```

process_start - 把 process_create_with_pgd 建好的进程挂进运行队列

与 process_create_with_pgd 配对使用：创建时任务停在 TASK_ALLOCATING,
调用方把"必须在新任务跑起来之前就位"的状态全部填完, 再调这里放行。
中途失败想放弃这个任务就用 task_reap_dead()/free_task_slot() 收尾。

**位置**：`kernel/task/task.c:715`

<a id="fn-kernel-task-task.c-725"></a>

### `task_create`

```c
task_t *task_create(const char *name, void (*entry)(void *), void *arg, uint8_t priority)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 CPU_AFFINITY_ANY 为亲和性转调 task_create_affinity。

**位置**：`kernel/task/task.c:725`

<a id="fn-kernel-task-task.c-748"></a>

### `task_rt_prio_clamp`

```c
static uint8_t task_rt_prio_clamp(uint8_t prio)
```

task_create_internal - 所有内核域任务创建的公共实现
（task_create_affinity / task_create_kernel_rt / task_create_vcpu 的底座）

`cpu_affinity` 必须在入队前指定，原因如下：
先 task_create()(内部已经 sched_enqueue, round-robin 可能挑中别的核)
再 task_set_cpu_affinity() 有窗口 —— 如果新任务在那两步之间已经被目标核
挑走开始执行, sched_dequeue() 就找不到它(RUNNING 的任务不在任何队列里),
紧接着 sched_enqueue() 又把它挂到指定核的队列上, 于是同一个任务
既在他核上运行, 又躺在指定核的运行队列里 —— 会被两个核同时执行,
任务状态与运行队列双双被写坏。

窗口对两种落点都存在, 只是后果不同：落在他核 = 同一任务被两个核同时跑；
落在目标核 = 同一任务"正在跑"又"躺在自己核的队列里", 之后会被重复调度。
实测症状(SMP=2, helper 模式下 Ctrl+] 停 guest 再重启, 约 1/2 命中)：
CPU exception #14 at RIP=sched_schedule+0xf2  (next->state = TASK_RUNNING)
CR2 = 0xffffffffffffffc8                      ← pick_next() 拿到野指针

**位置**：`kernel/task/task.c:748`

<a id="fn-kernel-task-task.c-757"></a>

### `task_create_internal`

```c
static task_t *task_create_internal(const char *name, void (*entry)(void *), void *arg, uint8_t priority, uint32_t cpu_affinity, task_domain_t domain, task_sched_policy_t policy, uint8_t rt_priority)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

申请任务槽并初始化 TCB、文件表与工作目录，构建初始栈帧后置 READY 并入队调度。

<details>
<summary>函数内备注（5 条）</summary>

**第 799 行**

> 复制任务名称(最多 TASK_NAME_LEN-1 字节)

**第 807 行**

> 在任务栈上构造初始切换帧, 使首次调度跳到 task_trampoline

**第 810 行**

> 初始化完成后才发布为 READY, 避免查找路径看到半初始化 TCB。

**第 813 行**

> 入队之前定好核(见本函数上方注释：入队后再改有窗口)

**第 816 行**

> 加入就绪队列, 等待调度

</details>

**位置**：`kernel/task/task.c:757`

<a id="fn-kernel-task-task.c-824"></a>

### `task_create_affinity`

```c
task_t *task_create_affinity(const char *name, void (*entry)(void *), void *arg, uint8_t priority, uint32_t cpu_affinity)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以内核域与 NORMAL 策略转调内部创建函数，并传入指定亲和 CPU。

**位置**：`kernel/task/task.c:824`

<a id="fn-kernel-task-task.c-830"></a>

### `task_create_kernel_rt`

```c
task_t *task_create_kernel_rt(const char *name, void (*entry)(void *), void *arg, uint8_t rt_priority, uint32_t cpu_affinity)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

创建内核域 FIFO 实时任务，把 rt_priority 作为实时优先级传入内部创建函数。

**位置**：`kernel/task/task.c:830`

<a id="fn-kernel-task-task.c-836"></a>

### `task_create_vcpu`

```c
task_t *task_create_vcpu(const char *name, void (*entry)(void *), void *arg, uint8_t priority, uint32_t cpu_affinity)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 VCPU 域与 NORMAL 策略创建任务，优先级由参数给出。

**位置**：`kernel/task/task.c:836`

<a id="fn-kernel-task-task.c-843"></a>

### `task_yield`

```c
void task_yield(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

暂停 syscall 计时后主动让出 CPU，返回后再重新开始计时。

<details>
<summary>函数内备注（1 条）</summary>

**第 847 行**

> 主动 yield 的这段时间不算 syscall 内核执行时长（与 task_block 一致）。
> 否则 tty 输入这类“syscall 里循环 task_yield 等待数据”会把等待时间
> 算进 syscall 延迟，/proc/rtstat 的 syscall_max 会被输入等待淹没。

</details>

**位置**：`kernel/task/task.c:843`

<a id="fn-kernel-task-task.c-884"></a>

### `task_exit`

```c
void task_exit(void)
```

退出流程跑在任务自己的内核栈上，不换栈。

“已经 DEAD” ≠ “已经离开内核栈/页表/TCB”：
notify 父进程之后，父进程可能立刻在另一颗核上 wait4 回收本任务。
此时本任务还要执行 sched_dequeue()/sched_schedule()，arch_task_switch
也还要在它的栈上保存上下文、并从它的页表切走。如果回收路径这时就
vm_destroy_user_process() + 复用栈槽/TCB，就会踩到正在使用的资源。

所以回收路径（task_reap_dead / 孤儿扫描）必须等 switch_done 置 1。
switch_done 由 arch_task_switch 在**真正换到 next 的栈和页表之后**写入
（release 语义），见 task.h / switch.h。

这与 Linux 的 do_exit → schedule → finish_task_switch 是同一个思路：
“逻辑死亡”和“栈/TCB 可以回收”是两个时刻。Linux 用 task_struct 引用
计数 + RCU 异步完成；本内核规模小，用一次性同步等待就够了。

<details>
<summary>函数内备注（5 条）</summary>

**第 889 行**

> 置 DEAD 要在槽表的锁里做：否则会有一个窗口 —— 别的核正好在
> task_alloc_slot 的孤儿回收扫描里读这个槽, 看到&quot;父进程还活着&quot;,
> 于是不收；而本任务马上就要变成 DEAD 的父进程。反过来也一样。
> 拿锁之后, &quot;谁是死是活&quot;和&quot;能不能收&quot;就是同一个瞬间的判定了。

**第 902 行**

> Notify waiting parent

**第 906 行**

> 从就绪队列移除(理论上已不在队列, 因为 RUNNING 时会重入队,
> 但 sched_dequeue 做了安全检查)。

**第 912 行**

> 切换出去。sched_schedule 在 arch_task_switch 里把 switch_done 置 1
> 之后，本任务不会再被调度，父进程才可以安全回收。

**第 918 行**

> 不应到达这里

</details>

**位置**：`kernel/task/task.c:884`

<a id="fn-kernel-task-task.c-925"></a>

### `task_current`

```c
task_t *task_current(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回当前 CPU 上正在运行的任务指针。

<details>
<summary>函数内备注（1 条）</summary>

**第 927 行**

> 从 per-CPU 控制块读取, AP 上才能拿到正确的任务。
> cpu_current() 内部已有 NULL 回退到 g_cpus[0], 因此在 cpu_init_bsp
> 之前的极早期路径也是安全的(会返回 g_cpus[0].current_task = NULL)。

</details>

**位置**：`kernel/task/task.c:925`

<a id="fn-kernel-task-task.c-937"></a>

### `task_block`

```c
void task_block(list_t *wait_queue)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

断言非中断上下文，把当前任务置阻塞并挂入等待队列后调度让出，返回后恢复 syscall 计时。

<details>
<summary>函数内备注（6 条）</summary>

**第 942 行**

> 参数验证：wait_queue 必须是内核地址或 NULL

**第 949 行**

> 暂停 syscall stime 计时(阻塞期间不计入 stime)

**第 955 行**

> 设置阻塞状态

**第 958 行**

> 如果提供了等待队列, 将任务加入

**第 963 行**

> 触发调度, 切换到其他任务

**第 966 行**

> 恢复 stime 计时(从被唤醒时刻起重新开始)

</details>

**位置**：`kernel/task/task.c:937`

<a id="fn-kernel-task-task.c-973"></a>

### `task_unblock`

```c
void task_unblock(task_t *task)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

转发给 sched_unblock 完成带 on_cpu 判断的唤醒。

<details>
<summary>函数内备注（1 条）</summary>

**第 975 行**

> 真正的唤醒逻辑在 sched_unblock()：它要在目标核 rq_lock 下判断
> on_cpu, 避免把「还在这颗核上跑, 只是已经置了 BLOCKED」的任务
> 同时挂进运行队列(SMP 下会被另一颗核重复执行)。

</details>

**位置**：`kernel/task/task.c:973`

<a id="fn-kernel-task-task.c-987"></a>

### `task_find_by_id`

```c
task_t *task_find_by_id(uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历任务池，返回 id 匹配且已使用、非死亡或分配中状态的任务。

**位置**：`kernel/task/task.c:987`

<a id="fn-kernel-task-task.c-998"></a>

### `task_send_signal`

```c
void task_send_signal(task_t *t, int sig)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 pending_sigs 中置对应信号位，若任务正阻塞在睡眠上则唤醒它。

<details>
<summary>函数内备注（1 条）</summary>

**第 1004 行**

> nanosleep 的睡眠点在 task_block() 里，只有信号能让它提前醒。
> 默认动作通常是终止，不唤醒就永远看不到这个信号。

</details>

**位置**：`kernel/task/task.c:998`

<a id="fn-kernel-task-task.c-1019"></a>

### `task_signal_pending_effective`

```c
bool task_signal_pending_effective(task_t *t)
```

task_signal_pending_effective - 有没有"真正会打断阻塞"的信号

判据必须和 deliver_pending_signals() 的动作表一致：
SIG_IGN、以及 SIG_DFL 本身就是忽略的几个信号(SIGCHLD 等)不算。
管道阻塞和 nanosleep 都用它，避免两处各写一份、慢慢漂移。

<details>
<summary>函数内备注（1 条）</summary>

**第 1026 行（行尾）**

> 最低置位

</details>

**位置**：`kernel/task/task.c:1019`

<a id="fn-kernel-task-task.c-1060"></a>

### `task_send_signal_to_pgid`

```c
int task_send_signal_to_pgid(uint32_t pgid, int sig)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向指定进程组内所有存活的用户进程发送信号，并返回实际发送数量。

**位置**：`kernel/task/task.c:1060`

<a id="mod-kernel-task-x86_64-cpu_startup.c"></a>

## kernel/task/x86_64/cpu_startup.c — x86_64 BSP / 次级核启动

`kernel/task/x86_64/cpu_startup.c` · 6 个函数 · 0 个有说明 · 138 行

通用 CPU 管理留在 kernel/task/cpu.c；这里只放架构相关部分：
INIT-SIPI + AP trampoline、LAPIC ID 读取、TSS per-CPU 初始化，
以及 SMP 自检的 x86_64 部分。

<a id="fn-kernel-task-x86_64-cpu_startup.c-37"></a>

### `arch_cpu_bsp_hw_id`

```c
uint64_t arch_cpu_bsp_hw_id(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 39 行**

> x86 的 hw_id 是 LAPIC ID：arch_cpu_hw_id() 只返回 0 占位，
> BSP 也必须走控制器后端读真实值，否则真实硬件上 BSP 的 APIC ID != 0 时，
> sched_resched_cpu(0) 会把 IPI 发到错误的核。

</details>

**位置**：`kernel/task/x86_64/cpu_startup.c:37`

<a id="fn-kernel-task-x86_64-cpu_startup.c-45"></a>

### `cpu_secondary_bootstrap`

```c
void cpu_secondary_bootstrap(uint32_t cpu_id)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 57 行**

> resched IPI：必须在 online 之前装好, 否则 BSP 发来的 kick 会丢。

</details>

**位置**：`kernel/task/x86_64/cpu_startup.c:45`

<a id="fn-kernel-task-x86_64-cpu_startup.c-70"></a>

### `cpu_bring_up_all`

```c
void cpu_bring_up_all(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/x86_64/cpu_startup.c:70`

<a id="fn-kernel-task-x86_64-cpu_startup.c-127"></a>

### `arch_cpu_smp_test_begin`

```c
void arch_cpu_smp_test_begin(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 129 行**

> 保持原行为：x86 的 BSP 到这里已经能收 timer 中断, 不在这里重新开。

</details>

**位置**：`kernel/task/x86_64/cpu_startup.c:127`

<a id="fn-kernel-task-x86_64-cpu_startup.c-132"></a>

### `arch_cpu_smp_test_diag_begin`

```c
void arch_cpu_smp_test_diag_begin(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/x86_64/cpu_startup.c:132`

<a id="fn-kernel-task-x86_64-cpu_startup.c-135"></a>

### `arch_cpu_smp_test_diag_round`

```c
void arch_cpu_smp_test_diag_round(uint32_t round)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/x86_64/cpu_startup.c:135`

<a id="mod-kernel-task-x86_64-sched.c"></a>

## kernel/task/x86_64/sched.c — x86_64 调度路径的架构部分

`kernel/task/x86_64/sched.c` · 2 个函数 · 0 个有说明 · 26 行

x86 的 CR3/TSS/FS 切换留在 kernel/task/sched.c 的 #if ARCH_X86_64 里；
这里只放伪帧参数选择。

<a id="fn-kernel-task-x86_64-sched.c-18"></a>

### `arch_sched_prev_pgd_slot`

```c
uint64_t **arch_sched_prev_pgd_slot(task_t *prev)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/x86_64/sched.c:18`

<a id="fn-kernel-task-x86_64-sched.c-23"></a>

### `arch_sched_next_pgd`

```c
uint64_t *arch_sched_next_pgd(task_t *next)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/task/x86_64/sched.c:23`

<a id="mod-kernel-time-hrtimer.c"></a>

## kernel/time/hrtimer.c — 高精度睡眠/唤醒（x86_64 先用）

`kernel/time/hrtimer.c` · 13 个函数 · 12 个有说明 · 215 行

定时事件 = min(下一个 100Hz tick, 本核最早的 task->sleep_expire_ns)。
x86 用 LAPIC TSC-deadline 精确武装；其它架构不支持 oneshot 时，
仍然靠 100Hz tick 的 itimer_tick() 唤醒。

时间基准用 timer_get_ns()（x86 = TSC 换算）。

<a id="fn-kernel-time-hrtimer.c-32"></a>

### `timer_arch_oneshot_supported`

```c
bool timer_arch_oneshot_supported(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

恒返回 false，表示当前架构定时器不支持单次编程模式。

**位置**：`kernel/time/hrtimer.c:32`

<a id="fn-kernel-time-hrtimer.c-37"></a>

### `timer_arch_program_oneshot_ns`

```c
void timer_arch_program_oneshot_ns(uint64_t deadline_ns)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，忽略传入的截止时间，不编程任何硬件定时器。

**位置**：`kernel/time/hrtimer.c:37`

<a id="fn-kernel-time-hrtimer.c-55"></a>

### `hrtimer_tick_period_ns`

```c
static uint64_t hrtimer_tick_period_ns(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回配置的 tick 毫秒数（未配置按 10ms）换算得到的纳秒周期。

**位置**：`kernel/time/hrtimer.c:55`

<a id="fn-kernel-time-hrtimer.c-61"></a>

### `hrtimer_cpu_id`

```c
static uint32_t hrtimer_cpu_id(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回当前 CPU 编号，超出最大 CPU 数时回退为 0。

**位置**：`kernel/time/hrtimer.c:61`

<a id="fn-kernel-time-hrtimer.c-67"></a>

### `hrtimer_next_deadline`

```c
static uint64_t hrtimer_next_deadline(uint32_t cpu, uint64_t now)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取本 CPU 下次 tick 时间，并扫描同 CPU 上休眠任务的到期时刻取更早者作为截止时间。

**位置**：`kernel/time/hrtimer.c:67`

<a id="fn-kernel-time-hrtimer.c-86"></a>

### `hrtimer_program_next`

```c
static void hrtimer_program_next(uint32_t cpu, uint64_t now)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅对支持单次模式的 CPU 编程硬件定时器，截止时间已过则改为当前时间加 1 纳秒。

<details>
<summary>函数内备注（1 条）</summary>

**第 93 行（行尾）**

> 已经过期：尽量立刻触发

</details>

**位置**：`kernel/time/hrtimer.c:86`

<a id="fn-kernel-time-hrtimer.c-98"></a>

### `hrtimer_expire_sleepers`

```c
static void hrtimer_expire_sleepers(uint32_t cpu, uint64_t now)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历本 CPU 上已到期且处于阻塞态的休眠任务，清除其到期时刻并唤醒。

**位置**：`kernel/time/hrtimer.c:98`

<a id="fn-kernel-time-hrtimer.c-116"></a>

### `hrtimer_cpu_init`

```c
void hrtimer_cpu_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

记录本 CPU 是否支持单次模式并设定下次 tick 时间，支持时立即编程定时器。

**位置**：`kernel/time/hrtimer.c:116`

<a id="fn-kernel-time-hrtimer.c-128"></a>

### `hrtimer_isr`

```c
bool hrtimer_isr(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断并推进 tick（落后时重新对齐），唤起到期休眠者并重新编程，返回本次 tick 是否到期。

<details>
<summary>函数内备注（2 条）</summary>

**第 135 行**

> 在唤醒 sleeper 之前抓 ISR 入口延迟；此时 g_programmed_deadline_ns
> 还是本次中断武装时写入的值。非 oneshot 架构该值为 0，不记录。

**第 151 行**

> 被长关中断拖后时不要补跑一串 tick；重新对齐即可。

</details>

**位置**：`kernel/time/hrtimer.c:128`

<a id="fn-kernel-time-hrtimer.c-161"></a>

### `hrtimer_sleep_until`

```c
void hrtimer_sleep_until(uint64_t deadline_ns)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

让当前任务睡到指定纳秒时刻；不支持单次模式时循环阻塞，否则关中断登记截止时间、编程定时器后循环阻塞。

<details>
<summary>函数内备注（1 条）</summary>

**第 178 行**

> 关中断做“登记 deadline + 武装 LAPIC + 睡眠”这三步：
> 否则定时器可能在任务真正 BLOCKED 之前到期，ISR 看到 RUNNING 的任务
> 只清标记不唤醒，随后任务睡下去就永远醒不来。
> task_block() 内部会自己 arch_irq_save/restore；外层保持关中断即可。

</details>

**位置**：`kernel/time/hrtimer.c:161`

<a id="fn-kernel-time-hrtimer.c-196"></a>

### `hrtimer_oneshot_supported`

```c
bool hrtimer_oneshot_supported(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回当前 CPU 是否支持单次定时器模式。

**位置**：`kernel/time/hrtimer.c:196`

<a id="fn-kernel-time-hrtimer.c-201"></a>

### `hrtimer_cpu_oneshot_supported`

```c
bool hrtimer_cpu_oneshot_supported(uint32_t cpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 CPU 编号合法时返回该 CPU 是否支持单次定时器模式。

**位置**：`kernel/time/hrtimer.c:201`

<a id="fn-kernel-time-hrtimer.c-206"></a>

### `hrtimer_irq_latency`

```c
bool hrtimer_irq_latency(uint64_t deadline_ns, uint64_t *lat_ns)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/time/hrtimer.c:206`

<a id="mod-kernel-time-itimer.c"></a>

## kernel/time/itimer.c — 每任务 ITIMER_REAL(alarm / setitimer)

`kernel/time/itimer.c` · 4 个函数 · 0 个有说明 · 118 行

语义与设计见 include/time/itimer.h。这里只记实现上的两个关键点：

1. 到期后先重装再投信号。
周期定时器在投 SIGALRM 之前就把 expire 推到下一个周期。这样用户 handler
里再调 setitimer 时看到的是一致状态, 而不是被我们事后覆盖。

2. 唤醒阻塞中的任务。
task_send_signal() 只往 pending_sigs 里 OR 一个位, 不会把 TASK_BLOCKED
的任务挪回就绪队列。而 alarm 的全部意义就在于打断阻塞中的 read/poll ——
不显式 unblock, 任务要等到下一个事件(比如管道来了数据)才看得到信号,
"超时脱身"就永远不成立。实测 LTP kill02 正是卡在这里。

在 tick(ISR 上下文)里 unblock 是既有用法：sched_enqueue() 内部有
in_irq_context() 分支, 设备 ISR 唤醒任务是常规操作。

3. SMP：每个核的 tick 都会调到这里(sched_tick 是 per-CPU 回调), 所以同一个
到期定时器可能被两个核同时看到。不加锁是故意的 —— 大多数动作是幂等的,
但"先重装再投信号"不是原子的: 两个核可以同时通过 now >= expire 的检查,
各投一次 SIGALRM(周期定时器把 expire 推到相近的新时刻, 一次性定时器
都被清 0)。已知限制: 多核下 alarm 可能重复投递；若要严格"一个周期
一个信号", 需要原子 claim expiry(CAS)或让每个任务只由亲和核处理。
默认构建是 SMP=1, 但 LTP 门禁跑 SMP=4, 这条限制会被真实触发。

<a id="fn-kernel-time-itimer.c-41"></a>

### `itimer_now_ns`

```c
static inline uint64_t itimer_now_ns(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/time/itimer.c:41`

<a id="fn-kernel-time-itimer.c-46"></a>

### `itimer_set`

```c
void itimer_set(task_t *t, uint64_t value_ns, uint64_t interval_ns)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/time/itimer.c:46`

<a id="fn-kernel-time-itimer.c-55"></a>

### `itimer_get`

```c
void itimer_get(task_t *t, uint64_t *remain_ns, uint64_t *interval_ns)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`kernel/time/itimer.c:55`

<a id="fn-kernel-time-itimer.c-71"></a>

### `itimer_tick`

```c
void itimer_tick(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（5 条）</summary>

**第 81 行**

> 已死/正在分配的槽不该再收信号；顺手把残值清掉, 免得槽被复用后
> 新任务莫名其妙挨一个 SIGALRM(复用的 TCB 会带着旧字段)。

**第 91 行**

> nanosleep 到期：清标记并唤醒。
> 必须在 itimer 的早退判断**之前**处理 —— 睡眠任务通常没有
> itimer_expire_ns，早退会把它们直接跳过，永远醒不来。

**第 102 行**

> 下面只处理 ITIMER_REAL

**第 106 行**

> 先重装(周期)或清除(一次性), 再投信号 —— 见文件头第 1 点

**第 114 行**

> 见文件头第 2 点：不 unblock 就叫不醒阻塞中的任务

</details>

**位置**：`kernel/time/itimer.c:71`

<a id="mod-kernel-vmm-aarch64-el2_run.c"></a>

## kernel/vmm/aarch64/el2_run.c — VMM 主循环与 VM exit 分发

`kernel/vmm/aarch64/el2_run.c` · 21 个函数 · 21 个有说明 · 980 行

对标 ref/bare-vm/arch/aarch64/el2_run.c, 适配 Avatar OS。

当前支持的 exit 类型(ESR_EL2.EC)：
0x01 — WFI/WFE trap(HCR_EL2.TWI=1)：跳过指令, resume
0x16 — HVC 超级调用：dispatcher 处理 HVC_DONE / HVC_PRINT
0x24 — Stage-2 Data Abort：权限故障处理
其余 — 未处理, 打印诊断信息后退出

<a id="fn-kernel-vmm-aarch64-el2_run.c-36"></a>

### `read_esr_el2`

```c
static inline uint64_t read_esr_el2(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 ESR_EL2 系统寄存器。

**位置**：`kernel/vmm/aarch64/el2_run.c:36`

<a id="fn-kernel-vmm-aarch64-el2_run.c-48"></a>

### `el2_advance_pc`

```c
static void el2_advance_pc(vcpu_t *vcpu, uint64_t esr)
```

el2_advance_pc — 使 vcpu->elr 跳过当前陷入指令
AArch64: ESR.IL(bit 25)=1 → 32-bit 指令(+4), =0 → 16-bit(+2, Thumb)
注意：HVC 陷入时 ELR_EL2 硬件已指向 HVC 下一条, 无需调用此函数。

**位置**：`kernel/vmm/aarch64/el2_run.c:48`

<a id="fn-kernel-vmm-aarch64-el2_run.c-76"></a>

### `vcpu_gpr_xzr`

```c
__attribute__((noinline)) static uint64_t vcpu_gpr_xzr(const vcpu_t *vcpu, uint32_t srt)
```

vcpu_gpr_xzr — 按 SRT 取 guest 通用寄存器, SRT==31 视作 XZR(恒零)

注意： 为什么要有这个助手, 而不是在调用点写个三元表达式：

aarch64 的 `vcpu_t.r[]` 只有 31 格(x0–x30)—— x31 在架构上就是 XZR,
没有对应的存储。所以 `vcpu->r[srt]` 在 srt==31 时是越界访问,
在 C 里是 UB。

实测(aarch64-linux-musl-gcc 11.2.1, -O2)：UB 一旦形成, GCC 就把
"srt==31 时 val 为 0" 这半支优化没了 —— 反汇编里 `b.eq` 直接跳到
`mov x4, x21`, 而 x21 在整个函数里从没被清过, 于是取到调用者留下的野值。
我在调用点改成"先判 srt<31 再索引"同样没用(反汇编一模一样)；
隔离出来的最小复现反而是正确的 —— 也就是说这个函数里的某种上下文
放大了它。别再试图用别的写法绕, 直接跨函数边界：
这里 noinline, 且索引表达式在界内, 编译器没有可利用的 UB。

代价是提交路径上多一次函数调用 —— 那本来就是一次 EL2 往返, 可以忽略。

真机症状(这个 bug 造成的)：写 `PC_OP_EN=0`(`str wzr`)时把调用者的
一个指针写进了 NPU 寄存器, 作业永远不完成, task counter 恒 0,
而寄存器回读, regcmd 内容, DMA 地址全都正常 —— 从任何一处都看不出来。

<details>
<summary>函数内备注（1 条）</summary>

**第 80 行（行尾）**

> x31 == XZR

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:76`

<a id="fn-kernel-vmm-aarch64-el2_run.c-96"></a>

### `aarch64_vcpu_idreg_read`

```c
static uint64_t aarch64_vcpu_idreg_read(uint32_t key)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 SYSREG_ID 匹配 ID 寄存器，返回硬编码的 MIDR/MPIDR/PFR/DFR/ISAR/MMFR 等值，未知返回 0。

<details>
<summary>函数内备注（36 条）</summary>

**第 100 行（行尾）**

> MIDR: Cortex-A76-like

**第 102 行（行尾）**

> MPIDR: uniprocessor

**第 104 行（行尾）**

> REVIDR

**第 109 行（行尾）**

> ID_AA64PFR1_EL1

**第 111 行（行尾）**

> ID_AA64ZFR0_EL1

**第 113 行（行尾）**

> ID_AA64SMFR0_EL1

**第 116 行（行尾）**

> ID_AA64DFR0_EL1

**第 118 行（行尾）**

> ID_AA64DFR1_EL1

**第 121 行（行尾）**

> ID_AA64ISAR0_EL1

**第 123 行（行尾）**

> ID_AA64ISAR1_EL1: no PAuth

**第 125 行（行尾）**

> ID_AA64ISAR2_EL1: no newer PAuth

**第 130 行（行尾）**

> ID_AA64MMFR1_EL1

**第 132 行（行尾）**

> ID_AA64MMFR2_EL1

**第 134 行（行尾）**

> ID_AA64MMFR3_EL1

**第 137 行（行尾）**

> ID_PFR0_EL1

**第 139 行（行尾）**

> ID_PFR1_EL1

**第 141 行（行尾）**

> ID_DFR0_EL1

**第 143 行（行尾）**

> ID_AFR0_EL1

**第 145 行（行尾）**

> ID_MMFR0_EL1

**第 147 行（行尾）**

> ID_MMFR1_EL1

**第 149 行（行尾）**

> ID_MMFR2_EL1

**第 151 行（行尾）**

> ID_MMFR3_EL1

**第 153 行（行尾）**

> ID_ISAR0_EL1

**第 155 行（行尾）**

> ID_ISAR1_EL1

**第 157 行（行尾）**

> ID_ISAR2_EL1

**第 159 行（行尾）**

> ID_ISAR3_EL1

**第 161 行（行尾）**

> ID_ISAR4_EL1

**第 163 行（行尾）**

> ID_ISAR5_EL1

**第 165 行（行尾）**

> ID_MMFR4_EL1

**第 167 行（行尾）**

> ID_ISAR6_EL1

**第 169 行（行尾）**

> MVFR0_EL1

**第 171 行（行尾）**

> MVFR1_EL1

**第 173 行（行尾）**

> MVFR2_EL1

**第 175 行（行尾）**

> ID_PFR2_EL1

**第 177 行（行尾）**

> ID_DFR1_EL1

**第 179 行（行尾）**

> ID_MMFR5_EL1

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:96`

<a id="fn-kernel-vmm-aarch64-el2_run.c-208"></a>

### `handle_sgi1r`

```c
static void handle_sgi1r(vcpu_t *vcpu, uint64_t val)
```

ICC_SGI1R_EL1 写入 —— guest 发 SGI(Linux 的 IPI)。

这个寄存器没有被硬件虚拟化：guest 写它必然陷入 EL2(EC=0x18),
由 VMM 按目标 vCPU 置 vGIC 的 SGI pending。之前这里直接按"未处理
sysreg"跳过, 于是 Linux AP bring-up / reschedule IPI 全部丢失 ——
表现是 4 个 vCPU 都起来了, 但 guest 的 smp bring-up 停在半路。

字段(ARM IHI 0069)：
[15:0]  TargetList(Aff0 位图)
[23:16] Aff1
[27:24] INTID
[39:32] Aff2
[40]    IRM：1 = 除自己外所有 PEs
[47:44] RS：非 0 时按范围选择, 本 VMM 不支持
[55:48] Aff3

本 VMM 的 vMPIDR_EL2 = vcpu_id, DTB cpu@N reg = N, 所以只支持 Aff0,
Aff1/2/3=0, RS=0。投递走 vmm_vgic3_set_sgi_pending(), 然后 kick 目标
vCPU —— 它若正在 guest 里, resched IPI 会把它踹出来, 下一次 entry 的
sync_entry 才会把 SGI 排进 LR。

<details>
<summary>函数内备注（1 条）</summary>

**第 238 行**

> 与目标的 entry/exit 同步互斥：sync_exit 会对 active 的 LR 执行
> clear_pending, 裸写 pending 会被它抹掉(丢一次 IPI, SMP 启动/
> RCU 握手随机卡死)。锁内只写 pending, kick 放到锁外 —— 顺序是
> &quot;先发布, 再要求退出&quot;。

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:208`

<a id="fn-kernel-vmm-aarch64-el2_run.c-252"></a>

### `handle_sysreg`

```c
static int handle_sysreg(vcpu_t *vcpu, uint64_t esr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 ESR 的 sysreg 编码：读 op0=3/crn=0 填 ID 寄存器，写 ICC_SGI1R 触发 SGI，写 ICC_DIR 执行 deactivate，均推进 PC，其它告警后跳过。

<details>
<summary>函数内备注（2 条）</summary>

**第 271 行**

> ICC_SGI1R_EL1 = S3_0_C12_C11_5, 写

**第 279 行**

> ICC_DIR_EL1 / ICV_DIR_EL1 = S3_0_C12_C11_1, 写。
> ICH_HCR_EL2.TDIR=1, 所以 split EOI 模式的 deactivate 会到这里。

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:252`

<a id="fn-kernel-vmm-aarch64-el2_run.c-305"></a>

### `handle_wfi`

```c
static int handle_wfi(vcpu_t *vcpu, uint64_t esr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

采样计数并打印 WFI 心跳，推进 PC；真 WFI 每次让出，WFE 按每 vCPU 1ms 限频让出。

<details>
<summary>函数内备注（4 条）</summary>

**第 307 行**

> WFI 在网络/定时器驱动的 guest 里会高频触发(guest 空闲时几乎持续
> 执行 WFI)。之前每条都打 INFO 日志, 实测 25 秒产生 30 万行, 会把
> guest 控制台输出彻底淹没, 故降为 DEBUG。

> 保留计数而不是整段删掉：它是判断「guest 还活着 / 陷入频率」最直接
> 的探针, 交互式控制台的输入延迟上限也由这个频率决定。

**第 323 行**

> WFI(真 idle)立刻让出；WFE(自旋等待)限频让出。

> ESR_EL2.ISS[0] 是 TI：0 = WFI, 1 = WFE。guest 的 qspinlock 等待循环
> 就是 `sevl; wfe; ldxr ...`；TWE=1 会把每次 WFE 变成 VM-exit。

> 两种情况都实测过：
> - 每次 WFE 都 task_yield()：8 vCPU 挤少量宿主核时形成上下文切换
> 风暴, 锁持有者拿不到稳定的 CPU 时间, 实测偶发 1-2% 启动停滞；
> - WFE 完全不让出：如果 waiter 在 guest 里屏蔽了 IRQ, 宿主 tick
> 可能长时间进不来, 锁持有者被 waiter 堵在 runqueue 里, 实测出现
> 42s 才启动完成的极端慢启动。

> 折中：WFE 最多每 1ms 让出一次。既把切换频率限制在毫秒级, 又保证
> 锁持有者不会无限期等宿主 tick。WFI 仍然每次立刻让出, 否则空闲
> vCPU 会占着核满速空转, 饿死同核其它 vCPU。

**第 340 行（行尾）**

> TI=0: WFI

**第 348 行（行尾）**

> 1ms

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:305`

<a id="fn-kernel-vmm-aarch64-el2_run.c-380"></a>

### `dump_vttbr_diag`

```c
static void dump_vttbr_diag(vm_t *vm, const char *what, uint64_t ipa, uint64_t pa, uint64_t esr)
```

诊断：打印硬件 VTTBR_EL2/VTCR_EL2 与软件值的对比。

用来区分两种"映射加了但 guest 看不到"的故障：
MATCH    — 硬件确实在用本 VM 的表 ⇒ 问题在表内容/属性
MISMATCH — 硬件用的是别人的表 ⇒ 缺页处理把映射加到了 A 表,
而 guest 走 B 表(永远 fault, 且 stage2_map_page 第二次
就能命中自己那张表, 看起来"完全正常")

<details>
<summary>函数内备注（2 条）</summary>

**第 383 行（行尾）**

> ISS.FSC：故障类型

**第 391 行**

> FSC 是这里最关键的信息：
> 0x04/0x05/0x06/0x07 = translation fault (L0..L3)  ⇒ 表里真的没有
> 0x0D/0x0E/0x0F      = permission fault            ⇒ 表项在, 属性不对
> 0x09/0x0A/0x0B      = access flag fault            ⇒ AF 位没置

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:380`

<a id="fn-kernel-vmm-aarch64-el2_run.c-404"></a>

### `handle_dabt`

```c
static int handle_dabt(vcpu_t *vcpu, uint64_t esr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由 HPFAR/FAR 计算 IPA，依次处理 NPU 恒等映射、RAM 块按需映射和 MMIO 分发；命中后重试指令或推进 PC，未命中读回 0。

<details>
<summary>函数内备注（16 条）</summary>

**第 411 行**

> HPFAR[39:4] = IPA[47:12] → IPA_base = hpfar &lt;&lt; 8(总是页对齐的)。

> 注意： 页内偏移只在数据访问(S1PTW=0)时能从 FAR 拿到；S1PTW=1 表示
> 这次故障是 guest 的 stage-1 页表遍历引起的, 此时 FAR_EL2 是 UNKNOWN
> (实测装的是 guest 的虚拟地址), 把它的低 12 位并进来是错的。

**第 421 行**

> 0) ── RAM 缺页：按需分配一个物理页 ──────────────────────────

> 注意： 这一段必须在最前面。下面那条「没有匹配设备就返回 0 并推进 PC」的
> 兜底(x-kernel 的语义)会静默吞掉未映射的 RAM 访问：读回 0, 写丢
> 进黑洞, guest 带着垃圾数据继续跑。那比直接崩掉难查得多。

> 判定用 ipa_page(页对齐)—— HPFAR 给的就是页基址。

**第 430 行**

> 0a) ── NPU DMA 窗口：恒等映射, 不分配宿主页 ────────────────

> 注意： 必须在上面那条&quot;RAM 按需分配&quot;之前。这块地址落在 guest RAM 窗口
> 内(它是 RAM 顶部的 48 MiB), 如果先走 RAM 分支, 就会被 pmm_alloc_pages
> 分到一段与 IPA 无关的宿主页 —— 而 NPU 是无 IOMMU 的物理设备, 它照
> 寄存器里的物理地址 DMA, 于是写进别人的内存。不报错, 只是数据全错。

> 恒等的意义：guest 里 librknnrt 拿到的&quot;设备地址&quot;就是驱动从这块预留池
> 分配出来的物理地址, 只有 IPA == PA 时它在两边指的是同一块内存。
> 详见 include/arch/aarch64/stage2.h 里 s2_ctx_t.npu_dma_base。

**第 448 行**

> 与普通 RAM 缺页共用 map_lock：同一 2 MiB 块的 L2/L3 写入必须串行

**第 464 行（行尾）**

> 不推进 PC：让 guest 重试那条指令

**第 469 行**

> 一次装整个 2 MiB 块, 而不是一页 —— 见 stage2_map_block 的注释：
> Linux 启动期缺页密集, 单页映射每次都要一次 EL2 往返(TCG 下约
> 1.2ms), 1447 次就是 1.7 秒。

**第 477 行**

> 同一 VM 的多个 vCPU 会并发缺页。整段「映射 + 复查」必须在
> map_lock 下串行, 否则两个 vCPU 会各自分配 L3 表/物理页并互相
> 覆盖, 被覆盖的页从 guest 看就是&quot;内存被换掉/清零&quot;。

**第 483 行（行尾）**

> zero

**第 487 行**

> 注意： 判据是 pa, 不是 map_block 的返回值：块已经被别的 vCPU 装好时
> stage2_map_block() 返回 0, 但 lookup 会命中 —— 这是成功, 不能
> 当成 &quot;PMM exhausted&quot; 退出 VM。

**第 502 行**

> 注意： 这里故意不做 TLB 维护。

> 缺页意味着这个 IPA 之前没有有效映射 —— TLB 里不可能有它的翻译
> (ARM 也不要求实现缓存&quot;无效&quot;结果, QEMU/KVM 不缓存)。所以刚装上的
> 表项立即可见, 不需要 tlbi。

> 反过来代价极大：stage2_tlb_flush_ipa() 里有两道 barrier_sync()
> (dsb ish), TCG 下要同步所有 vCPU —— 每次缺页都做的话, guest
> 启动时几千次缺页能白白多花一秒多。对照：加载期的批量映射
> stage2_map_range() 也是只在末尾刷一次。

**第 514 行（行尾）**

> 注意： 不推进 PC：让 guest 重试那条指令

**第 517 行**

> 1) 先尝试 MMIO 设备分发

**第 520 行**

> 访问宽度：ESR.ISS.SAS[23:22](0=byte,1=half,2=word,3=dword)

**第 522 行**

> 写数据：从 ISS.SRT[20:16] 指定的通用寄存器取。

> 注意： SRT == 31 是 XZR(恒零寄存器), 必须当 0 处理。
> `str wzr, [x1]`(存零)的 SRT 就是 31, 而 vcpu-&gt;r[31] 是个
> 没人清过的数组槽 —— 直接取它会拿到上一次留下的野值。

> 漏掉这个判断的后果不是&quot;写错一个值&quot;, 而是静默把随机数据
> 写进设备寄存器。真机上撞的是 NPU 提交路径里唯一那次写零
> (`REG_WRITE(0x0, PC_OP_EN)`)：NPU 收到的&quot;开始/停止&quot;位
> 是个内核指针, 于是作业怎么都不完成, task counter 恒 0,
> 而寄存器回读, regcmd 内容, DMA 地址全都正常。

> 下面读路径那句 `if (srt != 31)` 一直是对的 —— 写路径漏了它,
> 两条路本该对称。

**第 547 行（行尾）**

> XZR 丢弃

**第 555 行**

> x-kernel behavior: unmapped MMIO reads return 0 and writes are ignored.
> Linux probes optional devices early; retrying a non-emulated IPA would
> fault forever.

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:404`

<a id="fn-kernel-vmm-aarch64-el2_run.c-578"></a>

### `handle_iabt`

```c
static int handle_iabt(vcpu_t *vcpu, uint64_t esr)
```

── Stage-2 Instruction Abort(EC=0x20)─────────────────────

按需分页之前这条路径基本不会出现；现在必然会被触发：
- guest 的 stage-1 页表遍历要为取指服务 → S1PTW=1 的 instruction abort；
- guest 关 MMU 的早期阶段, 取指也会变成 stage-2 fault。

注意： 不能像 data abort 那样在"没命中设备"时走「返回 0 并推进 PC」的兜底 ——
取指没有"返回 0"这回事, 静默推进 PC 只会让 guest 跑到垃圾地址上。所以
非 RAM 的取指一律报错停 VM。

<details>
<summary>函数内备注（4 条）</summary>

**第 582 行（行尾）**

> HPFAR 给的就是页基址

**第 590 行（行尾）**

> zero

**第 603 行**

> 同上：首次映射不需要 TLB 维护

**第 604 行（行尾）**

> 不推进 PC：重试这次取指

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:578`

<a id="fn-kernel-vmm-aarch64-el2_run.c-634"></a>

### `psci_fid_supported`

```c
static int psci_fid_supported(uint64_t fid)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对 PSCI VERSION/CPU_ON/AFFINITY_INFO/SYSTEM_OFF/RESET/FEATURES 等 FID 返回 1，其它返回 0。

**位置**：`kernel/vmm/aarch64/el2_run.c:634`

<a id="fn-kernel-vmm-aarch64-el2_run.c-652"></a>

### `handle_psci`

```c
static int handle_psci(vcpu_t *vcpu)
```

返回 EL2_RESUME / EL2_VMEXIT；非 PSCI(前缀不符)时返回 -1 表示未处理

<details>
<summary>函数内备注（4 条）</summary>

**第 658 行（行尾）**

> 非 PSCI 调用

**第 662 行（行尾）**

> PSCI v0.2

**第 667 行**

> Linux 的 cpu_psci_cpu_boot() 传 target_cpu = DT cpu@N 的 reg
> (也就是 MPIDR)。本 VMM 的 vMPIDR_EL2 = vcpu_id, DTB 的
> cpu@N reg = N, 所以 Aff0 就是 vcpu_id；Aff1/2/3 必须为 0。

**第 706 行**

> 0 = ON, 1 = OFF(Linux 的 psci_affinity_info 约定)

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:652`

<a id="fn-kernel-vmm-aarch64-el2_run.c-728"></a>

### `handle_hvc`

```c
static int handle_hvc(vcpu_t *vcpu, uint64_t esr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先尝试 PSCI；否则按 x0 的 HVC 号处理打印（采样日志并 yield）或 DONE（退出 VM），未知号告警后继续。

<details>
<summary>函数内备注（4 条）</summary>

**第 731 行（行尾）**

> HVC number from guest x0

**第 733 行**

> 先尝试 PSCI(function ID 前缀 0x84/0xC4, 与 HVC_PRINT/DONE 不冲突)

**第 741 行**

> 必须留在 INFO：readme.md 用 LOG=info 跑 test-vmm, 这行就是通过证据。
> 但原来的 iter % 100 仍随迭代无界增长 —— 改用公共采样器抽稀成有界。

**第 745 行**

> 主动 yield：让出 CPU 给其他线程

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:728`

<a id="fn-kernel-vmm-aarch64-el2_run.c-763"></a>

### `vmm_exit_handler`

```c
static int vmm_exit_handler(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 ESR 的 EC 分发 WFI、HVC、SMC/PSCI、系统寄存器、数据与指令 abort 处理，异步退出仅采样计数，未知 EC 打印寄存器后退出。

<details>
<summary>函数内备注（7 条）</summary>

**第 779 行（行尾）**

> WFI/WFE

**第 781 行（行尾）**

> HVC

**第 782 行（行尾）**

> SMC (PSCI conduit)

**第 783 行**

> SMC 陷入时 ELR_EL2 指向 SMC 指令本身, 需手动步进；
> 而 HVC 陷入时硬件已指向下一条。

**第 797 行（行尾）**

> trapped sysreg

**第 799 行（行尾）**

> Stage-2 Data Abort

**第 801 行（行尾）**

> Stage-2 Instruction Abort
> —— 按需分页后必然触发
> (取指缺页 / S1PTW)

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:763`

<a id="fn-kernel-vmm-aarch64-el2_run.c-831"></a>

### `aarch64_check_vtimer`

```c
static void aarch64_check_vtimer(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若虚拟定时器已使能且未屏蔽、计数超过 cval+CNTVOFF，则向该 vCPU 注入 VTIMER 中断。

<details>
<summary>函数内备注（1 条）</summary>

**第 851 行（行尾）**

> vGIC 分层 dump 已收(排查完了, 见 git 历史)

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:831`

<a id="fn-kernel-vmm-aarch64-el2_run.c-867"></a>

### `aarch64_check_vpl011_rx`

```c
static void aarch64_check_vpl011_rx(vcpu_t *vcpu)
```

── 虚拟 PL011 RX 中断投递 ──────────────────────────────────

与 vtimer 同理, 在进入 guest 前把「设备侧已就绪」的中断同步给 vGIC。

RX 中断是电平触发：只要 RX FIFO 里还有数据, 且 guest 在 UARTIMSC
里开着 RX 位, 就必须保持 pending。只在 push 时置一次不行 —— guest
应答时 vGIC 会把 pending 位清掉(见 vmm_vgic3_sync_exit 的 clear_pending),
FIFO 里剩余的字节会因此再也没人来取。所以放在每次入口处重拉。

**位置**：`kernel/vmm/aarch64/el2_run.c:867`

<a id="fn-kernel-vmm-aarch64-el2_run.c-903"></a>

### `aarch64_check_vnpu`

```c
static void aarch64_check_vnpu(vcpu_t *vcpu)
```

── 虚拟 NPU 中断投递(RK3588 三核透传)───────────────────────

与 vPL011 RX 同构, 理由也一样：NPU 的中断是电平触发 —— 核里的
INT_STATUS 一直有效到 guest 写 INT_CLEAR。只在事件发生那一刻置一次
pending 是边沿做法, guest 应答时 vGIC 会把 pending 清掉, 剩下的
状态位(三个核先后完成, 或一个核里多个块)就再也没人来取了。
所以放在每次入口重拉。

注意： 为什么是轮询, 而不是像 GICv2 的 vtimer 那样装宿主 ISR：
宿主 ISR 在 guest 运行时是在 EL2 就地处理完直接 eret 回 guest 的
(不经过 vCPU 主循环, 拿不到 lr_switch_in 那次同步), 而且它的正常
收尾就是写 INT_CLEAR —— 等于把 guest 的中断吃掉, 两边都不报错。
更糟的是电平线在宿主 ack 之后立刻重触发(状态位还在), 宿主会卡死
在 ISR 里, guest 永远拿不到 CPU。轮询把这三个问题一次绕开,
代价只是每次入口多读 6 个寄存器(三核各 INT_STATUS/INT_MASK),
相对一次 EL2 往返可以忽略。

三核各投各的, 不是"任一核有效就投三条"：单核掩码的作业只有一个核
会完成, 多核作业三核完成时刻也差几微秒, 混在一起会让 guest 收到
不属于它的中断(drivers 侧表现为 "irq but no task")。

<details>
<summary>函数内备注（1 条）</summary>

**第 908 行**

> 只在上升沿记一次, 这样这个计数就是&quot;注入了几次中断&quot;,
> 而不是&quot;重拉了几次&quot;(后者是入口次数, 没有意义)。
> 真机验证时看 core1/core2 两行是否都出现, 就知道三核都报过中断。

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:903`

<a id="fn-kernel-vmm-aarch64-el2_run.c-926"></a>

### `vmm_arch_restore_guest_ctx`

```c
void vmm_arch_restore_guest_ctx(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

设置 VMPIDR，恢复 EL1/EL2 系统寄存器，重查定时器/控制台/NPU 中断并把可投递中断同步进 vGIC。

<details>
<summary>函数内备注（4 条）</summary>

**第 933 行**

> 每次入口按设备/定时器状态重拉虚拟中断, 再同步进 vGIC。

**第 938 行**

> vIRQ：把可投递中断排入 LR, 让 GIC 向 guest 产生虚拟 IRQ。
> GICv2 走 GICH_LR(MMIO), GICv3 走 ICH_LR&lt;n&gt;_EL2(系统寄存器)。

**第 942 行**

> 每 CPU 的 ICH_HCR_EL2.En 必须在这里开, 不能只在 vm_create() 里开一次：
> vm_create 跑在调用者的核上, 而 vCPU 任务是钉在 CPU0 的, SMP&gt;1 时两者可能
> 不是同一个核 —— 那会让 guest 一个虚拟中断都收不到(详见
> vmm_vgic3_hw_init 的注释)。同一个核重复写是幂等的。

**第 949 行**

> 注意： 用 switch_in 而不是直接 sync_entry：LR 是 per-pCPU 硬件, 本核
> 可能刚跑过别的 VM 的 vCPU 任务, 直接 sync 会串味(见其注释)。

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:926`

<a id="fn-kernel-vmm-aarch64-el2_run.c-958"></a>

### `vmm_arch_enter_guest`

```c
int vmm_arch_enter_guest(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用汇编入口 el2_enter_guest 进入 guest，并固定返回 1。

<details>
<summary>函数内备注（1 条）</summary>

**第 961 行（行尾）**

> el2_enter_guest 总是成功返回(否则直接崩溃)

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:958`

<a id="fn-kernel-vmm-aarch64-el2_run.c-964"></a>

### `vmm_arch_exit_handler`

```c
int vmm_arch_exit_handler(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转调 vmm_exit_handler 处理 EL2 退出。

**位置**：`kernel/vmm/aarch64/el2_run.c:964`

<a id="fn-kernel-vmm-aarch64-el2_run.c-969"></a>

### `vmm_arch_save_guest_ctx`

```c
void vmm_arch_save_guest_ctx(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存 EL1/EL2 系统寄存器到 vCPU，并按 GIC 版本回读 LR、同步 vGIC 的 active 状态。

<details>
<summary>函数内备注（1 条）</summary>

**第 973 行**

> GICv3：guest 的 ack/EOI 由硬件直接服务, 只能在这里回读 LR 状态,
> 把 guest 已经 EOI 掉的中断从软件 active 记帐里摘掉。

</details>

**位置**：`kernel/vmm/aarch64/el2_run.c:969`

<a id="mod-kernel-vmm-aarch64-virq.c"></a>

## kernel/vmm/aarch64/virq.c — AArch64 的「拉线」适配器

`kernel/vmm/aarch64/virq.c` · 1 个函数 · 1 个有说明 · 44 行

把一根 virq_t(号码是 GIC INTID)接到 vGIC 后端。见 include/vmm/vmm_virq.h
说明为什么只统一「线」而不统一「控制器」。

单独一个文件而不是塞进 el2_run.c：那个文件目前不 include vgic 的两个头,
塞进去会为了一行转发新增两条 include(宏冲突的经典入口)。本目录已被
Makefile 的 _KERNEL_ARCH_MODULES 自动收编, 不需要改构建。

<a id="fn-kernel-vmm-aarch64-virq.c-24"></a>

### `vmm_arch_irq_raise`

```c
void vmm_arch_irq_raise(vcpu_t *vcpu, virq_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 GIC 版本把指定中断线置为对应 vCPU 的 pending，不做使能判断。

<details>
<summary>函数内备注（1 条）</summary>

**第 34 行**

> 这里不做任何使能判断：guest 有没有 enable 这条线由 vGIC 在投递时
> (LR 装载 / pending &amp; enabled &amp; !active)判定。在 raise 时刻先判一次会
> 改变可观测行为 —— 见 vmm_virq.h。

</details>

**位置**：`kernel/vmm/aarch64/virq.c:24`

<a id="mod-kernel-vmm-aarch64-vm_init.c"></a>

## kernel/vmm/aarch64/vm_init.c — AArch64 的 VM 初始化

`kernel/vmm/aarch64/vm_init.c` · 2 个函数 · 2 个有说明 · 177 行

从 vmm.c 搬过来的：那是「架构无关」的文件, 却装着本架构 124 行的完整初始化
(stage-2, MMIO 总线, vGIC, vPL011, vCPU 数组), 而它的两个对等物
(x86 的 vmm_arch_vm_init / riscv 的 vmm_arch_vm_init)一直就在各自架构目录里。

由 vm.c 的 vm_create() 调用(Step 4b 之后统一成 vmm_arch_vm_init 钩子)。
本目录已被 Makefile 的 _KERNEL_ARCH_MODULES 对非 aarch64 整目录 filter-out,
所以外面那层 `#if ARCH_AARCH64` 是冗余的, 已去掉。

<a id="fn-kernel-vmm-aarch64-vm_init.c-37"></a>

### `vmm_arch_vm_init`

```c
int vmm_arch_vm_init(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 vCPU 数，初始化空 stage-2 与 NPU 窗口，注册 vNPU、vPL011 与 vGIC 设备，逐 vCPU 清零并初始化锁和 completion。

<details>
<summary>函数内备注（12 条）</summary>

**第 47 行**

> 初始化本 VM 自己的 Stage-2 —— 空表。

> 空表即&quot;全 trap&quot;：guest 访问任何 IPA 都会陷入 EL2, 由缺页处理决定是
> 模拟设备还是分配一页 RAM。所以不再需要老版本那句
> stage2_enable_mmio_trap()(它是&quot;先把 4 GiB 全映射再清掉非 RAM&quot;,
> 本质是同一件事, 只是绕了一圈)。

> 调用方(guest_loader)随后用 stage2_map_range() 把内核映像/DTB/initrd/
> 初始栈这几块宿主自己要写的地方显式映射进去, 其余全靠缺页。

**第 60 行**

> NPU 透传用的恒等窗口。非 RK3588 平台上这两个宏是 0, 窗口即关闭
> (stage2_set_npu_dma_window 用 size==0 表示&quot;没有&quot;)。
> 为什么必须恒等, 以及三处地址为什么要一致, 见 include/vmm/guest_loader.h。

**第 69 行**

> 建立 MMIO 总线并注册虚拟设备(虚拟 PL011 控制台)

**第 78 行**

> NPU 寄存器透传(0xFDAB0000..0xFDAE0000)。

> 平台没有 NPU 时 vnpu_init 直接返回 0 且不注册 —— 不是错误。
> 有 NPU 但注册失败要让建 VM 失败, 理由同下面 vpl011 那段注释：
> 静默死设备的症状是 guest 里驱动读到全 0, 日志上看不出异常。

**第 91 行**

> 注册失败的后果是静默死设备：该 GPA 在 stage-2 里没有映射 →
> guest 访问时陷入 → MMIO 总线未命中 → 读 0/写丢弃。guest 侧的
> PL011 会「初始化成功但永远收不到中断」, 日志上看不出任何异常。
> 所以这里必须让建 VM 失败, 而不是打个警告继续。

**第 101 行**

> vGIC layering:
> - vgic/vgic3 : VM-level virtual interrupt lifecycle state
> - vgicd/vgic3d: guest GICD MMIO configuration and forwarding
> - vgicc      : (GICv2) per-vCPU GICC MMIO + GICH LR cache
> - vgic3r     : (GICv3) per-vCPU redistributor(SGI/PPI 的 GICR)

**第 109 行**

> GICv3：GICD 只管 SPI, SGI/PPI 在 GICR；CPU interface 是系统寄存器,
> VMM 靠 ICH_LR&lt;n&gt;_EL2 注入 —— 没有 GICC MMIO 设备。

**第 118 行**

> 注意：不在这里开 ICH_HCR_EL2.En。
> 那是每 CPU 的系统寄存器, 必须设在真正跑 vCPU 的核上, 而本函数跑在
> 调用者的核上(从 /dev/vmm 启动时是用户态 helper 所在的核)。
> 见 vmm_vgic3_hw_init() 的注释 —— SMP&gt;1 时在这里设会直接让 guest 收不到
> 虚拟中断。真正的设置点在 vmm_arch_restore_guest_ctx(), 每次进 guest 前。

**第 126 行**

> 虚拟 CPU 接口：GICC MMIO + per-vCPU GICH LR 缓存。

**第 130 行**

> 虚拟 GICv2：GICD/GICC 由 MMIO 模拟, vgic core 维护软件中断状态。

**第 145 行**

> 初始化每个 vCPU 的状态

**第 152 行（行尾）**

> 还没跑过：0 是合法 CPU, 不能拿 0 当&quot;未运行&quot;

</details>

**位置**：`kernel/vmm/aarch64/vm_init.c:37`

<a id="fn-kernel-vmm-aarch64-vm_init.c-170"></a>

### `vmm_arch_vm_destroy`

```c
void vmm_arch_vm_destroy(vm_t *vm)
```

vmm_arch_vm_destroy — 与 vmm_arch_vm_init 严格逆序

目前 aarch64 只有 vpl011 一个池化设备(vGIC 的状态仍在 vm_t 里, 本轮不动)。
加新设备时在这里补一行, 顺序与 init 相反。

<details>
<summary>函数内备注（1 条）</summary>

**第 173 行**

> 把 NPU 还给宿主驱动 —— 不做的话停掉 VM 之后宿主 NPU 就永久废了,
> 而且再起一个 VM 会撞上 vnpu 的占用守卫。见 vmm_vnpu.h。

</details>

**位置**：`kernel/vmm/aarch64/vm_init.c:170`

<a id="mod-kernel-vmm-loader-fdt.c"></a>

## kernel/vmm/loader/fdt.c — FDT/DTB 的字节级工具与补丁

`kernel/vmm/loader/fdt.c` · 12 个函数 · 12 个有说明 · 505 行

从 guest_loader.c 拆出来的。这一组的特点是入参就是宿主缓冲
(`uint8_t *dtb, uint32_t dtb_size`), 一处不碰 vm_t, 也不碰 stage-2 ——
所以是全文件里唯一能零改动搬走的部分。

内容：
- FDT 常量与 token 级工具(be32 / put_be32 / find_string_offset / encode_cells)
- guest_loader_patch_dtb_initrd   —— 改写 /chosen/linux,initrd-{start,end}
- guest_loader_patch_dtb_memory   —— 改写 /memory 的 reg
- guest_loader_patch_dtb_bootargs —— 原地改写 /chosen/bootargs
- guest_loader_nop_dtb_nodes      —— 把未模拟的设备节点改成 nop

原型都在 include/vmm/guest_loader.h(公共头), 所以拆出来不需要动任何声明。

<a id="fn-kernel-vmm-loader-fdt.c-37"></a>

### `be32`

```c
static uint32_t be32(const uint8_t *p, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从字节缓冲偏移处按大端读取 32 位值。

**位置**：`kernel/vmm/loader/fdt.c:37`

<a id="fn-kernel-vmm-loader-fdt.c-42"></a>

### `put_be32`

```c
static void put_be32(uint8_t *p, uint32_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 32 位值按大端写入字节缓冲。

**位置**：`kernel/vmm/loader/fdt.c:42`

<a id="fn-kernel-vmm-loader-fdt.c-51"></a>

### `find_string_offset`

```c
static int find_string_offset(const uint8_t *strings, uint32_t slen, const char *name)
```

在 strings 块里找以 NUL 结尾的属性名, 返回其偏移

**位置**：`kernel/vmm/loader/fdt.c:51`

<a id="fn-kernel-vmm-loader-fdt.c-66"></a>

### `encode_cells`

```c
static void encode_cells(uint8_t *p, int cells, uint64_t value)
```

按 cells 个数以大端写入 value

**位置**：`kernel/vmm/loader/fdt.c:66`

<a id="fn-kernel-vmm-loader-fdt.c-79"></a>

### `guest_loader_patch_dtb_initrd`

```c
int guest_loader_patch_dtb_initrd(uint8_t *dtb, uint32_t dtb_size, uint64_t initrd_start, uint64_t initrd_end)
```

遍历 FDT 结构块, 把 initrd 的 start/end 两个 64 位属性改写到指定值。
(对标 kvmm loader.rs 的 patch_dtb_initrd)

<details>
<summary>函数内备注（2 条）</summary>

**第 130 行**

> 无操作

**第 132 行（行尾）**

> FDT_END 或异常

</details>

**位置**：`kernel/vmm/loader/fdt.c:79`

<a id="fn-kernel-vmm-loader-fdt.c-162"></a>

### `skip_name`

```c
static uint32_t skip_name(const uint8_t *dtb, uint32_t pos, uint32_t end)
```

结构块里跳过节点名(NUL 结尾)后的对齐位置

**位置**：`kernel/vmm/loader/fdt.c:162`

<a id="fn-kernel-vmm-loader-fdt.c-181"></a>

### `guest_loader_dtb_node_has_prop`

```c
int guest_loader_dtb_node_has_prop(const uint8_t *dtb, uint32_t dtb_size, const char *node, const char *prop)
```

在结构块里找到根下的 node 节点, 看它自己有没有 prop 属性。

注意： 单层循环, 不要写成"内层循环扫属性" —— 那样内外两层都会去消费
token, `pos` 被推进两次, 第二个节点开始就全乱了。第一版就是这么写的,
结果对所有 DTB 都返回 0(包括明明有属性的那份)—— 一个永远返回
"没有"的检查会让人去改本来正确的东西。所以这里用 hit 标记当前是否
正处在目标节点的属性区里, 全部在一个循环里推进。

FDT_NOP 要显式跳过：guest_loader_nop_dtb_nodes() 会把整个节点换成 NOP,
跳过之后才能继续扫后面的兄弟节点。

<details>
<summary>函数内备注（5 条）</summary>

**第 187 行（行尾）**

> 已经命中目标节点

**第 188 行（行尾）**

> 命中时所在深度(属性要在这个深度上才算它的)

**第 212 行**

> 匹配任意深度 —— 要查的两个节点不在同一层：
> npu@fdab0000 是根的直接子节点(depth 2), 而
> npu-pool@79000000 嵌在 /reserved-memory/ 里(depth 3)。
> 这两个名字本身唯一, 所以不用限定层级。
> (第一版限定 depth==2, 于是 no-map 那条永远是 0 —— 又一个
> &quot;看起来在查, 其实查不到&quot;的假阴性。)
> 名字要完全相等：npu@fdab0000 不能命中 npu@fdab0001。

**第 229 行（行尾）**

> 目标节点的属性区看完了, 没有

**第 246 行（行尾）**

> FDT_END 或结构块损坏

</details>

**位置**：`kernel/vmm/loader/fdt.c:181`

<a id="fn-kernel-vmm-loader-fdt.c-252"></a>

### `guest_loader_patch_dtb_memory`

```c
int guest_loader_patch_dtb_memory(uint8_t *dtb, uint32_t dtb_size, uint64_t mem_base, uint64_t mem_size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 FDT，读取顶层 #address/#size-cells，把 depth 2 的 memory 节点 reg 改写为给定基址和大小，成功后清 cache。

<details>
<summary>函数内备注（1 条）</summary>

**第 319 行**

> 无操作

</details>

**位置**：`kernel/vmm/loader/fdt.c:252`

<a id="fn-kernel-vmm-loader-fdt.c-332"></a>

### `guest_loader_patch_dtb_bootargs`

```c
int guest_loader_patch_dtb_bootargs(uint8_t *dtb, uint32_t dtb_size, const char *bootargs)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 FDT 字符串表找到 bootargs 属性，缓冲足够则清零并写入新串、清 cache 返回 0，否则失败。

<details>
<summary>函数内备注（1 条）</summary>

**第 381 行**

> 无操作

</details>

**位置**：`kernel/vmm/loader/fdt.c:332`

<a id="fn-kernel-vmm-loader-fdt.c-402"></a>

### `dtb_node_end`

```c
static int dtb_node_end(const uint8_t *dtb, uint32_t pos, uint32_t end, uint32_t *end_out)
```

对标 kvmm loader.rs 的 nop_dtb_nodes：
把指定节点的结构块子树整体替换为 FDT_NOP 令牌, 使 guest 完全看不到
该设备, 从而不会去探测它。

为什么必须这么做：VMM 只模拟了 PL011 与 GICD, DTB 里其余设备
(GICv2M MSI 帧, PCIe, PL061, PL031, flash…)都没有实现。guest 一旦
访问这些地址就会 Stage-2 fault, 而 exit handler 未命中 MMIO 总线时按
「权限故障」处理(恢复属性让 guest 重试)→ 立即再次 fault → 死循环,
表现为 guest 卡死在探测那台设备上。

<details>
<summary>函数内备注（2 条）</summary>

**第 432 行**

> 无操作

**第 434 行（行尾）**

> FDT_END 或异常

</details>

**位置**：`kernel/vmm/loader/fdt.c:402`

<a id="fn-kernel-vmm-loader-fdt.c-440"></a>

### `dtb_nop_range`

```c
static void dtb_nop_range(uint8_t *dtb, uint32_t start, uint32_t end)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把指定范围内按 4 字节对齐的每个单元改写为 FDT_NOP。

**位置**：`kernel/vmm/loader/fdt.c:440`

<a id="fn-kernel-vmm-loader-fdt.c-446"></a>

### `guest_loader_nop_dtb_nodes`

```c
int guest_loader_nop_dtb_nodes(uint8_t *dtb, uint32_t dtb_size, const char *const *names, int nr_names)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历 FDT 结构块，按完整节点名匹配，命中后用 FDT_NOP 覆盖整个节点，返回屏蔽数并清 cache。

<details>
<summary>函数内备注（2 条）</summary>

**第 476 行**

> 与待屏蔽名单逐个比对(按完整节点名)

**第 495 行**

> 无操作

</details>

**位置**：`kernel/vmm/loader/fdt.c:446`

<a id="mod-kernel-vmm-loader-guest_loader.c"></a>

## kernel/vmm/loader/guest_loader.c — guest 镜像加载与启动

`kernel/vmm/loader/guest_loader.c` · 4 个函数 · 4 个有说明 · 577 行

移植自 x-kernel: virt/kvmm-api/src/loader.rs, 适配 Avatar OS：
- kvfs → avatar 的 vfs_open / vfs_read_to_phys
- 加载目标为 guest 物理地址(经 stage2 的 identity map 直接换算)
- DTB 修补逻辑(initrd / memory)按 FDT 规范逐 token 走查

<a id="fn-kernel-vmm-loader-guest_loader.c-172"></a>

### `guest_loader_dtb_path`

```c
static const char *guest_loader_dtb_path(int nr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 vCPU 数返回 DTB 路径：1 核用默认或单核常量，2/4/8/12 查静态表，其余返回 NULL。

**位置**：`kernel/vmm/loader/guest_loader.c:172`

<a id="fn-kernel-vmm-loader-guest_loader.c-188"></a>

### `guest_loader_dtb_path`

```c
static const char *guest_loader_dtb_path(int nr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 vCPU 数返回 DTB 路径：1 核用默认或单核常量，2/4/8/12 查静态表，其余返回 NULL。

**位置**：`kernel/vmm/loader/guest_loader.c:188`

<a id="fn-kernel-vmm-loader-guest_loader.c-205"></a>

### `guest_loader_check_vcpus`

```c
int guest_loader_check_vcpus(int nr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 vCPU 数：x86 只查范围，aarch64 需 GICv3 且限 1/2/4/8/12，riscv 同样限定，并确认对应 DTB 存在。

<details>
<summary>函数内备注（4 条）</summary>

**第 213 行**

> x86 的 AP 入口是 guest 自己写 ICR 发 INIT/SIPI, VMM 侧在 vLAPIC
> 里实现(vmm_vcpu_start + MP 表), 不依赖 DTB/固件数据, 1..MAX_VCPUS
> 都可以。真正的上限由 MAX_VCPUS 与 vLAPIC 静态池共同决定。

**第 220 行**

> AP 入口由 SBI HSM hart_start 决定(guest 从 DTB 里看到 N 个 cpu,
> Linux 逐个 hart_start)。DTB 与 vCPU 数必须一一对应 —— 仓库里只
> 生成了 1/2/4/8/12 这几档。

**第 232 行**

> DTB 必须真的在 rootfs 里(同 aarch64)：缺失通常意味着 rootfs 是旧的,
> 在这里明确失败比让 guest 的 CPU_ON 等一个不存在的 hart 好查得多。

**第 261 行**

> DTB 必须真的在 rootfs 里。这一步让“旧 rootfs + 新内核”在启动前就以
> 明确原因失败(vmm_dev 返回 EINVAL), 而不是加载到一半才发现缺文件,
> 再让 helper 把 EIO 当成瞬态错误重试 2000 次。

</details>

**位置**：`kernel/vmm/loader/guest_loader.c:205`

<a id="fn-kernel-vmm-loader-guest_loader.c-301"></a>

### `guest_loader_run_linux`

```c
int guest_loader_run_linux(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验并补默认 vCPU 数，x86 走 x86_guest_boot；否则确认 LOADING 后建 VM，加载内核/DTB/initrd、检查 initrd 越界、打补丁屏蔽节点并写回 DTB。

<details>
<summary>函数内备注（22 条）</summary>

**第 306 行**

> vCPU 数必须在 vm_create() 之前定下来：vGIC, GICR 窗口和 vcpu 数组都是
> 按 nr_vcpus 初始化的。调用方(/dev/vmm 的 BOOT_CFG 或直启路径)可以
> 显式指定；&lt;=0 表示用本构建的默认值 GUEST_LINUX_NR_VCPUS。

**第 321 行**

> 重入保护：调用者(/dev/vmm 或直启路径)已经从 VM 池里拿到一个槽位并
> 置成 LOADING, 这里只需确认它可用。(从前是一个全局 &quot;guest 在跑吗&quot;,
> 那在多 VM 下会拦住第二个 VM。)

**第 341 行**

> ── 建立 VM(必须在加载映像之前)──────────────────────────

> stage-2 是在 vm_create() 里初始化的 —— 一张空表。而加载映像要往
> guest 物理地址里写, 那需要映射。按需分页下 guest_loader_write_guest() 会为每一页
> 现分配, 所以这里不再需要预先预留物理内存或整片清零：

> pmm_mark_allocated(192 MiB)   ← 删掉了
> memset(192 MiB)               ← 删掉了

> 从前那两行既是&quot;只能有一个 VM&quot;的根源(第二份无处安放, memset 会抹掉
> 第一个 VM 的内存), 也让每个 VM 无论用多少都硬吃 192 MiB。现在宿主
> 只为 guest 真正碰过的页付内存, 且两个 VM 的页天然隔离。

**第 361 行**

> 1. 加载 kernel Image

**第 367 行**

> 2. DTB：读进宿主缓冲再打补丁, 最后一次性写回 guest。

> 不能像从前那样直接对 guest 内存 `dtb[i]`：补丁代码按线性偏移索引 FDT,
> 而按需分页下 4 KB 的 DTB 可能落在不连续的物理页上 —— 越过页边界继续
> 线性索引就会写到宿主或别的 VM 的内存里。

**第 391 行**

> 3. 加载 initrd

**第 392 行**

> 注意： 先查大小, 再加载 —— 顺序不能反。

> guest_loader_load_file() 是边读边把页映射进 stage-2 的, 等加载完再
> 发现越界时, 越界那一段 IPA 已经被 initrd 的宿主页覆盖了。而 RK3588 上
> initrd 后面的那块地址正是 NPU 的恒等窗口 —— 后果不是&quot;initrd 被截断&quot;
> 这种看得见的症状, 而是 guest 驱动拿着一个&quot;物理地址&quot;去做 DMA, NPU 读到
> initramfs 的内容。全程没有任何一处报错, 只是推理结果不对。

**第 419 行**

> 打不开就不在这里报错 —— 下面 load_file 会报一条更具体的

**第 429 行**

> ── 屏蔽 VMM 未模拟的 DTB 设备节点 ─────────────────────

> VMM 只模拟了 PL011 与 GICD。其余设备一旦被 guest 访问就会 Stage-2
> fault, 而 exit handler 在 MMIO 总线未命中时按「权限故障」处理
> (stage2_restore 后让 guest 重试)→ 立刻再次 fault → 死循环。

> 实测：guest 的 GIC 初始化会探测 &quot;v2m@8020000&quot;(GICv2M MSI 帧),
> 因未模拟而卡死在 &quot;Root IRQ handler: gic_handle_irq&quot; 之后。

> 故启动前直接把这些节点从 DTB 中摘除(替换为 FDT_NOP), 使 guest
> 根本不去枚举它们。移植自 kvmm loader.rs 的 nop_dtb_nodes。

**第 447 行**

> 补好的 DTB 一次性写进 guest(memory/bootargs/initrd 都改完了)

**第 451 行**

> ── NPU 预留池的 `no-map` 自检 ─────────────────────────────────

> 只在真机上会出事, 而症状完全不在 DTB 上：guest 里 rknpu 照样 probe
> 成功(`[drm] Initialized rknpu ...`), 只是作业永远超时,
> `task counter: 0`。真实链条是

> DTB 少了 no-map → 那块内存仍是 System RAM
> → dma_init_coherent_memory() 写死的 memremap(WC) 对 System RAM
> 告警并 return NULL(kernel/iomem.c:106)
> → of_reserved_mem_device_init() 失败, NPU 拿不到预留池
> → 驱动退回默认池, 而那不在恒等窗口里
> → NPU 按物理地址读到垃圾 regcmd → 作业跑不起来

> 这个坑在真机上连着卡了三轮, 每轮都要从 guest 的一条 WARNING 往回推。
> 而 DTB 就在宿主手里 —— 所以在这儿一次性喊出来。

> 判据：no-map 是零长度属性, 它的名字只会在 strings 块里出现一次,
> 而且只有用到它的节点才会把它带进来。所以&quot;strings 块里没有 no-map&quot;
> 就等价于&quot;这份 DTB 里没有任何节点声明 no-map&quot; —— 对本项目来说就是
> NPU 池那份丢了。

**第 473 行**

> #if ARCH_AARCH64：GUEST_LINUX_NPU_DMA_SIZE 只有 aarch64 定义,
> 而 NPU 透传本身就是 aarch64/RK3588 专有的。

**第 498 行**

> 5. 配置 vCPU 的入口状态

> 必须在 vm_create() 之后：两个架构的 vm_init 都会 memset 整个
> vcpu 数组, 先写会被清掉。

**第 507 行**

> x0 直接写 vcpu-&gt;r[0]：el2_vmcs.S 在 eret 前用 `ldr x0, [x0, #0]`
> 最后加载 guest x0(VCPU_R0 偏移 0), 因此无需改动汇编。
> ARM64 boot 约定：x0 = DTB 物理地址, PSTATE = EL1h 且 DAIF 屏蔽。

**第 513 行（行尾）**

> EL1h, DAIF 屏蔽

**第 522 行**

> RISC-V Linux boot 协议(Documentation/riscv/boot.rst)：
> pc = Image 起始(按 2 MiB 对齐装入)
> a0 = boot hartid
> a1 = DTB 物理地址
> 其余寄存器无约定；sp 由内核自己在 head.S 里设好, 这里给个合法值
> 只是为了「万一它在设 sp 之前先出异常」时不至于落到 0。

> vsatp = 0(bare)：不能沿用宿主 satp —— guest 是独立地址空间,
> 由 Linux 自己在 head.S 里建页表再写 satp。沿用宿主页表的话, guest
> 一取指就会按宿主的内核映射跑, 行为完全不可预期。

> vstvec 也留 0：Linux 在 head.S 很早就会 csrw stvec 换成自己的
> trap vector；在此之前不让任何中断挂起(vsie=0, hvip 的注入发生在
> 每次入口, 由 vmm_arch_restore_guest_ctx 计算)。

**第 538 行（行尾）**

> 恢复 PC = sret 目标

**第 539 行（行尾）**

> guest 自己的 sepc

**第 546 行（行尾）**

> a0

**第 547 行（行尾）**

> a1

**第 553 行**

> 6. 创建 vCPU 任务

> AP 任务先建：它们一启动就睡在 vcpu-&gt;start 这个 completion 上,
> 等 guest 通过 PSCI CPU_ON 放行。BSP 最后建并立刻进入 guest。
> 这样 Linux 在 BSP 上开始跑的时候, CPU_ON 要唤醒的任务已经存在。

**第 576 行（行尾）**

> !ARCH_X86_64

</details>

**位置**：`kernel/vmm/loader/guest_loader.c:301`

<a id="mod-kernel-vmm-loader-guest_mem.c"></a>

## kernel/vmm/loader/guest_mem.c — 宿主访问 guest 内存的唯一入口

`kernel/vmm/loader/guest_mem.c` · 4 个函数 · 4 个有说明 · 199 行

从 guest_loader.c 拆出来的。按需分页之后, guest RAM 在宿主侧不再是
identity 映射的一段连续内存, 所以任何按 GPA 的读写都必须经这里现算
宿主地址(必要时现分配 + 建 stage-2 映射)。

拆出来还顺带解开一个跨 TU 循环依赖：guest_loader.c 就地 extern 并调用
x86_guest_boot(), 而 guest_boot.c 反过来依赖本文件的 guest_loader_write_guest()。

原型都在 include/vmm/guest_loader.h(公共头)。

<a id="fn-kernel-vmm-loader-guest_mem.c-45"></a>

### `guest_loader_gpa_ptr`

```c
void *guest_loader_gpa_ptr(vm_t *vm, uint64_t gpa, int alloc)
```

── GPA → 宿主可写地址(按需分页下唯一的入口)────────────────

注意： 从前这里到处是 `phys_to_virt(gpa)` —— 因为 guest RAM 是 identity 映射
(GPA == PA), 随手一算就能写。按需分页之后这两条前提都没了：
- 同一个 GPA 在不同 VM 里指向不同的物理页(隔离就靠这个)；
- 页面可能还没分配(首次访问才由缺页处理补上)。
所以宿主代码要碰 guest 内存, 一律走这里。

@alloc: 允许在未映射时现分配一页(加载映像时为 1；只想看看时为 0)。
返回 NULL 表示"没映射且不允许分配"或"PMN 没页了"。

<details>
<summary>函数内备注（8 条）</summary>

**第 47 行**

> 注意： 两条路径都必须把页内偏移加回去。

> map_* 系列返回的是页基址(它们只负责把 gpa 所在的页映射好), 而
> lookup 系列返回的地址里已经带了偏移(`(entry &amp; ~0xFFF) | (gpa &amp; 0xFFF)`)。
> 早先 map 那条路忘了加, 症状极具误导性：
> - 顺序装载, 且起点页对齐时完全正常 —— 一页里第一次写 off==0,
> 之后同一页的写都走 lookup 分支(已经映射了), 偏移是对的；
> - 而起点不在页边界的一次性写入(比如 x86 的 MP 表, GPA 0x9F800)
> 会把整段数据写到页首去, 目标位置留下一片零, 且没有任何报错。
> aarch64/riscv 的 guest 镜像恰好都是页对齐顺序装载, 所以一直没暴露。

**第 67 行（行尾）**

> zero

**第 70 行（行尾）**

> 加载期分配的页(与缺页驱动的 nr_fault 区分统计)

**第 80 行（行尾）**

> zero

**第 83 行（行尾）**

> 加载期分配的页(与缺页驱动的 nr_fault 区分统计)

**第 93 行（行尾）**

> zero

**第 96 行（行尾）**

> 加载期分配的页(与缺页驱动的 nr_fault 区分统计)

**第 99 行**

> 还没有 stage-2 的架构(vmm_test 的玩具 guest)：identity 映射

</details>

**位置**：`kernel/vmm/loader/guest_mem.c:45`

<a id="fn-kernel-vmm-loader-guest_mem.c-124"></a>

### `guest_loader_write_guest`

```c
int guest_loader_write_guest(vm_t *vm, uint64_t gpa, const void *src, size_t n)
```

── 页感知的 guest 内存访问族 ───────────────────────────────

注意： 不要对 guest_loader_gpa_ptr() 的返回值做跨页的指针算术。

identity 映射时代 `phys_to_virt(gpa)` 是线性的, 于是 `p + off`(off 超过
一页)恰好就是"gpa + off 那一页"—— 很多代码靠这个跨页 memcpy/memmove。
改成按需分页之后那条不变量没了：每个 guest 页都是各自从 PMM 分配的,
物理上不连续, `p + off` 走到的是宿主物理内存里的下一页, 而不是 guest
的下一页。表现是"数据搬过去了一部分, 另一部分是宿主的随机内容", 且没有
任何报错。

x86 的 bzImage 装载就栽在这里：11 MB 的保护模式内核用一句 memmove 搬运,
搬完 guest 的 GDT(在 payload 末尾 0xAA7B40)是垃圾, `lgdt` 之后
`mov %ax,%ds` 直接 #GP → IDT 还没建 → triple fault。

所以跨页的一律走下面这几个：它们逐页翻译, 逐页搬。

**位置**：`kernel/vmm/loader/guest_mem.c:124`

<a id="fn-kernel-vmm-loader-guest_mem.c-151"></a>

### `guest_loader_read_guest`

```c
int guest_loader_read_guest(vm_t *vm, uint64_t gpa, void *dst, size_t n)
```

从 guest 内存读一段到宿主缓冲(逐页)。

<details>
<summary>函数内备注（1 条）</summary>

**第 163 行（行尾）**

> 只读, 不分配

</details>

**位置**：`kernel/vmm/loader/guest_mem.c:151`

<a id="fn-kernel-vmm-loader-guest_mem.c-178"></a>

### `guest_loader_fill_guest`

```c
int guest_loader_fill_guest(vm_t *vm, uint64_t gpa, int byte, size_t n)
```

往 guest 内存填一段字节(逐页)。

**位置**：`kernel/vmm/loader/guest_mem.c:178`

<a id="mod-kernel-vmm-loader-image_load.c"></a>

## kernel/vmm/loader/image_load.c — 从 rootfs 往 guest 内存装载文件/区间

`kernel/vmm/loader/image_load.c` · 5 个函数 · 5 个有说明 · 222 行

从 guest_loader.c 拆出来的。它是「VFS 读 → 逐块写 guest 内存」这条通路,
与 DTB 补丁无关；DTB 那部分在 fdt.c。

静态加载缓冲(LOAD_CHUNK / g_load_buf)只被这一组使用, 所以跟着搬过来 ——
它本来也不该由 share 整个文件的那个 TU 持有。

原型都在 include/vmm/guest_loader.h(公共头)。

<a id="fn-kernel-vmm-loader-image_load.c-37"></a>

### `guest_loader_read_file_range`

```c
int guest_loader_read_file_range(const char *path, uint64_t file_off, void *buf, size_t len)
```

guest_loader_read_file_range — 把文件的 [file_off, file_off+len) 读进宿主缓冲

与 guest_loader_read_file() 的区别：能从中间读。bzImage 要用它先取头部
(解析 setup_sects / pref_address), 再决定各段装到哪。
len == 0 表示读到文件尾。返回读到的字节数(可能少于 len), 失败返回 -1。

<details>
<summary>函数内备注（2 条）</summary>

**第 49 行（行尾）**

> SEEK_SET

**第 71 行（行尾）**

> 文件尾

</details>

**位置**：`kernel/vmm/loader/image_load.c:37`

<a id="fn-kernel-vmm-loader-image_load.c-84"></a>

### `guest_loader_file_size`

```c
int guest_loader_file_size(const char *path)
```

guest_loader_file_size — 取文件大小(字节), 失败返回 -1

注意： 不要用 bzImage 头里的 `syssize`：那是 16 位时代以 16 字节为单位的旧账,
对现代 bzImage 不准。seek 到末尾最可靠。

<details>
<summary>函数内备注（1 条）</summary>

**第 93 行（行尾）**

> SEEK_END

</details>

**位置**：`kernel/vmm/loader/image_load.c:84`

<a id="fn-kernel-vmm-loader-image_load.c-114"></a>

### `guest_loader_load_range`

```c
int guest_loader_load_range(vm_t *vm, const char *path, uint64_t file_off, uint64_t gpa, uint64_t len)
```

guest_loader_load_range — 把文件的 [file_off, file_off+len) 直接装到 guest 的 gpa

len == 0 表示读到文件尾。

注意： 这是 x86 bzImage 装载的首选方式, 别再用"整个文件装到暂存区再搬"那套。
原因有二：
1. 那段搬运在按需分页下必须走逐页的 guest→guest 拷贝(见
guest_loader_write_guest 的说明), 凭空多一层容易出错的机制；
2. 直接按最终地址装载, 连 11 MB 的临时副本都不用, 启动更快。
两条装载区间(setup 与保护模式内核)本来就是分开的, 天然没有重叠。

返回装上的字节数, 失败返回 -1。

<details>
<summary>函数内备注（2 条）</summary>

**第 119 行（行尾）**

> O_RDONLY

**第 153 行（行尾）**

> 文件尾

</details>

**位置**：`kernel/vmm/loader/image_load.c:114`

<a id="fn-kernel-vmm-loader-image_load.c-160"></a>

### `guest_loader_load_file`

```c
int guest_loader_load_file(vm_t *vm, const char *path, uint64_t gpa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环按块读文件并写入 guest GPA，累计字节数，结束时关闭文件并返回总长度或 -1。

<details>
<summary>函数内备注（3 条）</summary>

**第 165 行（行尾）**

> O_RDONLY

**第 177 行**

> 写进 guest 内存：按需分页下必须经 guest_loader_write_guest(见它的注释)

**第 187 行（行尾）**

> 读到文件尾

</details>

**位置**：`kernel/vmm/loader/image_load.c:160`

<a id="fn-kernel-vmm-loader-image_load.c-206"></a>

### `guest_loader_read_file`

```c
int guest_loader_read_file(const char *path, uint8_t *buf, size_t cap)
```

把文件读进宿主缓冲(不碰 guest 内存)。

DTB 补丁必须走这条路：补丁代码是按线性偏移索引 FDT 的, 而在按需分页下
一个 4 KB 的 DTB 可能落在不连续的物理页上 —— 直接对 guest 内存做
`dtb[i]` 会写坏宿主或别的 VM。所以：读到宿主缓冲 → 在缓冲上打补丁 →
一次 guest_loader_write_guest() 写回去。

返回读到的字节数；缓冲不够放下整个文件时返回 -1(宁可不启动也不能截断
DTB —— 截断的 FDT 会让 guest 解析到垃圾)。

<details>
<summary>函数内备注（1 条）</summary>

**第 213 行**

> 缓冲满了但文件可能还没读完 —— 再探一个字节

</details>

**位置**：`kernel/vmm/loader/image_load.c:206`

<a id="mod-kernel-vmm-mmio_bus.c"></a>

## kernel/vmm/mmio_bus.c — VMM MMIO 设备总线实现

`kernel/vmm/mmio_bus.c` · 3 个函数 · 3 个有说明 · 71 行

移植自 x-kernel: virt/vdev/core/src/lib.rs (MmioBus)。
线性查找：设备数很少(<= MMIO_MAX_DEVICES), 无需哈希。

<a id="fn-kernel-vmm-mmio_bus.c-18"></a>

### `mmio_bus_init`

```c
void mmio_bus_init(mmio_bus_t *bus)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清零 MMIO 总线结构体，设备计数与设备表全部复位。

**位置**：`kernel/vmm/mmio_bus.c:18`

<a id="fn-kernel-vmm-mmio_bus.c-23"></a>

### `mmio_bus_register`

```c
int mmio_bus_register(mmio_bus_t *bus, mmio_device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

设备数未达上限时把设备追加进数组并打印名称、基址与大小，已满则报错返回 -1。

**位置**：`kernel/vmm/mmio_bus.c:23`

<a id="fn-kernel-vmm-mmio_bus.c-35"></a>

### `mmio_bus_handle`

```c
int mmio_bus_handle(mmio_bus_t *bus, uint64_t gpa, int is_write, uint8_t size, uint64_t value, uint32_t vcpu_id, uint64_t *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 GPA 遍历设备地址范围，命中后按读写调用带 vcpu 或不带 vcpu 的回调，读值写入 out 并返回 1，未命中返回 0。

<details>
<summary>函数内备注（2 条）</summary>

**第 68 行（行尾）**

> 命中

**第 70 行（行尾）**

> 无设备命中

</details>

**位置**：`kernel/vmm/mmio_bus.c:35`

<a id="mod-kernel-vmm-riscv64-hext_run.c"></a>

## kernel/vmm/riscv64/hext_run.c — RISC-V H-extension 架构钩子实现

`kernel/vmm/riscv64/hext_run.c` · 35 个函数 · 35 个有说明 · 1483 行

实现 vmm.h 声明的四个架构钩子：
vmm_arch_restore_guest_ctx  — 进入 guest 循环前初始化 VS-CSRs
vmm_arch_enter_guest        — 调用 hext_enter_guest(sret → VS-mode)
vmm_arch_exit_handler       — 处理 VS-mode → HS-mode 陷阱
vmm_arch_save_guest_ctx     — guest 退出后清理(当前无操作)

以及 RISC-V VMM 初始化：
vmm_arch_vm_init(vm_t *)        — 检查 H-ext 支持, 配置 hedeleg/hideleg/hstatus
hext_vcpu_setup(vcpu_t *, entry) — 初始化 vcpu 软件 VMCS

特权级关系：
HS-mode (本内核) → VS-mode (guest) → VU-mode (guest user, 本例不用)
VS-mode ecall → scause=10(区别于 U-mode ecall = 8)
WFI 在 hstatus.VTW=1 时：VS-mode 执行 WFI 触发 scause=2(虚拟指令异常)
注：部分 QEMU 版本报 scause=2, 另一些直接报 illegal instruction；
本 exit handler 两种都处理。

<a id="fn-kernel-vmm-riscv64-hext_run.c-74"></a>

### `vmm_riscv_host_tick`

```c
void vmm_riscv_host_tick(void)
```

vmm_riscv_host_tick — 宿主 tick 钩子（由 RISC-V timer ISR 弱调用）

唤醒所有正在 WFI 的 vCPU，让它们重新检查 guest 定时器截止时间。
频率就是宿主 tick(100Hz)，足够覆盖 guest 的 250Hz 虚拟定时器吗？
不够精确，但 WFI 只在 guest 明确"没事可做"时发生；real 截止时间
由下一次宿主 tick 补上，最坏 10ms 的唤醒延迟。要更细可以把它改成
按 deadline 定时，但当前代价/收益不划算。

**位置**：`kernel/vmm/riscv64/hext_run.c:74`

<a id="fn-kernel-vmm-riscv64-hext_run.c-99"></a>

### `hext_check_support`

```c
static int hext_check_support(void)
```

直接查 misa.H(bit 7), 而不是「试探性读一下 hstatus, 没炸就算有」。

老写法在没有 H 扩展的 CPU 上表现极差：csrr hstatus 触发 illegal
instruction → 走宿主异常处理 → 通常直接 panic 在启动路径上, 报出来的是
一条和虚拟化毫无关系的非法指令, 排查方向全错。

实测(QEMU 8.2.2 + OpenSBI v1.3, `-machine virt` 且不传 -cpu)：
misa 里 H 位是置上的(OpenSBI 打印 "Boot HART Base ISA : rv64imafdch"),
所以默认配置就能用。换 QEMU 版本/CPU 型号后如果这里报错, 加
`-cpu rv64,h=true` 即可。

<details>
<summary>函数内备注（2 条）</summary>

**第 101 行**

> 先打一行再读 —— 万一读炸了, 这行就是日志里的最后一句,
> 配合紧随其后的 backtrace(会指向 vmm_arch_vm_init)足以定位。
> 没有 H 扩展时读 hstatus 必触发 illegal instruction, 而 S-mode
> 没有任何办法捕获它(内核的异常处理只会 panic), 所以只能这样。

**第 111 行**

> 能执行到这儿就说明 hstatus 可读 ⇒ CPU 带 H 扩展。
> (没有 H 时 0x600 是未定义 CSR, 上一条 csrr 已经陷入并 panic。)

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:99`

<a id="fn-kernel-vmm-riscv64-hext_run.c-135"></a>

### `hext_vcpu_setup`

```c
int hext_vcpu_setup(vcpu_t *vcpu, void (*entry)(void))
```

── 每 VM 的 MMIO 总线, 虚拟设备与 guest 栈 ────────────────

这三样从前都是文件级 static(g_rv_mmio_bus / g_rv_uart_dev / g_rv_plic_dev
/ g_guest_stack), 整机只有一份 —— 第二个 VM 的 vmm_arch_vm_init() 会在同一个
总线上重复注册设备, 覆盖第一个 VM 的设备实例。现在它们都住在 vm_t 里
(见 include/vmm/vmm.h 的 ARCH_RISCV64 段)：

vm->mmio_bus_storage  总线本体        (通用字段, aarch64/x86 也有)
vm->guest_stack[]     给 hext_vcpu_setup 用的 guest 栈

设备的 state / 锁 / 设备对象都不在 vm_t 里了 —— 它们按 vm->slot 住在
kernel/vmm/vdev/ 下各设备自己的静态池(见 vpl011/uart16550/vplic 的槽位说明)。

<details>
<summary>函数内备注（7 条）</summary>

**第 139 行**

> guest PC = entry(内核虚拟地址, 与 host 共享地址空间, 无 hgatp)
> pc    = 恢复 PC(sret 目标, 只写回 HS sepc)
> vsepc = guest 自己的 sepc, VMM 不碰；这里给同值只是让 guest 万一在
> 首次陷阱前读 sepc 时不至于看到 0。

**第 147 行**

> vsstatus：SPP=0(guest 运行在 VS-mode), SIE=0(中断关闭)
> VS-mode 的 sstatus.SPP 在 vsstatus.SPP 处, bit 8

**第 152 行**

> guest trap vector：暂用 entry 作为 guest stvec

**第 155 行**

> vsscratch = 0(guest 初始值)

**第 158 行**

> vsatp：与 host 共享内核页表, guest 可执行 kernel VA 地址的代码
> vsatp=0(bare)时 guest 会把 kernel VA 当物理地址使用, 立即 page fault

**第 163 行**

> vsie = 0：guest 中不使能任何中断

**第 166 行**

> guest 栈：r[2] = sp。栈从本 VM 的 vm_t 里取 —— 从前是按 vcpu_id 索引
> 的文件级数组, 两个 VM 的 vcpu0 会共用同一块栈。

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:135`

<a id="fn-kernel-vmm-riscv64-hext_run.c-177"></a>

### `vmm_arch_vm_init`

```c
int vmm_arch_vm_init(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

检查 H 扩展，写 hedeleg/hideleg/hie/hcounteren 与 hstatus.VTW，初始化 vCPU；有独立内存时建 G-stage 空表并注册 16550A 与 vPLIC。

<details>
<summary>函数内备注（11 条）</summary>

**第 187 行**

> 1. 检查 H-ext 支持

**第 193 行**

> 2. 异常/中断委托

> hedeleg：把「VU-mode 自己该处理」的异常交给 guest 内核(VS-mode)处理。
> 不委托的话, guest 用户进程每次缺页都会陷到 HS-mode, 而我们并没有
> 把 trap 反射回 VS 的代码 —— 结果是 guest 用户态一跑就死。
> Linux 的 initramfs 起来后 /init 一执行就要缺页, 所以这一条是必需的。

> HEDELEG_COMMON 里不含 bit2(illegal instruction)：hstatus.VTW=1
> 时 VS-mode 的 WFI 报 virtual instruction(cause 22), 必须陷到 HS-mode
> 才能被换成宿主 yield —— 委托出去就再也看不见了。

> hideleg：VS 定时器/外部中断委托给 VS-mode, 这样 hvip.VSTIP/VSEIP
> 置起后由 guest 自己的 vsie 决定何时接收, 而不是每置一次就陷一次
> HS-mode。

> hie：置 VSTIE|VSEIE, 与 x-kernel hext_init() 的
> `csrs hie, HIE_VSEIE | HIE_VSTIE` 一致。它管的只是「已委托给
> VS 的中断, 在 guest 当时接不了时(例如它正跑在自己的陷阱处理程序
> 入口, vsstatus.SIE=0)怎么办」——置位则升到 HS 让 VMM 抢断,
> VMM 让宿主跑一段后 resume。留 0 的话这类中断会一直挂在 hvip 里,
> guest 的定时器从此不再推进(实测 guest 完全起不来)。

> 抢断是安全的, 前提是「恢复 PC」与「guest 的 vsepc」分开存放
> (VCPU_PC vs VCPU_VSEPC)—— 见 hext_vcpu.S 陷阱向量第 3 节。

> 历史注记：本文件曾写着「QEMU 8.2 上写这两个 CSR 会 illegal instruction」,
> 因此长期留着默认值 0。实测是误判 —— binutils 认这两个符号名, QEMU 也
> 正常接受写入。真正会让 guest 用户态崩溃的是不委托 hedeleg。

**第 228 行**

> 3. hcounteren：允许 guest 直接执行 rdtime/rdcycle/rdinstret。
> 不给的话 guest 读 time 会非法指令 —— 而 Linux RISC-V 的时钟源和
> delay 循环都直接读 time, 起不来。

**第 235 行**

> 4. hstatus.VTW=1：WFI 在 VS-mode 陷入 HS-mode
> (等价 AArch64 的 HCR_EL2.TWI=1), 由 handle_wfi 换成宿主 yield。

**第 244 行**

> 5. 初始化 vCPU 数组

**第 251 行（行尾）**

> 还没跑过：0 是合法 CPU, 不能拿 0 当&quot;未运行&quot;

**第 260 行**

> hstatus_save 是「上次陷入时的 hstatus」快照, 入口靠它还原
> hstatus.SPVP 来决定 sret 回 VS 还是 VU。第一次进入 guest 时
> 还没有任何陷入, 快照必须是 0 —— 那会让 SPVP=0, sret 进 VU-mode,
> guest 的内核代码在用户态取指/执行特权指令, 直接全线非法指令。

> 所以这里预置成 SPVP=1(VS-mode)：第一次进入落在 guest 内核。
> 这条与 hext_vcpu.S 第 7/8 步是配套的, 改那边必看这里。

**第 273 行**

> 6. G-stage(hgatp Stage-2)内存隔离：仅当 VM 请求了独立 guest RAM
> (cfg.mem_size != 0)时启用。移植自 x-kernel gstage.rs。

> 默认(mem_size==0)保持原「共享 host 地址空间, 无 hgatp」路径不变,
> 即当前 vmm_test 使用的已验证行为, 避免回归。

> 注意： 建出来的是空表(&quot;空表即全 trap&quot;)：guest RAM 不再预映射,
> 首次访问才由缺页处理分配物理页并装映射。宿主自己要写的那几块
> (内核映像/DTB/initrd/初始栈)由 guest_loader 用
> rv_gstage_map_range() 显式映射 —— 与 aarch64 完全同构。

**第 287 行**

> 建立 MMIO 总线并注册虚拟 16550A 控制台 + 虚拟 PLIC

**第 290 行**

> 注册失败 = 静默死控制台, 必须让建 VM 失败(同 aarch64)

**第 295 行**

> 注册失败 = 静默死设备, 必须让建 VM 失败(同控制台)

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:177`

<a id="fn-kernel-vmm-riscv64-hext_run.c-318"></a>

### `vmm_arch_vm_destroy`

```c
void vmm_arch_vm_destroy(vm_t *vm)
```

vmm_arch_vm_destroy — 与 vmm_arch_vm_init 严格逆序

顺序与 init 相反：init 是 uart16550 → vplic, 这里是 vplic → uart16550。

<details>
<summary>函数内备注（1 条）</summary>

**第 320 行**

> 先摘掉 WFI 登记：tick 钩子可能还在别的 hart 上跑

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:318`

<a id="fn-kernel-vmm-riscv64-hext_run.c-347"></a>

### `vcpu_timer_irq_on_entry`

```c
static void vcpu_timer_irq_on_entry(vcpu_t *vcpu)
```

vcpu_timer_irq_on_entry — 进入 guest 前注入 VS 定时器中断

对标 x-kernel vdev/riscv64/timer.rs 的 RiscvTimerHook::on_entry：
guest 经 SBI 设置了截止时间且已到期 → 置 hvip.VSTIP, 使 VS-mode 看到
定时器中断挂起(无需 vCLINT/ACLINT)。

客人时间与宿主时间同源：两边 timebase 都是 10 MHz(见 platform.conf 的
counter_hz 与 guest DTB 的 timebase-frequency), 所以 htimedelta 保持 0,
guest 的 rdtime 与宿主 READ_TIME() 直接可比。换平台配置时要一起改,
否则 guest 的时钟会按错误的倍率走。

**位置**：`kernel/vmm/riscv64/hext_run.c:347`

<a id="fn-kernel-vmm-riscv64-hext_run.c-364"></a>

### `vcpu_external_irq_on_entry`

```c
static void vcpu_external_irq_on_entry(vcpu_t *vcpu)
```

vcpu_external_irq_on_entry — 进入 guest 前把 vPLIC 的可投递中断同步给 hvip

与定时器同理, 也跟 AArch64 的 aarch64_check_vpl011_rx() 同构：
外部中断是电平触发 —— 只要 PLIC 里还有「已使能, 未 active, 优先级
高于阈值」的中断, 每次入口都要重新置 hvip.VSEIP。只在设备侧 push 时置
一次不行：guest claim 时 vPLIC 会清掉 pending 位, 而设备侧(比如 UART 的
RX FIFO 里还有字节)可能仍然是有效的。

<details>
<summary>函数内备注（1 条）</summary>

**第 366 行**

> 设备侧先同步进 PLIC：控制台 RX 有数据/待发送 → 置对应中断源

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:364`

<a id="fn-kernel-vmm-riscv64-hext_run.c-385"></a>

### `vcpu_software_irq_on_entry`

```c
static void vcpu_software_irq_on_entry(vcpu_t *vcpu)
```

vcpu_software_irq_on_entry — 把软件 pending(SBI send_ipi)同步成 hvip.VSSIP

hvip 是**每 hart 一份**的 HS CSR：远端 vCPU 发 IPI 时只能写目标 vCPU
的软件状态, 真正写进硬件必须等目标自己下一次入口 —— 这就是「先置
pending, 再 kick, 目标退出后重新入口时注入」那条链的最后一环。

同时记下 ipi_seq 的入口快照与「本次是否注入过」, 供 exit 时区分
「guest 消费掉了」与「运行期间又来了新 IPI」(见 vmm_arch_save_guest_ctx)。
本函数与发送侧共用 vcpu->irq_lock(vmm_run_vcpu 已持锁调用)。

**位置**：`kernel/vmm/riscv64/hext_run.c:385`

<a id="fn-kernel-vmm-riscv64-hext_run.c-402"></a>

### `vcpu_remote_fence_on_entry`

```c
static void vcpu_remote_fence_on_entry(vcpu_t *vcpu)
```

vcpu_remote_fence_on_entry — 执行 SBI RFENCE 记下的远端失效

必须在 hext_per_hart_csrs_ensure() 之后：hfence.vvma 刷的是「当前
hgatp.VMID 的 G-stage 对 VS-stage 翻译的缓存」, hgatp 没切到本 VM 时
刷的是别人。也必须在 sret 进 guest 之前 —— 否则 guest 可能带着旧
翻译跑过整个时间片。

<details>
<summary>函数内备注（1 条）</summary>

**第 414 行**

> 本函数在 vcpu-&gt;irq_lock 下运行(与发送侧同一把锁), 所以这里可以
> 直接清位。若发送恰好发生在执行之后, 它会在 unlock 后置新位并 kick,
> 下一次入口再执行一次 —— 只会多刷, 不会漏刷。

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:402`

<a id="fn-kernel-vmm-riscv64-hext_run.c-429"></a>

### `vmm_arch_irq_raise`

```c
void vmm_arch_irq_raise(vcpu_t *vcpu, virq_t irq)
```

vmm_arch_irq_raise — RISC-V 的「拉线」适配器(见 include/vmm/vmm_virq.h)

PLIC 的 pending 是源级全局的：同一个位置进所有 context, 可见性由每个
context 自己的 enable/threshold 过滤(见 vplic.c 的说明)。所以这里没有
vcpu_id 这个维度 —— 不是省略, 是语义上不存在。

<details>
<summary>函数内备注（1 条）</summary>

**第 434 行**

> 不做任何使能判断：投递时的 enable/threshold 过滤在 vplic_next_deliverable 里

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:429`

<a id="fn-kernel-vmm-riscv64-hext_run.c-459"></a>

### `hext_per_hart_csrs_ensure`

```c
static void hext_per_hart_csrs_ensure(vcpu_t *vcpu)
```

hext_per_hart_csrs_ensure — 把「每 hart 一份」的 HS 级 CSR 补齐

vmm_arch_vm_init() 是在 helper(/bin/vmm-run)所在的 hart 上跑的, 而 vCPU
任务钉在 hart0 上；SMP>1 时两者不是同一颗。下面这些 CSR 都只写了一处
(init), 于是 vCPU 跑在另一颗 hart 上时它们全是复位值：
hgatp            = Bare → G-stage 形同关闭(penalty：guest 直接访问
宿主物理地址, MMIO 也不再陷入)
hedeleg/hideleg/hie   → guest 的缺页, VS 定时器/外部中断全不委托
hcounteren       → guest 读 time 直接非法指令(Linux 的时钟源就是它)
hstatus.VTW      → VS-mode 的 WFI 不再陷入, 宿主 yield 路径失效
症状与 x86 那两个「每核状态只在别的核上初始化」的坑完全一样：SMP=1 正常,
SMP>1 概率性起不来(helper 落在哪颗 hart 决定), 所以必须在每次进 guest 前
在真正跑 vCPU 的 hart 上重设一遍。

注意： 多 VM 之后这里还要多一层：hgatp 不只是 per-hart, 还是per-VM 的。
同一颗 hart 上时间片轮转两个 VM 的 vCPU 任务时, 每次入口都得把 hgatp 换回
本 VM 的(否则 guest 会拿着另一个 VM 的 G-stage 跑, 症状是"两个 VM 互相
看见对方的内存"这种最不该出现的故障)。所以本函数必须收 vcpu 而不是
不收参数 —— 见下面的 rv_gstage_activate(&vcpu->vm->gstage)。

<details>
<summary>函数内备注（2 条）</summary>

**第 470 行**

> hgatp 换成本 VM 的。幂等：已经是这个值就不写, 也不刷 TLB(进 guest 的
> 常态路径因此只有一次 csrr)。

**第 477 行**

> 这里**不做**「按映射代数全刷」。兄弟 vCPU 刚在别的 hart 上装的映射
> 不需要本 hart 主动失效：RISC-V 允许缓存「不可翻译」结果, 而那种缓存
> 只会让本 hart 再 fault 一次, 缺页处理里的 rv_gstage_tlb_flush() 会
> 在本地刷掉。映射建立后不会改指向, 所以不存在「仍有效但过时的正翻译」。
> (曾按代数每轮入口全刷: 4 vCPU 并行启动时几乎每轮都命中, 启动从
> 秒级退化到几十秒 —— 这是实测数据, 别再把它加回来。)

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:459`

<a id="fn-kernel-vmm-riscv64-hext_run.c-487"></a>

### `vmm_arch_restore_guest_ctx`

```c
void vmm_arch_restore_guest_ctx(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

补齐每 hart 的 HS 级 CSR 与本 VM 的 hgatp，落地远端 fence，并同步定时器、外部和软件中断。

<details>
<summary>函数内备注（2 条）</summary>

**第 489 行**

> VS-CSRs 由 hext_enter_guest 汇编逐次恢复, 这里做的是「每次入口」的
> 两件事：每 hart 的 HS 级 CSR 补齐(含本 VM 的 hgatp), 以及
> 「设备 → 虚拟中断控制器」的同步。都必须在每次入口做, 不能只在
> 启动时做一次。

**第 497 行**

> 远端 SBI RFENCE 必须在 hgatp 切好之后、进 guest 之前落地

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:487`

<a id="fn-kernel-vmm-riscv64-hext_run.c-506"></a>

### `vmm_arch_enter_guest`

```c
int vmm_arch_enter_guest(vcpu_t *vcpu)
```

enter_guest：执行一次 sret → VS-mode, 等到 guest 陷入后返回

<details>
<summary>函数内备注（1 条）</summary>

**第 508 行**

> 每次 task_yield 后需要重设 hstatus.SPV+SPVP 和 sstatus.SPP。
> 这些已在 hext_enter_guest 汇编内完成, 此处只需调用。

> 虚拟中断的注入不在这里：它在 vmm_arch_restore_guest_ctx(), 与本函数
> 同属「每次入口」的钩子, 但由 vmm_run_vcpu 在关中断之后统一调用。

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:506`

<a id="fn-kernel-vmm-riscv64-hext_run.c-529"></a>

### `vcpu_wfi_has_work`

```c
static int vcpu_wfi_has_work(vcpu_t *vcpu)
```

WFI 睡眠期间是否有“该醒”的事件；由 handle_wfi 的睡眠循环调用

<details>
<summary>函数内备注（1 条）</summary>

**第 540 行**

> 外部中断不能漏：控制台输入走 /dev/vmm 的 push_rx，它只把字节放进
> 16550 的 RX 环，不 kick；vPLIC 的 pending 要等入口才注入。如果
> WFI 循环只看定时器/IPI，输入到达后 guest 会一直睡到下一个定时器
> 截止时间。这里把「控制台有数据 / vPLIC 有可投递中断」也算作事件，
> 由宿主 tick 把 vCPU 叫醒后立刻回入口注入。

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:529`

<a id="fn-kernel-vmm-riscv64-hext_run.c-553"></a>

### `handle_wfi`

```c
static int handle_wfi(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若不是 WFI 特征值则告警，仍把 PC 加 4；循环睡在 wfi_wake 上直到有工作，BSP 每轮醒来返回主循环以泵控制台。

<details>
<summary>函数内备注（5 条）</summary>

**第 555 行**

> 只有 WFI 才该走这里。

> hstatus.VTW=1 时 VS-mode 的 WFI 报 virtual instruction(cause 22),
> stval 里是 WFI 的编码 0x10500073。cause 2(illegal instruction)在
> 老 QEMU 上也被用来报同一个陷阱, 所以两者都收。

> 但不能无条件当成 WFI 跳过 4 字节 —— 别的虚拟指令异常也会是
> cause 22, 盲目 +4 会把 guest 的 PC 挪到错的地方(而且压缩指令只有
> 2 字节, +4 会多吃一条)。这里对不上就记一笔再照常跳过 4 字节,
> 至少让这种事在日志里留痕。

**第 571 行**

> 步进 guest PC 越过 WFI 指令(4 字节)

**第 574 行**

> ── 真正的「睡到有事件」：挂在自己的 wfi_wake completion 上 ─────

> 不能在 HS 下执行宿主 wfi（醒来未必对应 guest 事件，而且会挡住
> 同 hart 的兄弟 vCPU）；也不能 yield 一次就立刻重进 guest —— 空闲
> vCPU 会变成 WFI exit/entry 忙循环，4/8 个 vCPU 挤 2 个 hart 时
> 把 CPU 吃光，SMP 启动期的 stop_machine 会因某颗 vCPU 长期得不到
> 调度而卡住（实测 state 停在 PREPARE、3/4 ack，几十秒不前进）。

> 唤醒源见 vmm_riscv_host_tick() 的说明：kick（IPI/设备/stop）立即
> 唤醒，宿主 tick 兜底重查 guest 定时器/外部中断。

**第 590 行**

> 先 reset 再置 in_wfi：
> - kick 在 reset 之前到达 → 事件位已置，下面的复查看得到；
> - kick 在置位之后到达   → vmm_vcpu_kick 看到 in_wfi 并 complete；
> - tick 钩子在两者之间   → 大不了多等一个 10ms tick。

**第 609 行**

> BSP 每个 tick 都回一次主循环：直启模式没有 helper，宿主控制台
> 的输入要靠 vmm_run_vcpu 顶部的 vmm_console_pump() 搬进 guest；
> BSP 一直睡在 WFI 里就没人泵了。helper 模式下这只是每 10ms
> 一次的空转，可以接受。

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:553`

<a id="fn-kernel-vmm-riscv64-hext_run.c-628"></a>

### `sbi_console_putchar`

```c
static void sbi_console_putchar(vcpu_t *vcpu, uint8_t byte)
```

── guest 控制台输出(SBI console_putchar)─────────────────

走与控制台 vdev 的 THR 通路相同的出口(见 vmm_console_putchar)：
helper 模式下必须进 TX 缓冲, 否则 guest 在 8250 驱动起来之前的那几行
会漏进宿主内核日志。

**位置**：`kernel/vmm/riscv64/hext_run.c:628`

<a id="fn-kernel-vmm-riscv64-hext_run.c-657"></a>

### `guest_read_u64`

```c
static int guest_read_u64(vcpu_t *vcpu, uint64_t gpa, uint64_t *out)
```

── guest 物理地址读取(GPA → HPA → 内核直接映射)────────────

注意： 翻译必须走查表(rv_gstage_lookup), 不能再用「GPA 减窗口基址」那种
算术换算。老版本那样写是因为 guest RAM 被整段预留, identity 映射,
GPA 与 HPA 之间有个固定偏移；改成按需分页之后每一页都是各自从 PMM 分配
的, 那个偏移根本不存在 —— 继续用算术换算会读到别的 VM 或宿主内核的
物理内存(没有任何报错, 只是数据是错的)。

查不到就是查不到：调用方(MMIO 取指/解码)要靠这个返回值决定放弃。
注意这里不会触发缺页分配 —— 缺页只由 guest 自己的访问驱动,
宿主读 guest 内存是另一回事, 读到未映射地址说明 guest 的状态本身不对。

**位置**：`kernel/vmm/riscv64/hext_run.c:657`

<a id="fn-kernel-vmm-riscv64-hext_run.c-666"></a>

### `guest_read_u32`

```c
static int guest_read_u32(vcpu_t *vcpu, uint64_t gpa, uint32_t *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 G-stage 把 GPA 翻成 HPA，成功则读 volatile 32 位值返回 1，否则 0。

**位置**：`kernel/vmm/riscv64/hext_run.c:666`

<a id="fn-kernel-vmm-riscv64-hext_run.c-675"></a>

### `guest_read_u16`

```c
static int guest_read_u16(vcpu_t *vcpu, uint64_t gpa, uint16_t *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 G-stage 把 GPA 翻成 HPA，成功则读 volatile 16 位值返回 1，否则 0。

**位置**：`kernel/vmm/riscv64/hext_run.c:675`

<a id="fn-kernel-vmm-riscv64-hext_run.c-685"></a>

### `guest_va_to_gpa`

```c
static int guest_va_to_gpa(vcpu_t *vcpu, uint64_t va, uint64_t *gpa_out)
```

guest 虚拟地址 → guest 物理地址(软件遍历 vsatp 指向的 Sv39/Sv48 页表)

<details>
<summary>函数内备注（3 条）</summary>

**第 702 行（行尾）**

> 无页表：VA 即 GPA(bare 模式)

**第 722 行（行尾）**

> 保留组合

**第 725 行**

> 叶项：4KiB(level0) / 2MiB / 1GiB

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:685`

<a id="fn-kernel-vmm-riscv64-hext_run.c-743"></a>

### `decode_compressed_mmio`

```c
static int decode_compressed_mmio(uint16_t inst, int is_store, mmio_access_t *acc)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解码 16 位压缩 load/store 的 opcode/funct3，填出读写方向、宽度、寄存器号和指令长度，不匹配返回 0。

**位置**：`kernel/vmm/riscv64/hext_run.c:743`

<a id="fn-kernel-vmm-riscv64-hext_run.c-782"></a>

### `decode_mmio_access`

```c
static int decode_mmio_access(uint64_t inst, int is_store, mmio_access_t *acc)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

压缩指令转交压缩解码器；否则解码标准 LOAD/STORE 的 funct3，填访问宽度、寄存器号与指令长度，失败返回 0。

<details>
<summary>函数内备注（3 条）</summary>

**第 784 行**

> 压缩指令(低 2 位 != 11)先处理

**第 792 行（行尾）**

> LOAD

**第 817 行（行尾）**

> STORE

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:782`

<a id="fn-kernel-vmm-riscv64-hext_run.c-843"></a>

### `mmio_instruction`

```c
static int mmio_instruction(vcpu_t *vcpu, uint64_t *inst_out)
```

取 guest PC 处的指令；取指失败时回落到 htinst

<details>
<summary>函数内备注（2 条）</summary>

**第 851 行（行尾）**

> 压缩指令

**第 862 行**

> 回退取 htinst(不是 htval！)：htval 是出错 GPA&gt;&gt;2, 当指令解码会解出
> 完全无关的 opcode/长度/寄存器号 —— 偶发的 guest 跑飞。

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:843`

<a id="fn-kernel-vmm-riscv64-hext_run.c-884"></a>

### `gstage_fault_gpa`

```c
static inline uint64_t gstage_fault_gpa(const vcpu_t *vcpu)
```

── G-stage 缺页：先分 RAM / MMIO, 再各自处理 ───────────────

出错 GPA 的取法：htval 保存的是 GPA >> 2, 低两位拿不回来；页内偏移
从 stval 取。两者拼起来是对的 —— 因为 VS-stage 与 G-stage 的翻译都是
页粒度的, 虚拟地址的 [11:0] 与 guest 物理地址的 [11:0] 恒等, 而 stval
给的正是那个虚拟地址(GVA=1 时)。

注意： 别把它和 htinst 搞混：htinst 是"触发陷阱的那条指令", 用来解码 MMIO
访问；拿 htval 当指令解会解出完全无关的 opcode/长度/寄存器号(见
mmio_instruction 的说明)。

**位置**：`kernel/vmm/riscv64/hext_run.c:884`

<a id="fn-kernel-vmm-riscv64-hext_run.c-904"></a>

### `handle_ram_fault`

```c
static int handle_ram_fault(vcpu_t *vcpu, uint64_t gpa, uint64_t code)
```

handle_ram_fault — RAM 窗口内的 G-stage 缺页：分配物理页 + 装映射

这是按需分页的落点。返回 EL2_RESUME 表示映射已装好, guest 重跑同一条
指令即可；失败返回 EL2_EXIT(PMM 没页 / 没有 VM)。

注意： 不推进 guest PC —— 这正是"缺页"与"MMIO 模拟"的分野：MMIO 那条路
是 VMM 替 guest 完成了这次访问, 所以必须把 PC 挪过去(否则无限重复)；
这里只是把内存补上, 访问本身还得 guest 自己重做一次。两条路走反了的表现
分别是"死循环"和"跳过一条随机指令", 都很难查。

注意： 一次装整个 2 MiB 块(512 页)而不是一页：每次缺页都要一次完整的
HS-mode 往返, 而 guest 启动期是密集触碰内存的。aarch64 那边实测把 1447 次
缺页压到 6 次。代价是最多 2 MiB 的过取。

<details>
<summary>函数内备注（4 条）</summary>

**第 912 行（行尾）**

> 清零, 防跨 VM 数据泄漏

**第 914 行**

> 多 vCPU 下 n==0 有两种含义, 不能一律当「PMM 没页」：
> a) 这个 2 MiB 块已经被**兄弟 vCPU** 装好了(它先缺页),
> 此时查表命中, 这就是成功；
> b) PMM 真的没页了。
> 判据只能是查表 —— 拿 n 判会把正常的并发缺页误判成 OOM, 直接
> 把整个 VM 停掉(症状: 两核 guest 随机在启动中途消失)。

**第 933 行**

> 装完必须刷 G-stage TLB。

> aarch64 那边省掉了这一步(&quot;刚缺页就说明这条翻译本来没缓存&quot;), riscv
> 这里保留：hfence.gvma 就是一条指令, 而 RISC-V 规范明确允许实现缓存
> &quot;不可翻译&quot;的结果 —— 省掉它就等于把&quot;能不能跑&quot;押在具体实现的行为上。

**第 942 行**

> 抽样打印, 便于按地址对账(首次启动的几条最有诊断价值)

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:904`

<a id="fn-kernel-vmm-riscv64-hext_run.c-951"></a>

### `handle_gstage_fault`

```c
static int handle_gstage_fault(vcpu_t *vcpu, uint64_t code)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取故障 GPA 与指令，解码 MMIO 访问后交给总线；命中则回写读值并推进 PC，否则打印错误退出 VM。

<details>
<summary>函数内备注（1 条）</summary>

**第 966 行**

> gpa 一定要打：解不出来时第一个要问的就是&quot;guest 在访问哪&quot;,
> 少了它只能看到一串像随机数的指令编码(实测踩过)。

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:951`

<a id="fn-kernel-vmm-riscv64-hext_run.c-998"></a>

### `sbi_hart_in_mask`

```c
static int sbi_hart_in_mask(uint64_t mask, uint64_t base, uint32_t hart)
```

SBI 的 hart mask 约定：a0 = mask, a1 = base；base == -1 表示全体。
其余情况第 i 位对应 hartid = base + i(只用到低 64 位)。

**位置**：`kernel/vmm/riscv64/hext_run.c:998`

<a id="fn-kernel-vmm-riscv64-hext_run.c-1020"></a>

### `sbi_send_ipi_to`

```c
static void sbi_send_ipi_to(vm_t *vm, uint32_t hart)
```

给目标 vCPU 记一个软件中断并 kick。

状态写在 target->irq_lock 下, 与目标自己的 entry/exit 同步互斥；
unlock 之后再 kick —— 目标可能正在别的 hart 上跑 guest, 先让它退出,
下一次入口才看得到 pending(见 vcpu_software_irq_on_entry)。顺序反了
就是「被叫醒了但什么都没看见」。

**位置**：`kernel/vmm/riscv64/hext_run.c:1020`

<a id="fn-kernel-vmm-riscv64-hext_run.c-1041"></a>

### `sbi_request_fence_to`

```c
static void sbi_request_fence_to(vm_t *vm, uint32_t hart, uint32_t req)
```

给目标 vCPU 记一个远端 fence 请求并 kick。

SBI 要求目标 hart 自己执行 fence.i / sfence.vma；本 VMM 的 vCPU 是分时
运行的任务, 只能在目标下一次入口前补执行(见 vcpu_remote_fence_on_entry)。
调用者(发起 fence 的 vCPU)也在掩码里时同样走这条路：ecall 返回后它
自己下一轮 restore 就会执行, 不必在内核态里同步插一条。

**位置**：`kernel/vmm/riscv64/hext_run.c:1041`

<a id="fn-kernel-vmm-riscv64-hext_run.c-1061"></a>

### `sbi_hsm_handler`

```c
static uint64_t sbi_hsm_handler(vcpu_t *vcpu, uint64_t func, uint64_t hartid, uint64_t start, uint64_t opaque, uint64_t *out)
```

SBI HSM(0x48534d)。Linux 的 CPU bring-up 只用 hart_start / get_status：
- hart_start(a0=hartid, a1=start_addr, a2=opaque)
- hart_get_status(a0=hartid) → a1 = STARTED/STOPPED
hart_stop 暂时回 NOT_SUPPORTED(CPU 热插拔用；正常启动/关机不会走它,
关机走 SRST)。不把「停止」半实现成退出任务 —— 那会让 vm_request_stop
把整个 VM 拆掉(见 vcpu_task_fn 的收尾)。

<details>
<summary>函数内备注（1 条）</summary>

**第 1076 行**

> 启动自己 = 它已经在运行(SBI 规定返回 ALREADY_AVAILABLE)

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:1061`

<a id="fn-kernel-vmm-riscv64-hext_run.c-1101"></a>

### `sbi_ipi_handler`

```c
static uint64_t sbi_ipi_handler(vcpu_t *vcpu, uint64_t func, uint64_t mask, uint64_t base)
```

SBI IPI(0x735049)：a0=mask, a1=base → 给每颗目标置 ipi_pending

**位置**：`kernel/vmm/riscv64/hext_run.c:1101`

<a id="fn-kernel-vmm-riscv64-hext_run.c-1124"></a>

### `sbi_rfence_handler`

```c
static uint64_t sbi_rfence_handler(vcpu_t *vcpu, uint64_t func, uint64_t mask, uint64_t base)
```

SBI RFENCE(0x52464e43)：a0=mask, a1=base, a2=start, a3=size, a4=asid

地址/大小/ASID 一律忽略, 转成「目标 hart 全刷」—— 语义上永远安全,
只是保守。HFENCE_* 系列是给嵌套 hypervisor 用的, 本 VMM 没有嵌套
guest, 按 KVM 的做法当 NOP 成功返回。

<details>
<summary>函数内备注（1 条）</summary>

**第 1144 行（行尾）**

> 无嵌套 guest

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:1124`

<a id="fn-kernel-vmm-riscv64-hext_run.c-1165"></a>

### `handle_vs_ecall`

```c
static int handle_vs_ecall(vcpu_t *vcpu)
```

── VS-mode ecall 处理(scause=10)─────────────────────────

移植自 x-kernel arch/riscv64/mod.rs handle_vs_ecall：
- guest 测试魔数 PRINT/DONE
- SBI legacy：set_timer / console_putchar / console_getchar
- SBI base：spec version / impl id / probe extension
- SBI TIME.set_timer
- SBI IPI.send_ipi / HSM.hart_start|get_status / RFENCE(多 vCPU)

<details>
<summary>函数内备注（14 条）</summary>

**第 1167 行（行尾）**

> a7 = SBI EID / hypercall 号

**第 1168 行（行尾）**

> a6 = SBI FID

**第 1169 行（行尾）**

> a0 = 第一个参数

**第 1171 行**

> 步进 guest PC 越过 ecall(4 字节)

**第 1176 行**

> 留在 INFO(test-vmm 的通过证据), 抽稀成有界输出

**第 1193 行**

> 无输入时按 SBI 约定返回 -1(不是 0 —— 0 是合法字符 &#x27;\0&#x27;)

**第 1206 行（行尾）**

> v0.2

**第 1209 行（行尾）**

> &quot;AVTR&quot;

**第 1213 行**

> Linux 会读这三个填 /proc/cpuinfo, 返回 0 表示「未知」是合法的

**第 1251 行**

> 新截止时间通常在将来, 先撤掉可能还挂着的定时器中断,
> 否则 guest 会立刻再吃一次(下一次入口会按新的 deadline 重算)

**第 1264 行**

> SBI send_ipi：只置目标 vCPU 的软件 pending(实际注入在目标下一次
> 入口), 不在这里碰 hvip —— hvip 是每 hart 的, 而这里可能跑在
> 另一颗 hart 上。

**第 1283 行**

> 远端 fence 记到目标 vCPU, 由它下一次进 guest 前执行

**第 1289 行**

> SRST(复位/关机)。Linux 在 poweroff/reboot 时调用 FID 0。
> 收到就当作 guest 正常退出 —— 这也是 guest 里敲 `poweroff` 之后
> 宿主能拿回控制权的路径(/dev/vmm 的 helper 会看到 EPOLLHUP)。

> 必须先回一次成功(结果寄存器已写好), 再返回 VMEXIT：反过来的话
> guest 会带着未定义的 a0 继续跑。

**第 1309 行**

> 未实现的扩展一律回 NOT_SUPPORTED(不是崩溃)。
> 新版 Linux 会探测 DBCN/CPPC/PMU 等, 回 -2 它就安静地换别的路径。

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:1165`

<a id="fn-kernel-vmm-riscv64-hext_run.c-1322"></a>

### `vmm_arch_exit_handler`

```c
int vmm_arch_exit_handler(vcpu_t *vcpu)
```

exit_handler：处理一次 guest → HS-mode 陷阱

<details>
<summary>函数内备注（17 条）</summary>

**第 1328 行**

> 对标 x-kernel vdev/riscv64/timer.rs 的 RiscvTimerHook::on_exit：
> 已退出 guest, 清除注入的 VS 定时器/外部中断挂起位, 避免残留导致下次
> 进入 guest 立即重复触发。两者都是电平触发, 下一次入口会按当时的设备
> 与 vPLIC 状态重新计算(见 vmm_arch_restore_guest_ctx)。

**第 1338 行**

> 中断(定时器/外部)陷入 HS-mode：短暂让给宿主内核处理, 然后
> resume。对标 kvmm exit_handler 的 is_interrupt 分支。

> 注：hideleg 已把 VS 定时器/外部中断委托给 VS-mode, 所以正常情况
> 下走不到这里 —— 中断在 guest 内部就被消化了。能到这里的是
> 宿主自己(HS 侧)的中断, 比如宿主时钟。

**第 1347 行**

> ── 必须在这里补一次宿主中断分派 ─────────────────────────────

> guest 运行期间走的是 hext_trap_vector, 不经过宿主的通用陷阱分发器,
> 所以 irq_install() 注册的 handler 一个都不会被调用。后果最重的是
> 宿主定时器: timer_handler 里的「重新装载下一次 tick」不会执行,
> STIP 永远挂着。SMP=1/guest vCPU&lt;=宿主核数时看不出来(反正没有别的
> vCPU 要抢); SMP=2 + 4 个 guest vCPU 时, 同一颗 hart 上的两颗 vCPU
> 会互相把对方从 guest 里打出来, 却谁也回不到宿主 S 模式去补 tick
> —— 表现为启动无声卡死, 且没有任何错误日志。

> 合成 frame 里把 SPP 置 1(宿主内核态): timer_handler 用这一位决定
> 要不要扫描宿主 UART 发 SIGINT, guest 时间片里不该做那件事。

**第 1371 行**

> 兜底清 SSIP：正常路径上 plic_ipi_entry 已经清过, 但若该 handler
> 尚未注册(早期启动), 电平型的 SSIP 会让这里变成 IPI 风暴。

**第 1378 行**

> 让出 CPU：宿主 handler 里置的 need_resched 在这里兑现

**第 1386 行**

> scause=2 (Illegal instruction) 或 22 (Virtual instruction)：
> hstatus.VTW=1 时 VS-mode 的 WFI 陷入此处。不同 QEMU 版本报 2 或 22,
> 两者都当 WFI 处理(步进 + 按定时器截止 yield/resume)。

**第 1393 行（行尾）**

> 10

**第 1396 行（行尾）**

> Instruction G-stage Page Fault

**第 1397 行（行尾）**

> Load G-stage Page Fault

**第 1398 行（行尾）**

> Store/AMO G-stage Page Fault

**第 1402 行**

> ── 先分 RAM / MMIO ────────────────────────────────────

> 按需分页之后 G-stage 表初始是空的, 所以 guest 取指/访存每一次
> 落到新页都会走到这里。RAM 窗口内的走缺页分配, 窗口外的才是
> 设备 MMIO, 交给总线模拟。

> 注意： 这个分支必须在最前面。老版本没有它, 20/21/23 一律按 MMIO 处理
> —— 对 RAM 地址取指会去&quot;解码一条根本不存在的指令&quot;, 而访存则是
> 查表未命中 → EL2_EXIT → 整个 VM 停机。缺页因此表现为
> &quot;guest 启动到一半毫无征兆地没了&quot;, 而不是一个缺页。

**第 1417 行**

> 非 RAM 的取指：本 VMM 不做取指 MMIO 模拟(没有需要执行代码段的
> 设备), 所以这一定是 guest 跑飞了 —— 跳进了设备地址或未映射区间。
> 报出地址比让 handle_gstage_fault 去解码「一条根本不存在的指令」
> 有用得多。

**第 1430 行**

> 设备 MMIO：G-stage 未映射 → 陷入模拟(移植自 kvmm mod.rs)

**第 1434 行（行尾）**

> Instruction Page Fault(guest 自己的页表缺项)

**第 1435 行（行尾）**

> Load Page Fault

**第 1436 行（行尾）**

> Store Page Fault

**第 1437 行**

> VS-mode 自己取指/访存缺页 —— guest 内核页表的问题, VMM 帮不上忙。

> 从 VU-mode 来的缺页不该出现在这里：hedeleg 已经把 12/13/15
> 委托给 VS-mode, 由 guest 内核自己填页表。真在这儿看到它们, 通常
> 意味着 guest 内核在 VS-mode 里踩了没映射的地址。

</details>

**位置**：`kernel/vmm/riscv64/hext_run.c:1322`

<a id="fn-kernel-vmm-riscv64-hext_run.c-1474"></a>

### `vmm_arch_save_guest_ctx`

```c
void vmm_arch_save_guest_ctx(vcpu_t *vcpu)
```

save_guest_ctx：guest 退出后与硬件同步 VS 中断状态

只同步**软件中断**：vsip.SSIP 与 hvip.VSSIP 是同一个挂起位的两个视图,
guest 在 VS-mode 清 sip.SSIP 会把 hvip.VSSIP 一起清掉。若 VMM 不回读,
软件 pending 永远为 1, 每次入口都重新注入 —— guest 表现为 IPI 风暴,
一个核被自己的软件中断占满。

判定「这次注入被消费」的三个条件, 缺一不可：
1. 入口确实注入过(ipi_injected)；
2. 序号没变 —— 运行期间没有别的 vCPU 发来新 IPI。新 IPI 到来时
VSSIP 本来就是 0(远端写不到本 hart 的 hvip), 只看条件 3 会把
新 IPI 误清掉, 这正是 KVM 用 irqs_pending_mask 防的那个竞态；
3. 硬件 hvip.VSSIP 已被清 —— guest 真的消费了。

本函数由 vmm_run_vcpu 在持有 vcpu->irq_lock 时调用, 与发送侧互斥。

**位置**：`kernel/vmm/riscv64/hext_run.c:1474`

<a id="mod-kernel-vmm-vcpu.c"></a>

## kernel/vmm/vcpu.c — vCPU：执行主循环 + 内核任务

`kernel/vmm/vcpu.c` · 7 个函数 · 7 个有说明 · 628 行

从 vmm.c 拆出来的(那边混了 VM 池, 主循环, 控制台泵和一个架构的初始化)。
这里管「一个 vCPU 在内核里是一回事」：
- vmm_run_vcpu     : 执行主循环, 通过架构钩子抽象 eret / vmlaunch 的差异
- vcpu_task_fn     : 任务体, 跑主循环 + 收尾(含销毁自己那个 VM 的槽位)
- vcpu_task_create : 建档, 含按核摊开的亲和性设置

池的并发规则("跨任务只传 vmid", "销毁由 vCPU 任务自己做")写在 vm.c ——
vcpu_task_fn 的收尾正是那条规则的执行者。

<a id="fn-kernel-vmm-vcpu.c-37"></a>

### `vmm_vcpu_start`

```c
int vmm_vcpu_start(vcpu_t *vcpu, uint64_t entry, uint64_t context_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按架构初始化 AP vCPU 的复位寄存器现场（RISC-V SBI HSM、x86 SIPI 实模式、aarch64 EL1h），置 RUNNING 并完成 start 唤醒 AP 任务。

<details>
<summary>函数内备注（19 条）</summary>

**第 40 行**

> RISC-V 的 AP 入口：guest 在 BSP 上执行 SBI HSM hart_start(hartid,
> start_addr, opaque), handle_vs_ecall 转到这里。

> SBI HSM 规定的复位现场(与 KVM 的 kvm_sbi_hsm 一致)：
> sepc = a1(start_addr)   satp = 0(bare)
> a0   = hartid           a1 = opaque(对 Linux 是 DTB 物理地址)
> sstatus.SIE = 0, 其余 VS-CSR 全零
> our vcpu 里 pc/vsepc 分开存(c 是 HS sret 目标, vsepc 是 guest 自己的
> sepc), 两个都填入口地址。hstatus_save 必须预置 SPVP=1, 否则第一次
> sret 会落到 VU-mode(见 vmm_arch_vm_init 里同一句的说明)。

**第 59 行（行尾）**

> SPP=0(VS), SIE=0

**第 60 行（行尾）**

> Linux 在 head.S 里自己写

**第 62 行（行尾）**

> bare：guest 从 head.S 开始建自己的页表

**第 71 行**

> r[2](sp) 留 0：SBI HSM 复位现场不含 sp, Linux 的 secondary_start_sbi
> 会在碰栈之前自己设好。a0/a1 才是约定里的两个参数。

**第 73 行（行尾）**

> a0 = hartid

**第 74 行（行尾）**

> a1 = opaque(Linux: DTB PA)

**第 87 行**

> 放行睡在 start completion 上的 AP 任务(可能被唤醒到另一颗宿主核)

**第 93 行**

> x86 的 AP 入口：guest 在 BSP 上往 ICR 写 SIPI, vLAPIC 转到这里。
> SIPI 的语义是「目标 CPU 从实模式 vector&lt;&lt;12:0 开始执行」, 所以
> entry 就是 CS 基址(必须是 4 KiB 对齐、落在 1 MiB 以下)。

**第 104 行（行尾）**

> SIPI 没有 context 参数

**第 106 行**

> AP 的初始现场。VMCS 不在这里建 —— 必须在**承载这颗 vCPU 的宿主核**
> 上建(见 vmm_arch_restore_guest_ctx() 里 vmcs_ready 那段说明)。
> 这里只填 vcpu 里的入口描述, 由 AP 任务第一次进 guest 前构造 VMCS：
> g_ap_reset = 1 → vmcs_init_guest 走 16 位实模式分支
> g_rip      = CS 基址(vector &lt;&lt; 12)，GUEST_RIP 固定为 0
> 实模式的 CR0/段/EFER 由 vmcs_init_guest 填, 这里不重复。

**第 116 行（行尾）**

> SIPI：CS base = vector &lt;&lt; 12

**第 122 行（行尾）**

> IF=0，与真机 SIPI 后一致

**第 124 行**

> 让 AP 任务在自己的宿主核上重做 VMCLEAR→VMPTRLD→写字段

**第 133 行**

> 放行睡在 start completion 上的 AP 任务(可能被唤醒到另一颗宿主核)

**第 143 行**

> AP 的初始状态：EL1h, DAIF 全屏蔽；x0 = context_id。
> sysregs 清零 =&gt; SCTLR_EL1=0(MMU 关闭), 正是 Linux secondary_entry
> 期望的状态；SP_EL1 置 0, Linux 会从 secondary_data 自己取栈。

**第 150 行**

> AP 的 FP/SIMD 初始现场：q0-q31/FPCR/FPSR 全零(架构复位值)

**第 154 行（行尾）**

> EL1h, DAIF masked

**第 167 行**

> 放行 AP 任务。它可能睡在另一颗核上：completion 的唤醒走
> sched_unblock() → 远端 resched IPI, 正是多 vCPU 需要的那条路。

</details>

**位置**：`kernel/vmm/vcpu.c:37`

<a id="fn-kernel-vmm-vcpu.c-180"></a>

### `vmm_vcpu_inject_irq`

```c
void vmm_vcpu_inject_irq(vcpu_t *target, virq_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在目标 vCPU 的 irq_lock 下调用架构层置中断挂起，解锁后再 kick 目标，保证 pending 发布先于唤醒。

<details>
<summary>函数内备注（2 条）</summary>

**第 187 行**

> 写 pending 与目标的 entry/exit 同步互斥。锁的 release/acquire 同时
> 保证：后一次注入不会在前一次还没被同步看到时被覆盖成「只有后者」。

**第 195 行**

> 发布之后再 kick：目标若在别的核上跑 guest, 会先退出, 再在下一轮
> entry 里看到这一步写入的 pending。顺序不能反。

</details>

**位置**：`kernel/vmm/vcpu.c:180`

<a id="fn-kernel-vmm-vcpu.c-208"></a>

### `vmm_vcpu_inject_vector`

```c
void vmm_vcpu_inject_vector(vcpu_t *target, uint32_t vector)
```

x86 guest IPI：向量已经由 guest 的 ICR 写确定, 不需要再过 IO-APIC
重定向表(ICR 直接携带 vector)。次序域与 vmm_vcpu_inject_irq 相同：
持 target->irq_lock 写 IRR, 释放后再 kick。

**位置**：`kernel/vmm/vcpu.c:208`

<a id="fn-kernel-vmm-vcpu.c-223"></a>

### `vmm_vcpu_kick`

```c
void vmm_vcpu_kick(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

vCPU 若在 WFI 中则完成 wfi_wake，并在 host_cpu 有效时向该核发 resched，促使 guest 退出或让出 CPU。

<details>
<summary>函数内备注（2 条）</summary>

**第 229 行**

> RISC-V 的 VS-mode WFI 会把 vCPU 任务挂在 wfi_wake completion 上
> (见 hext_run.c 的 handle_wfi)。SBI IPI / 设备注入 / stop 请求都可能
> 是让它&quot;有活干&quot;的唯一信号 —— 只置 need_resched 不会唤醒一个已经
> completion_wait 睡下的任务，所以必须先把它从 WFI 里叫起来；它醒来
> 后会在主循环里看到 ipi_pending 并重新进 guest。

> 重复 complete 是幂等的（done 保持 1），completion 会在下次 WFI 前
> reset。

**第 246 行**

> 不直接改 task 状态 —— 只让那颗核进入调度。目标在 guest 里时,
> resched IPI 会被硬件转成一次 VM-exit / HS trap, 主循环顶部
> sched_check_and_yield() 自然把 CPU 让出来。

</details>

**位置**：`kernel/vmm/vcpu.c:223`

<a id="fn-kernel-vmm-vcpu.c-261"></a>

### `vmm_run_vcpu`

```c
int vmm_run_vcpu(vcpu_t *vcpu)
```

vmm_run_vcpu — vCPU 执行主循环

通过架构钩子抽象 eret(AArch64)/ vmlaunch+vmresume(x86)差异。
返回 0：guest 正常退出；-1：未处理 exit。

<details>
<summary>函数内备注（9 条）</summary>

**第 266 行**

> 宿主请求停止(/dev/vmm 的 close)。

> 在循环顶部查而不是在别处：这里正好是「上一次 guest 已经退出,
> HCR_EL2 已被 el2_trap_exit 还原成 host 模式(TGE=1, VM=0)」
> 的状态, 直接 return 不会把 Stage-2 或 guest 向量表留在生效状态。

**第 278 行**

> 主动让出 CPU。

> 必须有这个调用：guest 退出走的是 VMM 自己的 guest_vec_table,
> 不经过 sched_check_and_yield_from_trap() —— 那个钩子挂在宿主
> 正常异常向量表的返回路径上。所以 vCPU 任务在循环里从不进入调度器,
> 宿主其它任务会被完全饿死。实测症状：宿主 shell 里跑 /bin/vmm-run,
> helper 卡在启动 guest 的那次 write 之后就再也不动了(连它自己的
> banner 都打不出来), 因为再也抢不到 cpu0。

> 位置与上面的停止检查相同：此刻上一次 guest 已退出, HCR_EL2 已被
> el2_trap_exit 还原成 host 模式, 在这里切任务是安全的。
> sched_check_and_yield() 自己判 need_resched 与中断上下文, 没有
> 待调度任务时只是一次廉价判断。

**第 295 行**

> 记录本颗 vCPU 当前所在的宿主 CPU(多 vCPU 视角)。

> 每次进 guest 前刷新：调度器不做运行期迁移(任务入队即粘核),
> 所以这里读到的值就是「这颗 vCPU 被钉在哪颗核」, exit handler 里
> 通过 vmm_vcpu_host_cpu() 能直接拿到。同时也覆盖了「任务第一次
> 被调度」——到这一刻 host_cpu 才第一次有真实值。

**第 308 行**

> 每轮进 guest 前重写本 VM 的 VTCR/VTTBR —— 本核可能刚跑过别的 VM
> 的 vCPU 任务(时间片轮转), 不重写就会用错页表。这也是&quot;挂起后
> 恢复不需要额外 stage-2 动作&quot;的原因。

**第 315 行**

> 恢复 guest 上下文(AArch64: EL1 sysregs；x86: no-op)。

> 持本 vCPU 的 irq_lock：这一步既会读 pending/active 快照, 也会
> 由架构入口钩子重拉电平线(vtimer/console/NPU)。远端 vCPU 的
> 注入必须等它完成, 否则会出现「set_pending 与 LR 扫描各写一半」。

**第 326 行**

> 进入 guest(AArch64: eret; x86: vmlaunch/vmresume)。

> x86 的 vLAPIC 定时器按「guest 实际执行时间」计时(见 vmm_vlapic.h
> 的 exec_ns)：这里量一次 entry→exit 的墙钟差累加进去。不这么做,
> 宿主核被多个 vCPU 分时时, 一个 guest ISR 跨过好几个 tick 周期,
> 每次 entry 都会立刻再到期 —— guest 被 tick 风暴饿死, 连
> CPU bring-up 的握手都执行不了。

**第 349 行**

> 保存 guest 上下文, 再处理可能会阻塞/调度/打印的 VM-exit。
> 同样持锁：GICv3 的 sync_exit 会回读 LR 并清 active/pending,
> 远端注入不能插在中间。

**第 361 行**

> 每次回到宿主都顺手收一次控制台输入(见 vmm_console_pump)。

> 注意： 只让 BSP 泵。宿主真实 UART 的 RX FIFO 是单消费者资源：
> 多 vCPU 各自从自己的任务里调 uart_rx_ready()/uart_getc(), 同一个
> 字节会被其中一颗抢走, 另一颗看到空 —— 输入会随机丢。跨 vCPU 的
> 串行化本来就要有(锁或单一 owner), BSP-only 是最简单且与
> 「控制台属于 VM, 不属于某颗 vCPU」的语义一致的做法。

**第 374 行**

> 处理 VM exit

</details>

**位置**：`kernel/vmm/vcpu.c:261`

<a id="fn-kernel-vmm-vcpu.c-397"></a>

### `vcpu_task_fn`

```c
static void vcpu_task_fn(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

vCPU 任务入口：发布 task/host_cpu，非 BSP 等 start，调用 vmm_run_vcpu；退出时请求整 VM 停止、打印栈高水位、通知 vm_vcpu_stopped 并 task_exit。

<details>
<summary>函数内备注（5 条）</summary>

**第 402 行**

> 任务真正跑起来之后才发布 task/state/host_cpu —— 创建者(可能在
> task_create 返回前就与本任务并发)不写这些字段, 避免和本任务竞争。

**第 413 行**

> 非 BSP 在 AP 闸门处等 PSCI CPU_ON。等的时候 state 保持 OFFLINE ——
> AFFINITY_INFO 要据此回答 OFF。BSP 直接进入 guest。

**第 428 行**

> 本颗 vCPU 退出 = 整个 VM 该收了。

> 正常关机(HVC_DONE / SBI shutdown)与错误退出都走这里：置 VM 级的
> stop_req, 让兄弟 vCPU 在循环顶部看到后各自退出。多 vCPU 下不能
> 只让自己退出 —— 否则 VM 永远等不齐最后一颗, 槽位也回收不了。

**第 445 行**

> 本颗 vCPU 退场, 由最后一个退出的兄弟负责回收 VM 槽位
> (连同 stage-2 按需页与设备槽位)。

> 必须在 task_exit() 之前调：池的并发规则是&quot;跨任务只传 vmid&quot;,
> 谁也不能在任务还活着的时候清槽位；task_exit() 之后本任务就没了,
> 再没人能安全地做这件事。多 vCPU 下直接 vm_free() 会在第一个退出时
> 拆掉还有兄弟在跑的 stage-2 —— 必须走 vm_vcpu_stopped()。

**第 454 行**

> 打印这个 vCPU 任务的内核栈高水位 —— 定 TASK_STACK_SIZE 的依据就是它。
> 栈是静态数组挨着放的, 溢出会先踩坏邻居任务而不是自己, 所以这个
> 数字比&quot;崩没崩&quot;可靠得多(见 task.h 里 TASK_STACK_SIZE 的说明)。

</details>

**位置**：`kernel/vmm/vcpu.c:397`

<a id="fn-kernel-vmm-vcpu.c-470"></a>

### `vcpu_task_create`

```c
struct task *vcpu_task_create(vcpu_t *vcpu, uint8_t priority)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用十进制拼出任务名，先记账并把 VM 置 RUNNING，再按 (slot+vcpu_id)%ncpu 指定核创建任务，失败回滚计数。

<details>
<summary>函数内备注（8 条）</summary>

**第 472 行**

> 任务名：vm&lt;vmid&gt;c&lt;vcpu_id&gt;。

> 曾经这里写成先 &quot;vm&lt;vmid&gt;&quot; 再 &quot;vcpu&lt;id&gt;&quot; —— 第二段把 name[0..2] 整个
> 覆盖掉了, 所以 ps 里永远显示 vcpu&lt;id&gt;, 第一段是死代码(改了半天名字
> 不变就是这个原因)。两段都保留：一个 VM 可以有多个 vCPU
> (MAX_VCPUS=12), 只用 vmid 会让同一 VM 的 vcpu 任务重名。

> 注意： vmid 和 vcpu_id 都必须按十进制打出来, 不能写成
> `&#x27;0&#x27; + (id % 10)` 或 `&#x27;0&#x27; + (id &amp; 0xF)`：
> - vmid 自增到 255 才回绕(vm.c 的 g_next_vmid), %10 会让 vmid 1/11
> 同时显示成 vm1；
> - vcpu_id 现在最多到 11, %10 会让 vcpu10/11 变成 c0/c1, 和
> vcpu0/1 重名 —— 4 分 12 时 `ps` 里就是 c0,c1,c2,...,c9,c0,c1,
> 看起来像任务丢了/重复了, 实际只是名字撞了。
> 长度上界：&quot;vm255&quot; + &quot; &quot; + &quot;c11&quot; = 9 字符, TASK_NAME_LEN=16 富余。

**第 495 行（行尾）**

> vmid &lt;= 255 → 最多 3 位

**第 510 行（行尾）**

> vcpu_id &lt; 100 → 最多 2 位

**第 524 行**

> 注意： 必须用 task_create_affinity(), 不能 task_create() 之后再
> task_set_cpu_affinity()：后者有窗口 —— 任务在 task_create() 里已经
> 入队(round-robin 可能挑中 CPU1), 若 CPU1 在那两步之间把它挑走执行,
> sched_dequeue() 就找不到它(RUNNING 不在队列里)→ 紧接着 sched_enqueue()
> 又把它挂到指定核的队列 → 同一个 vcpu 任务被他核运行着, 同时躺在那个核的
> 队列里, 被两个核同时跑, 任务状态/运行队列双双写坏。
> 实测就是 Ctrl+] 停 guest 之后再启动时崩在 sched_schedule 的 pick_next()。

> ── vCPU 落在哪颗核上：VMM 自己按核轮转(不钉死 cpu0)──────────

> 三个前提值得写清楚：

> ① 调度器没有负载均衡(sched.c 里没有 balance/steal/migrate),
> 所以任务一旦入队就粘在某颗核上 —— 这不是&quot;跑着跑着被迁走&quot;, 而是
> &quot;启动时摊开&quot;。也正因如此, per-CPU 硬件状态只要在每次进 guest 前
> 重设就够了(运行中迁移是另一件事, 本设计不涉及；task_set_cpu_affinity
> 的注释也写了这条)。这些重设点早就按 per-CPU 写好了：
> x86    : VMXON(s_vmx_on[])+ VMPTRLD + I/O/MSR bitmap(只读)
> aarch64: VTTBR/VTCR + GICv3 的 LR 归属表(按 get_current_cpu_id() 索引)
> riscv  : hgatp + hdeleg/hideleg/hstatus(hext_per_hart_csrs_ensure)

> ② 不要把摊开这件事交给 `CPU_AFFINITY_ANY` —— 试过, 实测摊不开：
> sched_enqueue 的 RR 是 `__atomic_fetch_add(&amp;g_rr_counter, 1) % n`,
> 而每次启动一个 VM 恰好有偶数个 ANY 任务入队(vCPU + 另一个),
> 奇偶恒定 ⇒ 三个 VM 的 vCPU 全落在 cpu0, 等于还是钉死的。
> 判据：日志里连着三行 `on cpu0/2`。
> (要试&quot;完全交给调度器&quot;：把下面的 want 换成 CPU_AFFINITY_ANY。)

> ③ 真摊开之后, 多个 VM 的 vCPU 在不同核上真正并发跑, 而不是挤在一颗
> 核上时间片轮转。

> ── 曾经在这里挡路的跨核竞态：VMCS 宿主区里的 TSS 是 cpu0 的 ──────

> 摊开之后一度出现「三个 guest 都能起到 shell, 但宿主 busybox 随后被
> SIGSEGV 打死」(`User PF CR2=0x1103`, RIP 处的字节根本不是有效代码),
> 且只在 SMP&gt;1 且 vCPU 跑在非 BSP 核上时出现。根因不在调度器, 也不在
> 栈, 而在 VMCS 的宿主 TR：

> - TSS 是每核一份的(boot/x86_64/tss.c 的 g_tss[], 选择子
> 0x30 + cpu_id*0x10), 而 vmcs_init_host() 原来硬读 GDT[6] ——
> 那是 cpu0 的 TSS 描述符；
> - VMCS 宿主区只会在 VM-exit 时被装回, 不会被保存, 所以那个
> 一次性快照永远留在 VMCS 里, refresh 也从来没管过它；
> - 于是 vCPU 在 cpu1 上退出一次之后, cpu1 的 TR 就指向 g_tss[0]。
> x86_64 主机的 IDT 门 ist 全是 0(boot/x86_64/exception.c),
> ring3→ring0 一律靠 TSS.RSP0 换栈 —— 而 g_tss[0].rsp0 是
> cpu0 当前任务的内核栈顶。cpu1 上任何一个用户态中断/异常
> (定时器, 用户缺页)都会把陷阱帧压进 cpu0 那个任务的栈里,
> 两颗核同时写同一段栈。

> 修复：x86_tss_current() 取本核 TSS, vmcs_init_host() 和
> vmx_refresh_host_state() 都用它(后者每次入口前重刷, 见那边注释,
> 同时留了一条不变量自检)。钉 cpu0 时永远不会复现, 因为那时
> cpu1 上根本不发生 VM-exit —— 老的双分表里&quot;3 VM 全钉 cpu0 干净&quot;
> 那一行就是这么来的。(判据是&quot;有没有 VM-exit 落在非 BSP 核上&quot;,
> 与 VM 个数无关；表里另两行的&quot;干净&quot;是小样本, 别当成结论。)

> 复现：`VMN=3 /tmp/three_vm.sh x86_64 2`(或见 docs/vmm/X86_GUEST_LINUX.md §11.5)。

**第 584 行**

> ── 摊到哪颗核：按「VM 槽位 + vCPU 编号」轮转 ──────────────────

> 从前是 `vm-&gt;slot % ncpus` —— 单 vCPU 时没问题, 多 vCPU 时同一 VM 的
> 所有兄弟会全部落在同一颗核, 等于自己把自己串行化了, 而且
> 「每颗 vCPU 在哪颗核」这个视角也就没有意义了。

> 用 (slot + vcpu_id) 而不是 (slot*MAX_VCPUS + vcpu_id)：后者在两核
> 机器上会让 slot0 与 slot1 的 vcpu0 都落到 cpu0(MAX_VCPUS=4 是偶数),
> 多 VM 仍然串行。加法形式同时兼顾两个维度：
> - 同一 VM 的兄弟 vcpu_id 不同 → 优先分散；
> - 单 vCPU 的多个 VM(vcpu_id=0)按 slot 交替。
> 调度器没有负载均衡, 所以这就是最终落点。

**第 601 行**

> 先记账, 再建任务：新任务可能在 task_create 返回前就在另一颗核上
> 跑完并退出, 先建任务会让那次 vm_vcpu_stopped() 看到计数为 0, 误判
> 自己是最后一颗。state 也提前推到 RUNNING, 避免新任务看到 LOADING。

**第 611 行**

> 回滚计数, 但不销毁 VM —— 调用方还要走它自己的错误路径

**第 617 行**

> 不再在这里写 vcpu-&gt;task/state 或打日志 —— 本任务可能已经在另一颗核
> 上跑起来(甚至退出并回收了 VM)。这些字段由 vcpu_task_fn 自己发布,
> 日志也在那里打。

> 注意： 从前这里还有一句 `g_vmm_stop_requested = 0` —— 那在多 VM 下是
> 个真 bug：起第二个 VM 会把第一个 VM 尚未被消费的停止请求清掉。
> 现在停止请求是 per-VM 的(vm-&gt;stop_req), 创建时就随 vm_alloc 的
> memset 归零, 不需要在这里动别人的。

</details>

**位置**：`kernel/vmm/vcpu.c:470`

<a id="mod-kernel-vmm-vdev-console-vpl011.c"></a>

## kernel/vmm/vdev/console/vpl011.c — 虚拟 PL011 UART 设备实现

`kernel/vmm/vdev/console/vpl011.c` · 14 个函数 · 14 个有说明 · 460 行

移植自 x-kernel: virt/vdev/vpl011/src/lib.rs, 适配 Avatar OS：
- klogger::kprint → klog_putchar(宿主内核日志)
- RxChannel → 本文件内的 RX 环形缓冲 + vpl011_push_rx()；
宿主侧由 vmm_console_pump() 轮询真实 PL011 喂入
- TxChannel：kvmm 里只有控制设备打开了通道才逐字节直通, 否则走宿主
日志。avatar 没有独立控制设备 —— 宿主控制台本身就是终端, 故始终
直通：行缓冲会把 guest 的按键回显(无换行的单字符)一直憋到回车,
交互式会话不可用。见 vpl011_put_char()。

<a id="fn-kernel-vmm-vdev-console-vpl011.c-55"></a>

### `vpl011_of`

```c
static vpl011_slot_t *vpl011_of(const vm_t *vm)
```

取本 VM 的槽位。

注意： 除了范围检查, 还要比对 `st.owner`：vm_free() 会把整个 vm_t memset 成 0
(含 slot), 并发窗口里一个已释放的 vm 指针会索引到 0 号槽 —— 那是
另一个 VM 的控制台, TX 环会投错 VM。owner 由 init 写入, 由 destroy 清成
NULL, 槽位复用时重写, 正好当这个守卫。

**位置**：`kernel/vmm/vdev/console/vpl011.c:55`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-117"></a>

### `rx_irq_asserted_locked`

```c
static int rx_irq_asserted_locked(const vpl011_state_t *s)
```

调用者需持有 slot->lock

**位置**：`kernel/vmm/vdev/console/vpl011.c:117`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-122"></a>

### `vpl011_rx_irq_asserted`

```c
int vpl011_rx_irq_asserted(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁读取 RX 中断是否置起，设备不存在返回 0。

**位置**：`kernel/vmm/vdev/console/vpl011.c:122`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-137"></a>

### `vpl011_push_rx`

```c
void vpl011_push_rx(vm_t *vm, uint8_t c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁把字符放入 RX FIFO，满则计数丢弃并只在首次丢弃时告警。

<details>
<summary>函数内备注（1 条）</summary>

**第 148 行**

> 满则丢弃。这里可以打日志：调用点是任务上下文(/dev/vmm 的
> write 或 vmm_run_vcpu 的循环), 不在 klog 锁内。只报第一次。

</details>

**位置**：`kernel/vmm/vdev/console/vpl011.c:137`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-163"></a>

### `vpl011_rx_flush`

```c
void vpl011_rx_flush(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁把 RX FIFO 头尾与计数清零。

**位置**：`kernel/vmm/vdev/console/vpl011.c:163`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-176"></a>

### `vpl011_tx_set_enabled`

```c
void vpl011_tx_set_enabled(vm_t *vm, int enabled)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

设置 vm->console_owned；设备已初始化时在锁下写并关闭时清空 TX FIFO。

<details>
<summary>函数内备注（2 条）</summary>

**第 182 行**

> 设备还没 init(vmm_dev.c 在 vm_create 之前就会调这里)——
> 归属标志在 vm_t 里, 直接写即可, 不能因此漏掉。

**第 192 行**

> 关通道时清空残留, 免得下次开通道吐出上一次会话的尾巴

</details>

**位置**：`kernel/vmm/vdev/console/vpl011.c:176`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-198"></a>

### `vpl011_tx_channel_enabled`

```c
int vpl011_tx_channel_enabled(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在设备锁下读取 vm->console_owned，设备未初始化则直接读。

<details>
<summary>函数内备注（1 条）</summary>

**第 204 行**

> 归属标志在 vm_t 里, 设备未 init 时也要能读(vmm_dev.c 的早期调用)。

> 注意： 有设备时必须在锁里读, 不要图快写成裸 `return vm-&gt;console_owned;`
> (这里从前就是那么写的)。那个标志过去只在 boot 时写一次 —— 那时
> vCPU 任务还没被创建, 天然有序, 裸读没暴露过问题。但 VMM_IOC_ATTACH
> 现在会在运行期改它(切前台), 而读它的 vmm_console_pump() 常常
> 跑在另一颗核上：裸读在弱序架构上没有 acquire 语义, 那一核可能
> 好几轮之后才看见新值, 表现为&quot;刚 attach 的那几下按键有概率被泵抢走&quot;
> —— 字节会被推进一台用户看不见的 guest 的 RX FIFO。
> uart16550 那份一直是在锁里读的, 这里跟上它。

</details>

**位置**：`kernel/vmm/vdev/console/vpl011.c:198`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-224"></a>

### `vpl011_tx_has_data`

```c
int vpl011_tx_has_data(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁返回 TX FIFO 是否有数据。

**位置**：`kernel/vmm/vdev/console/vpl011.c:224`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-238"></a>

### `vpl011_tx_pop`

```c
int vpl011_tx_pop(vm_t *vm, uint8_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁从 TX FIFO 取一字节写入 c 并推进尾指针，成功返回 1，空返回 0。

**位置**：`kernel/vmm/vdev/console/vpl011.c:238`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-262"></a>

### `put_char_locked`

```c
static void put_char_locked(vpl011_state_t *s, uint8_t c)
```

调用者需持有 slot->lock

<details>
<summary>函数内备注（3 条）</summary>

**第 265 行**

> 直启模式：立即输出。klog_putchar 直通 uart_putchar(未开中断时
> 是直接 MMIO 写), 不经行缓冲, 所以 guest 按键回显是实时的。

> 注意这里在持锁状态下调 klog_putchar：klog 自己也有锁, 且
> uart_putchar 在缓冲满时会 timer_spin 等待。这把锁的中断已关,
> 不至于死锁, 但属于「锁内做 I/O」—— 换来的是直启模式下不引入
> 新的锁层级。通道模式(helper)下不会走到这里。

**第 274 行**

> 逐字符走唯一入口(与 vuart16550.c 的 uart_put_char 同理)

**第 281 行（行尾）**

> 丢弃最新字节, 与 kvmm 一致

</details>

**位置**：`kernel/vmm/vdev/console/vpl011.c:262`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-289"></a>

### `vpl011_read`

```c
static uint64_t vpl011_read(mmio_device_t *dev, uint64_t off, uint8_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁按偏移返回 UARTDR（弹 RX 字节）、UARTFR、CR/IMSC、RIS/MIS 与 PID/CID 常量，未知返回 0。

<details>
<summary>函数内备注（3 条）</summary>

**第 292 行**

> priv 仍指向 st(语义不变), 锁在包装器里 —— 用 container_of 找回

**第 312 行（行尾）**

> 输出永远不阻塞

**第 327 行**

> 电平触发：数据还在 FIFO 里就一直为高

</details>

**位置**：`kernel/vmm/vdev/console/vpl011.c:289`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-366"></a>

### `vpl011_write`

```c
static void vpl011_write(mmio_device_t *dev, uint64_t off, uint8_t size, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁按偏移处理 UARTDR 输出字符、UARTCR/IMSC 存值，ICR 等忽略。

<details>
<summary>函数内备注（2 条）</summary>

**第 383 行**

> RX 中断的使能位只影响 RIS/MIS 的呈现, 不改变 FIFO 内容；
> 置 pending 由调用方在进入 guest 前按 vpl011_rx_irq_asserted() 做。

**第 389 行**

> RIS 完全由 FIFO 状态导出(电平触发), 没有需要清的粘滞位

</details>

**位置**：`kernel/vmm/vdev/console/vpl011.c:366`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-406"></a>

### `vpl011_init`

```c
int vpl011_init(vm_t *vm, mmio_bus_t *bus)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取本 VM 设备槽，清零状态并置 CR=UARTEN|TXE|RXE 与 owner，设 ops/priv 后注册到 MMIO 总线。

<details>
<summary>函数内备注（4 条）</summary>

**第 410 行**

> 设备对象在池里, 不再由调用方传进来

**第 415 行**

> 只清状态与设备对象, 不碰锁(见 vpl011_destroy 的说明)。

> 从前这里要&quot;先存后恢复 tx_channel&quot; —— 因为归属标志当时是 state 的一个
> 字段, 而 memset 会把它清回 0, 导致通道模式失效, vmm_console_pump 复活
> 去跟用户态 helper 抢同一个 UART(GUEST_CONSOLE.md §2 记着这个陷阱)。
> 归属标志仍在 vm_t.console_owned, 不在这个结构里, 所以陷阱不适用于
> 状态清零本身 —— 它现在只对&quot;锁&quot;这一类跨 init 存活的对象有意义。

**第 425 行（行尾）**

> UARTEN | TXE | RXE

**第 426 行（行尾）**

> of() 靠它认出&quot;这个槽位属于谁&quot;

</details>

**位置**：`kernel/vmm/vdev/console/vpl011.c:406`

<a id="fn-kernel-vmm-vdev-console-vpl011.c-446"></a>

### `vpl011_destroy`

```c
void vpl011_destroy(vm_t *vm)
```

vpl011_destroy — 归还本 VM 的槽位(由 vmm_arch_vm_destroy → vm_free 调用)

三条硬要求：
① 幂等, 且对"从未 init 过的槽"安全 —— vmm_dev.c 的回滚路径会在
vm_create 失败时调 vm_free, 此时设备一次都没 init(状态全零,
锁是静态零值 = 解锁态, memset 是空操作)。
② 永不 memset 锁对象本身 —— 清零一个正被持有的锁等于凭空放锁,
两个任务会同时进临界区。只清 st 与 dev。
③ 必须在 g_vm_pool_lock 外面调用：它靠设备自己的锁串行化,
而池锁的临界区里不应该做设备操作。

<details>
<summary>函数内备注（1 条）</summary>

**第 456 行（行尾）**

> 含 st.owner —— of() 随即失效

</details>

**位置**：`kernel/vmm/vdev/console/vpl011.c:446`

<a id="mod-kernel-vmm-vdev-console-vuart16550.c"></a>

## kernel/vmm/vdev/console/vuart16550.c — 虚拟 16550A UART 实现(riscv64/x86_64)

`kernel/vmm/vdev/console/vuart16550.c` · 18 个函数 · 18 个有说明 · 528 行

移植自 x-kernel: virt/vdev/uart16550/src/lib.rs, 适配 Avatar OS：
- klogger::kprint → klog_write(直启模式)/ TX 环形缓冲(helper 模式)
- RxChannel → 本文件内的 RX 环形缓冲 + uart16550_push_rx()
- 中断：由宿主在每次进 guest 前按 uart16550_irq_asserted() 经 vPLIC
置 pending(电平触发语义, 见头文件)

与 vpl011.c 是同一个模板的两个实例：TX 双通路, RX 单 FIFO, 电平触发中断。
改这里时请顺带看一眼对面, 两边语义应当保持一致。

注意： 与 vpl011 唯一的有意差异：不做行缓冲。
vpl011 早期版本把 guest 输出攒到换行才 flush, 那会把 guest 的按键回显
(无换行的单字符)一直憋到回车 —— 交互式会话直接不可用。这里逐字节直通。
同理, THR 空中断必须实现：Linux 的 8250 驱动靠它推进发送队列, 缺了会让
输出在第一批字符之后静默停住(详见头文件)。

── 多 VM：状态与锁都搬进了 vm_t ─────────────────────────────

从前上面那两样是文件级 static(g_uart16550 / g_uart16550_lock), 整机只有
一份 —— 第二个 VM 的 init 一句 memset 就把第一个 VM 的控制台清空。现在
每个 vm_t 里各有一份, 函数一律以 vm 为第一参数；MMIO 回调拿不到 vm,
就走 dev->priv → state->owner 回指(与 vpl011.c 同一手法)。

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-62"></a>

### `uart16550_of`

```c
static uart16550_slot_t *uart16550_of(const vm_t *vm)
```

取本 VM 的槽位。除范围检查还要比对 st.owner —— vm_free 会把整个 vm_t
memset 成 0(含 slot), 并发窗口里已释放的 vm 指针会索引到 0 号槽
(另一个 VM 的控制台)。owner 由 init 写入, destroy 清成 NULL。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:62`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-120"></a>

### `rx_irq_asserted_locked`

```c
static int rx_irq_asserted_locked(const uart16550_state_t *s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 RX FIFO 非空且开了 RX 中断，或 guest 开了 THR 空中断。

<details>
<summary>函数内备注（1 条）</summary>

**第 122 行**

> 电平触发：FIFO 非空且 guest 开了 RX 中断, 或者 guest 开了 THR 空中断

</details>

**位置**：`kernel/vmm/vdev/console/vuart16550.c:120`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-129"></a>

### `uart16550_push_rx`

```c
void uart16550_push_rx(vm_t *vm, uint8_t c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁向 RX FIFO 放字符，满则递增丢弃计数后返回。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:129`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-151"></a>

### `uart16550_irq_asserted`

```c
int uart16550_irq_asserted(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁返回 RX 中断是否置起，无设备返回 0。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:151`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-168"></a>

### `uart16550_rx_flush`

```c
void uart16550_rx_flush(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁清零 RX FIFO 头尾、计数与丢弃计数。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:168`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-186"></a>

### `uart16550_tx_set_enabled`

```c
void uart16550_tx_set_enabled(vm_t *vm, int enabled)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

设置 vm->console_owned；设备存在时在锁下写并关闭时清空 TX 环。

<details>
<summary>函数内备注（2 条）</summary>

**第 193 行**

> 设备还没 init(vmm_dev.c 在 vm_create 之前就会调这里)——
> 归属标志在 vm_t 里, 直接写即可, 不能因此漏掉。

**第 205 行**

> 关通道时清空 TX 环：剩下的字节是上一个 guest 的, 别再放给下一个

</details>

**位置**：`kernel/vmm/vdev/console/vuart16550.c:186`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-213"></a>

### `uart16550_tx_channel_enabled`

```c
int uart16550_tx_channel_enabled(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在锁下读取 vm->console_owned，设备未初始化则直接读。

<details>
<summary>函数内备注（1 条）</summary>

**第 219 行**

> 归属标志在 vm_t 里, 设备未 init 时也要能读(vmm_dev.c 的早期调用)

</details>

**位置**：`kernel/vmm/vdev/console/vuart16550.c:213`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-228"></a>

### `uart16550_tx_pop`

```c
int uart16550_tx_pop(vm_t *vm, uint8_t *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁从 TX FIFO 取一字节写入 c，成功返回 1，空返回 0。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:228`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-250"></a>

### `uart16550_tx_has_data`

```c
int uart16550_tx_has_data(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁返回 TX FIFO 是否非空。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:250`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-277"></a>

### `uart_put_char`

```c
static void uart_put_char(uart16550_state_t *s, uint8_t c)
```

uart_put_char — guest 写 THR 的一个字节(调用方必须已持锁)

直启模式：立即走 klog 的唯一入口。宿主 uart_putchar 会把 '\n' 补成
"\r\n", 所以 guest 的裸 '\n'(earlycon)与 tty 已转好的 "\r\n" 都能正确换行。

注意本函数在持锁状态下调 klog_write：klog 自己也有锁, 且 uart_putchar
在缓冲满时会等待。这把锁的中断已关, 因此不会有锁序反转；与 vpl011 的处理
一致。

<details>
<summary>函数内备注（2 条）</summary>

**第 280 行**

> 逐字符走唯一入口：guest 控制台本来就是字节流, 粒度只能是字节,
> 但至少每次输出都真的经过那把锁, 不和别的核各写各的。

**第 288 行**

> 缓冲满：丢尾。helper 读得比 guest 写得慢时才会发生

</details>

**位置**：`kernel/vmm/vdev/console/vuart16550.c:277`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-298"></a>

### `uart16550_read`

```c
static uint64_t uart16550_read(mmio_device_t *dev, uint64_t off, uint8_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅接受 1/4 字节宽度，在锁下按 DLAB 返回 DLL/DLM 或 RBR/IER，并给出 IIR/LCR/MCR/LSR/MSR/SCR，未知返回 0。

<details>
<summary>函数内备注（3 条）</summary>

**第 301 行**

> priv 仍指向 st(语义不变), 锁在包装器里 —— 用 container_of 找回

**第 329 行**

> 中断识别：bit0=0 表示有中断挂起。RX 优先于 THR 空
> (与真实 16550 的优先级一致)。

**第 352 行**

> 发送即时完成 → THRE/TEMT 恒置位；RX 有数据时加 DR

</details>

**位置**：`kernel/vmm/vdev/console/vuart16550.c:298`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-373"></a>

### `uart16550_write`

```c
static void uart16550_write(mmio_device_t *dev, uint64_t off, uint8_t size, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅接受 1/4 字节宽度，在锁下按 DLAB 写 DLL/DLM，否则 THR 输出字符，或保存 IER/FCR/LCR/MCR/SCR。

<details>
<summary>函数内备注（2 条）</summary>

**第 392 行（行尾）**

> THR：输出 guest 字符

**第 404 行（行尾）**

> 只记 FIFO 使能位

</details>

**位置**：`kernel/vmm/vdev/console/vuart16550.c:373`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-437"></a>

### `uart16550_putchar`

```c
void uart16550_putchar(vm_t *vm, uint8_t c)
```

uart16550_putchar — 从 VMM 自己(而非 guest 的 THR 写)往控制台送一个字节

唯一调用者是 RISC-V 的 SBI console_putchar：guest 在 8250 驱动起来之前
用 SBI 打印早期信息 / panic。走的必须是与 THR 完全相同的那条通路,
否则 helper 模式下这几行会漏进宿主内核日志(走 klog)而不是 helper 的
终端 —— 表现是 guest 启动日志中间莫名少一段。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:437`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-458"></a>

### `uart16550_port_read`

```c
uint64_t uart16550_port_read(mmio_device_t *dev, uint64_t off)
```

── 端口 I/O 包装 ──────────────────────────────────────────

x86 的 8250 驱动走端口 I/O(COM1 = 0x3F8), 不是 MMIO。寄存器语义与
MMIO 版完全一致, 所以这里只做「1 字节访问」的转发, 让 PIO 后端复用
同一份状态机(TX/RX 环形缓冲, IIR/LSR 逻辑)而不是再写一份。
见 kernel/vmm/x86_64/vmx.c 的 x86_pio_handle()。

这两个不接 vm：状态在 dev->priv 里, 锁用 container_of 从槽位拿。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:458`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-463"></a>

### `uart16550_port_write`

```c
void uart16550_port_write(mmio_device_t *dev, uint64_t off, uint8_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 1 字节宽度调用 uart16550_write 完成端口写。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:463`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-468"></a>

### `uart16550_init`

```c
int uart16550_init(vm_t *vm, mmio_bus_t *bus)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取本 VM 设备槽，清零状态并设 owner，填 ops/priv 后注册到 MMIO 总线。

<details>
<summary>函数内备注（3 条）</summary>

**第 472 行**

> 设备对象在池里, 不再由调用方传进来

**第 477 行**

> 只清状态与设备对象, 不碰锁(见 uart16550_destroy)。

> 整片清零 state 本身仍是安全的：控制台归属(从前叫 state.tx_channel)
> 住在 vm-&gt;console_owned, 不在设备状态里, 所以不需要&quot;先存后恢复&quot;那一套。
> 见 include/vmm/vmm_uart16550.h 里 state 定义下方的说明。

**第 485 行（行尾）**

> of() 靠它认出&quot;这个槽位属于谁&quot;

</details>

**位置**：`kernel/vmm/vdev/console/vuart16550.c:468`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-500"></a>

### `uart16550_dev`

```c
mmio_device_t *uart16550_dev(vm_t *vm)
```

uart16550_dev — 取本 VM 的控制台设备对象

给 x86 的 PIO 路径用：它不走 MMIO 总线(IN/OUT 由 VM-exit 的 qualification
直接给出端口号), 所以要自己拿设备对象去调 uart16550_port_read/write。
设备对象在池里, 调用方不再能从 vm_t 拿到它。

**位置**：`kernel/vmm/vdev/console/vuart16550.c:500`

<a id="fn-kernel-vmm-vdev-console-vuart16550.c-514"></a>

### `uart16550_destroy`

```c
void uart16550_destroy(vm_t *vm)
```

uart16550_destroy — 归还本 VM 的槽位(由 vmm_arch_vm_destroy → vm_free 调用)

三条硬要求(与 vpl011_destroy 同)：幂等, 对"从未 init 过的槽"安全,
永不 memset 锁对象(清零一个正被持有的锁 = 凭空放锁)。
必须在 g_vm_pool_lock 外面调用 —— 它靠自己的锁串行化。

<details>
<summary>函数内备注（1 条）</summary>

**第 524 行（行尾）**

> 含 st.owner —— of() 随即失效

</details>

**位置**：`kernel/vmm/vdev/console/vuart16550.c:514`

<a id="mod-kernel-vmm-vdev-npu-vnpu.c"></a>

## kernel/vmm/vdev/npu/vnpu.c — kernel/vmm/vdev/npu/vnpu.c

`kernel/vmm/vdev/npu/vnpu.c` · 9 个函数 · 9 个有说明 · 472 行

RK3588 NPU 寄存器透传 —— 见 include/vmm/vmm_vnpu.h 顶上那段说明。

本文件只做一件事：把 guest 的 MMIO 访问按宽度转发到真实寄存器。
没有状态机, 没有影子寄存器, 不解释任何位 —— 这是透传, 不是模拟。
也正因为如此, guest 里的驱动行为与真机完全一致, 出问题时不会多一层
"到底是 VMM 模拟错了还是驱动错了"的疑问。

<a id="fn-kernel-vmm-vdev-npu-vnpu.c-42"></a>

### `vnpu_is_mmu_reg`

```c
static inline bool vnpu_is_mmu_reg(uint64_t off)
```

NPU 的 IOMMU 寄存器块 —— 落在本透传窗口之内, 必须摘掉不能转发。

vendor DTS 给的四个块(0xFDAB9000 / 0xFDABA000 / 0xFDACA000 / 0xFDADA000)
换算到本窗口的偏移就是下面这四个。RK3588 的 MMU 是 Rockchip 自研的单级
IOMMU：一张表, 谁写谁说了算 —— guest 要是够得着, 它就能把任意物理地址
映射进来, 宿主好不容易拿到的隔离立刻归零。

窗口内偏移：核0+0x9000, 核0+0xA000, 核1+0xA000, 核2+0xA000。

**位置**：`kernel/vmm/vdev/npu/vnpu.c:42`

<a id="fn-kernel-vmm-vdev-npu-vnpu.c-112"></a>

### `vnpu_key_off`

```c
static inline bool vnpu_key_off(uint64_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断偏移是否为需每次打印的关键 NPU 寄存器（0x08/0x10/0x14/0x34）。

**位置**：`kernel/vmm/vdev/npu/vnpu.c:112`

<a id="fn-kernel-vmm-vdev-npu-vnpu.c-120"></a>

### `vnpu_mmu_touched`

```c
static void vnpu_mmu_touched(const char *what, uint64_t off)
```

guest 碰 MMU 寄存器 —— 只报一次, 然后静默忽略(不是致命错, 但它意味着
guest 的 DTB 里写了 iommus, 而我们是按"guest 没有 IOMMU"设计的)

**位置**：`kernel/vmm/vdev/npu/vnpu.c:120`

<a id="fn-kernel-vmm-vdev-npu-vnpu.c-132"></a>

### `vnpu_reg`

```c
static inline volatile void *vnpu_reg(uint64_t offset)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回宿主 NPU 基址加偏移的 volatile 指针。

**位置**：`kernel/vmm/vdev/npu/vnpu.c:132`

<a id="fn-kernel-vmm-vdev-npu-vnpu.c-137"></a>

### `vnpu_read`

```c
static uint64_t vnpu_read(mmio_device_t *dev, uint64_t offset, uint8_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验越界并拒绝 MMU 寄存器，首次访问打印日志，按 1/2/4 字节宽度读宿主寄存器，其它宽度返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 154 行**

> 按访问宽度转发。NPU 寄存器是 32 位, 但驱动可能用 8/16 位读同一片地址
> (比如探测), 照宽度回即可。

</details>

**位置**：`kernel/vmm/vdev/npu/vnpu.c:137`

<a id="fn-kernel-vmm-vdev-npu-vnpu.c-169"></a>

### `vnpu_write`

```c
static void vnpu_write(mmio_device_t *dev, uint64_t offset, uint8_t size, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验边界并拦截 MMU 寄存器；写 PC_OP_EN 时同步 NPU IOMMU 表，检查描述符与 regcmd 地址并打印关键寄存器，最后按宽度转发写。

<details>
<summary>函数内备注（12 条）</summary>

**第 181 行**

> 提交点：guest 写 PC_OP_EN(0x08) = 1, NPU 马上要开始 DMA。
> 在这之前把 stage-2 的映射镜像进 NPU 的 IOMMU 表 —— 这是&quot;NPU 能看到的
> 内存&quot;的唯一权威来源, 也是窗口外缓冲不再出错的原因。

**第 191 行**

> 表填完了才对账 —— 探针放在 PC_DATA_ADDR 那一步会看到一张
> 还没填的空表(真机上就是这么误报了一次&quot;表错了&quot;)。
> PC_DATA_ADDR 的值此刻还在 —— 它就在下面那行 key trace 里。

**第 197 行**

> 提交完成, 记下这次要探的地址(下一次 NPU 寄存器访问时打)

**第 202 行**

> 作业失败后的第一次 NPU 寄存器访问 —— 通常是驱动超时后做的
> `RKNPU: soft reset`。作业期间 guest 一个寄存器都不写, 所以这个
> 触发点很干净。

> 这时候 dump 才看得到失败时刻的 MMU 状态, 尤其是
> PAGE_FAULT_ADDR —— 它会直接给出翻译不过去的那个 IPA。
> 之前那次 dump 是在 enable 时打的(作业还没开始), 页错误位必然是空的。

**第 214 行**

> 注意： 只在出问题时打。

> 探针一开始每次都打, 是为了验&quot;我建的表对不对&quot; —— 那个问题已经
> 有答案了(真机上跑出了 100 多行&quot;两者一致 &quot;, 零缺页)。
> 再每次都打, 就是每提交一次刷上百行, 把后面真正有用的东西
> 淹没掉。

> 判据：任何一组 bank 缺页。没有缺页就不打 —— 缺页才是
> &quot;NPU 读不到它要的东西&quot;的唯一直接证据。

**第 237 行**

> 关键寄存器每次写都记(见 vnpu_key_off 的说明)

**第 244 行**

> 描述符地址越界检查 —— 这是&quot;NPU 读不读得到 regcmd&quot;的直接判据。
> 0x10 的值可能是 1(切 slave 的哨兵)或一个真地址；真地址必须落在
> 恒等窗口里, 否则 NPU 按物理地址去读会读到别处的东西, 而两边都不报错：
> guest 侧驱动只管写下去, NPU 那边只是跑不出结果。

**第 264 行**

> 地址在窗口里 —— 但那只说明这份 regcmd 放对了地方。

> regcmd 里还带着权重/输入/输出缓冲的物理地址(它们是寄存器写的
> 值, 不是 PC 侧那几条描述符), NPU 会照着这些地址去读写。
> 只要其中任何一个落在窗口外, NPU 就会去访问宿主的物理内存 ——
> 那块地址在宿主眼里根本不是 guest 的页, 于是：
> · NPU 的输出写进了别人的内存 → guest 读到的输出永远是垃圾
> · 写坏的那块如果是别的 guest 页 → guest 内核莫名 Oops
> 而两边都不报错：guest 只管提交, NPU 只管写。

> 所以这里把整份 regcmd 扫一遍, 把&quot;像物理地址, 又不在窗口里&quot;的
> 值全部列出来。一次性, 有上限, 不会刷屏。

**第 279 行（行尾）**

> PC_DATA_AMOUNT 个 8 字节单元

**第 286 行（行尾）**

> 别在热路径上扫太多

**第 295 行**

> 只挑&quot;看着像这段 guest RAM 的物理地址&quot;的值：
> 0x70000000..0x7FFFFFFF。窗口是它的一个子区间。

**第 317 行**

> 其余偏移每个只记一次(见 g_vnpu_wrote 的说明)

</details>

**位置**：`kernel/vmm/vdev/npu/vnpu.c:169`

<a id="fn-kernel-vmm-vdev-npu-vnpu.c-357"></a>

### `vnpu_init`

```c
int vnpu_init(vm_t *vm, mmio_bus_t *bus, uintptr_t host_base)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

无 NPU 平台直接返回成功；被其它 VM 占用则拒绝；填 ops/priv 并注册 MMIO，释放宿主中断、初始化并启用 NPU MMU，记录归属。

<details>
<summary>函数内备注（5 条）</summary>

**第 359 行**

> 平台没有 NPU(QEMU, 其它板子)→ 不注册设备。
> 这不是错误：guest 的 DTS 里也不会有 npu 节点。

**第 375 行**

> 注意： ops/priv 必须在 mmio_bus_register 之前填好。

> mmio_bus_register() 第一件事就是读 `dev-&gt;ops-&gt;name / -&gt;base / -&gt;size`
> 打那行 &quot;[mmio] registered &#x27;...&#x27;&quot; —— 此时 ops 还是 NULL(vm_t 整个被
> memset 过), 于是读 0 号地址：
> [el1_sync] EC=0x25 ESR=0x96000007 FAR=0x0
> #0 mmio_bus_register+0x6c
> #1 vnpu_init+0x50
> 真机上第一次跑到就崩在这儿。vpl011_init 是按这个顺序写的
> (先 dev.ops = ... 再 register), 照它来。

**第 388 行**

> sync 要查这个 VM 的 stage-2, 而 vnpu_write 只拿得到 dev

**第 397 行**

> 让宿主驱动放手：关掉它在 GIC 里使能的 142/143/144。
> 不做的后果见 rknpu_host_release() 的注释(宿主 ISR 会抢先把 guest
> 的中断清掉, 而且两边都不报错)。

> 放在注册成功之后：万一上面失败, 建 VM 半途而废, 宿主手里还拿着
> NPU, 不会出现&quot;谁都没有 NPU&quot;的空窗。

**第 406 行**

> 把 NPU 的地址翻译接管过来：guest 交给 NPU 的是 IPA, 由宿主的表
> 翻到真实物理页 —— 这样它给什么缓冲都行, 不再受恒等窗口限制。

</details>

**位置**：`kernel/vmm/vdev/npu/vnpu.c:357`

<a id="fn-kernel-vmm-vdev-npu-vnpu.c-420"></a>

### `vnpu_destroy`

```c
void vnpu_destroy(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅当本 VM 持有 NPU 时清除归属，先关闭 NPU 地址翻译再交还宿主驱动。

<details>
<summary>函数内备注（2 条）</summary>

**第 426 行**

> 交还给宿主 —— 这样停掉 VM 之后还能再起一个(vmm_helper_regress.sh
> 的 --restart 正是走这条路), 宿主自己也能接着跑 NPU 作业。

**第 429 行**

> 注意： 关翻译在前：宿主 reclaim 之后自己驱动 NPU 用的是物理地址

</details>

**位置**：`kernel/vmm/vdev/npu/vnpu.c:420`

<a id="fn-kernel-vmm-vdev-npu-vnpu.c-457"></a>

### `vnpu_irq_asserted`

```c
bool vnpu_irq_asserted(vm_t *vm, uint32_t core)
```

中断线是否有效 —— 判据是 `(INT_STATUS & INT_MASK) != 0`。

注意：注意： 别写成 `INT_STATUS & ~INT_MASK` —— 那是把掩码反着用了,
结果恒为 0, 于是一次都不会注入。真机上就是这么错的：
NPU 从 `no-map` 修好之后一直在正常完成作业
(guest 里 `INT_STATUS=0x100`, 宿主里同一份作业也是 `0x100`,
而宿主的驱动判它成功, 打出了检测框),
但 guest 的 ISR 永远不跑 → 6 秒超时, task counter 为 0。
所有寄存器回读, regcmd, DMA 全都正常, 症状完全指向 NPU 没跑。

为什么 `& INT_MASK` 才对：实测 `INT_STATUS == INT_RAW_STATUS & INT_MASK`
(raw=0xc0000151, mask=0x300, status=0x100 —— 逐位吻合), 也就是
INT_STATUS 本身就已经是掩码后的值, 再 `& ~mask` 就抹干净了。
写成 `& INT_MASK` 对两种解释都对：INT_STATUS 已经是掩码值时它是恒等,
万一它是原始值也照样正确。`& ~mask` 则只在"INT_STATUS 是原始值"这种
本硬件并不成立的解释下才对。

三核各自独立判：一次作业可能只跑一个核(AUTO/单核掩码), 也可能三核
一起完成但先后差几微秒, 所以不能"任一核有效就投三条线", 只能各投各的。

这里只读两个寄存器, 不写任何东西, 见头文件里"只读, 不 ack"那条。

<details>
<summary>函数内备注（1 条）</summary>

**第 459 行（行尾）**

> 目前三核全局唯一；留参数是为了将来能按 VM 区分

</details>

**位置**：`kernel/vmm/vdev/npu/vnpu.c:457`

<a id="mod-kernel-vmm-vdev-vgic-vgic.c"></a>

## kernel/vmm/vdev/vgic/vgic.c — VM-level virtual GICv2 core.

`kernel/vmm/vdev/vgic/vgic.c` · 22 个函数 · 22 个有说明 · 274 行

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-18"></a>

### `valid_vcpu`

```c
static int valid_vcpu(const vgic_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 vgic 非空且 vcpu_id 小于其 vCPU 数。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:18`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-23"></a>

### `valid_irq`

```c
static int valid_irq(uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回中断号是否小于 1020。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:23`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-28"></a>

### `irq_bit`

```c
static uint32_t irq_bit(uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 1 << (irq & 31) 的位掩码。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:28`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-33"></a>

### `lowest_irq`

```c
static uint32_t lowest_irq(uint32_t bits, uint32_t base)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取 bits 中最低置位的位序号并加上 base 返回。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:33`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-45"></a>

### `lr_value`

```c
static uint32_t lr_value(uint32_t irq, uint32_t source_vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

构造含 Group1、Pending、默认优先级与 INTID 的 LR 值，SGI（<16）额外填源 vCPU。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:45`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-54"></a>

### `vmm_vgic_init`

```c
int vmm_vgic_init(vgic_t *vgic, uint32_t nr_vcpus)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 vGIC、限制 vCPU 数，清零结构并给每 vCPU 的 enabled0 置 SGI 掩码，打印就绪日志。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:54`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-73"></a>

### `vmm_vgic_set_dist_enabled`

```c
void vmm_vgic_set_dist_enabled(vgic_t *vgic, int enabled)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若 vgic 存在则设置 dist_enabled 标志。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:73`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-79"></a>

### `vmm_vgic_dist_enabled`

```c
int vmm_vgic_dist_enabled(const vgic_t *vgic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 vgic 的 dist_enabled，空指针返回 0。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:79`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-84"></a>

### `vmm_vgic_set_sgi_pending`

```c
void vmm_vgic_set_sgi_pending(vgic_t *vgic, uint32_t vcpu_id, uint32_t source_vcpu, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 vCPU 与 SGI 号后置源位图位并置 pending 位。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:84`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-95"></a>

### `vmm_vgic_set_pending`

```c
void vmm_vgic_set_pending(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验中断号：SGI 转 SGI 置位（源 0），PPI 置该 vCPU pending0，SPI 置对应 spi_pending 字。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:95`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-110"></a>

### `vmm_vgic_set_enabled`

```c
void vmm_vgic_set_enabled(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq, int enabled)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略 SGI；PPI 改该 vCPU enabled0，SPI 改全局 enabled 字，按 enabled 置位或清位。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:110`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-128"></a>

### `vmm_vgic_enabled_word`

```c
uint32_t vmm_vgic_enabled_word(const vgic_t *vgic, uint32_t vcpu_id, uint32_t word)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 返回该 vCPU enabled0|SGI 掩码，其它返回全局 enabled 字。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:128`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-137"></a>

### `vmm_vgic_pending_word`

```c
uint32_t vmm_vgic_pending_word(const vgic_t *vgic, uint32_t vcpu_id, uint32_t word)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 返回该 vCPU pending0，其它返回其 spi_pending 字。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:137`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-148"></a>

### `vmm_vgic_active_word`

```c
uint32_t vmm_vgic_active_word(const vgic_t *vgic, uint32_t vcpu_id, uint32_t word)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 返回该 vCPU active0，其它返回其 spi_active 字。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:148`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-159"></a>

### `vmm_vgic_clear_pending_word`

```c
void vmm_vgic_clear_pending_word(vgic_t *vgic, uint32_t vcpu_id, uint32_t word, uint32_t bits)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 清该 vCPU pending0 的指定位，其它清对应 spi_pending 字。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:159`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-172"></a>

### `vmm_vgic_clear_active_word`

```c
void vmm_vgic_clear_active_word(vgic_t *vgic, uint32_t vcpu_id, uint32_t word, uint32_t bits)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 清 active0 位，否则清对应 spi_active 字位。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:172`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-185"></a>

### `vmm_vgic_next_pending`

```c
int vmm_vgic_next_pending(const vgic_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先检查 SGI/PPI 区 pending&enabled&~active，否则逐字检查 SPI，返回最低可用中断号，无则 -1。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:185`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-203"></a>

### `vmm_vgic_ack`

```c
int vmm_vgic_ack(vgic_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取下一待处理中断，清 pending、置 active（SGI 清源位），清对应 LR 并返回中断号或 -1。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:203`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-223"></a>

### `vmm_vgic_eoi`

```c
void vmm_vgic_eoi(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验参数后清除该 vCPU 对应中断的 active 位（PPI 在 active0，SPI 在 spi_active）。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:223`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-233"></a>

### `vmm_vgic_inject_timer`

```c
void vmm_vgic_inject_timer(vgic_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 VIRQ_VTIMER 置为 pending 并调用 sync_entry 尝试装入 LR。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:233`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-239"></a>

### `vmm_vgic_sync_entry`

```c
void vmm_vgic_sync_entry(vgic_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取最低待处理中断，若 LR 已有则跳过，否则找空槽（SGI 取源 vCPU）写入一条 pending LR 后返回。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:239`

<a id="fn-kernel-vmm-vdev-vgic-vgic.c-270"></a>

### `vmm_vgic_sync_exit`

```c
void vmm_vgic_sync_exit(vgic_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若 vCPU 有效则从硬件回读 LR 状态到软件模型。

**位置**：`kernel/vmm/vdev/vgic/vgic.c:270`

<a id="mod-kernel-vmm-vdev-vgic-vgicc.c"></a>

## kernel/vmm/vdev/vgic/vgicc.c — per-vCPU GICC and GICH state.

`kernel/vmm/vdev/vgic/vgicc.c` · 9 个函数 · 9 个有说明 · 159 行

<a id="fn-kernel-vmm-vdev-vgic-vgicc.c-21"></a>

### `valid_vcpu`

```c
static int valid_vcpu(const vgic_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 vgic 非空且 vcpu_id 小于其 vCPU 数。

**位置**：`kernel/vmm/vdev/vgic/vgicc.c:21`

<a id="fn-kernel-vmm-vdev-vgic-vgicc.c-26"></a>

### `vgicc_read_for_vcpu`

```c
static uint64_t vgicc_read_for_vcpu(mmio_device_t *dev, uint64_t off, uint8_t size, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 vCPU 后按偏移返回 GICC_CTLR/PMR/IIDR；读 IAR 时执行 ack，成功返回中断号，否则返回伪中断值。

**位置**：`kernel/vmm/vdev/vgic/vgicc.c:26`

<a id="fn-kernel-vmm-vdev-vgic-vgicc.c-50"></a>

### `vgicc_write_for_vcpu`

```c
static void vgicc_write_for_vcpu(mmio_device_t *dev, uint64_t off, uint8_t size, uint64_t value, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 vCPU 后写 CTLR（记录 EOI 模式）与 PMR；写 EOIR（非 split 模式）或 DIR 时执行 EOI。

**位置**：`kernel/vmm/vdev/vgic/vgicc.c:50`

<a id="fn-kernel-vmm-vdev-vgic-vgicc.c-88"></a>

### `vgicc_init`

```c
int vgicc_init(mmio_device_t *dev, mmio_bus_t *bus, vgic_t *vgic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

给每个 vCPU 设 GICC IIDR 与 PMR=0xf0，填 ops/priv 并注册到 MMIO 总线。

**位置**：`kernel/vmm/vdev/vgic/vgicc.c:88`

<a id="fn-kernel-vmm-vdev-vgic-vgicc.c-108"></a>

### `vgicc_lr_has_irq`

```c
int vgicc_lr_has_irq(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历 LR，存在状态非零且 INTID 匹配的中断则返回 1，否则 0。

**位置**：`kernel/vmm/vdev/vgic/vgicc.c:108`

<a id="fn-kernel-vmm-vdev-vgic-vgicc.c-119"></a>

### `vgicc_lr_empty_slot`

```c
int vgicc_lr_empty_slot(vgic_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

找第一个状态为空的 LR 槽并返回索引，没有返回 -1。

**位置**：`kernel/vmm/vdev/vgic/vgicc.c:119`

<a id="fn-kernel-vmm-vdev-vgic-vgicc.c-129"></a>

### `vgicc_write_lr`

```c
void vgicc_write_lr(vgic_t *vgic, uint32_t vcpu_id, uint32_t slot, uint32_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验后把值写入软件 LR 镜像并用 gic_write_lr 写硬件寄存器。

**位置**：`kernel/vmm/vdev/vgic/vgicc.c:129`

<a id="fn-kernel-vmm-vdev-vgic-vgicc.c-137"></a>

### `vgicc_clear_lr_irq`

```c
void vgicc_clear_lr_irq(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历 LR，把 INTID 匹配且有状态的槽清零并写硬件。

**位置**：`kernel/vmm/vdev/vgic/vgicc.c:137`

<a id="fn-kernel-vmm-vdev-vgic-vgicc.c-150"></a>

### `vgicc_save_state_from_hw`

```c
void vgicc_save_state_from_hw(vgic_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历 LR 从硬件读取，有状态则保存否则清 0，并把硬件 LR 清零。

**位置**：`kernel/vmm/vdev/vgic/vgicc.c:150`

<a id="mod-kernel-vmm-vdev-vgic-vgicd.c"></a>

## kernel/vmm/vdev/vgic/vgicd.c — VM-owned virtual GICv2 distributor.

`kernel/vmm/vdev/vgic/vgicd.c` · 9 个函数 · 9 个有说明 · 200 行

<a id="fn-kernel-vmm-vdev-vgic-vgicd.c-30"></a>

### `reg_read`

```c
static uint64_t reg_read(const vgic_t *vgic, uint32_t off, uint8_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 dist_regs 按字节偏移和小端拼出 size 字节值，越界字节忽略。

**位置**：`kernel/vmm/vdev/vgic/vgicd.c:30`

<a id="fn-kernel-vmm-vdev-vgic-vgicd.c-39"></a>

### `reg_write`

```c
static void reg_write(vgic_t *vgic, uint32_t off, uint8_t size, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 value 按字节小端写入 dist_regs 对应偏移，越界字节忽略。

**位置**：`kernel/vmm/vdev/vgic/vgicd.c:39`

<a id="fn-kernel-vmm-vdev-vgic-vgicd.c-46"></a>

### `word_index`

```c
static int word_index(uint64_t off, uint32_t base, uint32_t *index)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断偏移是否落在 base 起 ENABLE_WORDS*4 范围内，是则算出字索引返回 1。

**位置**：`kernel/vmm/vdev/vgic/vgicd.c:46`

<a id="fn-kernel-vmm-vdev-vgic-vgicd.c-54"></a>

### `vgicd_set_pending`

```c
void vgicd_set_pending(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转调 vmm_vgic_set_pending。

**位置**：`kernel/vmm/vdev/vgic/vgicd.c:54`

<a id="fn-kernel-vmm-vdev-vgic-vgicd.c-59"></a>

### `vgicd_is_enabled`

```c
int vgicd_is_enabled(const vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验中断号后返回其 enabled 字中对应位是否置位。

**位置**：`kernel/vmm/vdev/vgic/vgicd.c:59`

<a id="fn-kernel-vmm-vdev-vgic-vgicd.c-66"></a>

### `handle_sgir`

```c
static void handle_sgir(vgic_t *vgic, uint32_t value, uint32_t source_vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 SGIR 的 SGI 号与目标过滤模式，算出目标 CPU 位图并逐个置 SGI pending。

**位置**：`kernel/vmm/vdev/vgic/vgicd.c:66`

<a id="fn-kernel-vmm-vdev-vgic-vgicd.c-92"></a>

### `vgicd_read_for_vcpu`

```c
static uint64_t vgicd_read_for_vcpu(mmio_device_t *dev, uint64_t off, uint8_t size, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按偏移返回 GICD_CTLR/TYPER/IIDR、使能/挂起/active 字与目标寄存器视图，其余回退 dist_regs。

**位置**：`kernel/vmm/vdev/vgic/vgicd.c:92`

<a id="fn-kernel-vmm-vdev-vgic-vgicd.c-119"></a>

### `vgicd_write_for_vcpu`

```c
static void vgicd_write_for_vcpu(mmio_device_t *dev, uint64_t off, uint8_t size, uint64_t value, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按偏移处理 GICD_CTLR、set/clear 使能/挂起/active、SGIR 与 ITARGETSR 写入，其余存入 dist_regs。

**位置**：`kernel/vmm/vdev/vgic/vgicd.c:119`

<a id="fn-kernel-vmm-vdev-vgic-vgicd.c-190"></a>

### `vgicd_init`

```c
int vgicd_init(mmio_device_t *dev, mmio_bus_t *bus, vgic_t *vgic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

填 ops/priv 并注册 GICD MMIO 设备，成功后打印就绪日志。

**位置**：`kernel/vmm/vdev/vgic/vgicd.c:190`

<a id="mod-kernel-vmm-vdev-vgicv3-vgicd3.c"></a>

## kernel/vmm/vdev/vgicv3/vgicd3.c — 虚拟 GICv3 分发器(GICD)模拟

`kernel/vmm/vdev/vgicv3/vgicd3.c` · 6 个函数 · 6 个有说明 · 205 行

guest 通过 MMIO 配置 GICD。GICv3 打开 ARE 之后 GICD 只管 SPI
(INTID >= 32), SGI/PPI 全在 GICR(见 vgicr3.c), ITARGETSR 变成 RES0,
SPI 目标改用 IROUTER。

三个必须读回正确值的地方(否则 guest 的探测会卡住)：
- GICD_CTLR.RWP(bit31)：ARE 生效的握手位, 必须读回 0
- GICD_TYPER：ITLinesNumber / CPUNumber / LPIS=0 决定 guest 分配多少 IRQ
- IGROUPR / IPRIORITYR / ICFGR：字节可寻址后备存储, 读回写进去的值

真实的宿主 GIC 分发器永不触碰。

<a id="fn-kernel-vmm-vdev-vgicv3-vgicd3.c-49"></a>

### `word_index`

```c
static int word_index(uint64_t off, uint32_t base, uint32_t *index)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断偏移是否在 base..base+GICD_BLOCK_SIZE 内，是则返回字索引 1。

**位置**：`kernel/vmm/vdev/vgicv3/vgicd3.c:49`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicd3.c-57"></a>

### `reg_read`

```c
static uint64_t reg_read(const vgic3_t *vgic, uint32_t off, uint8_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 vgic3 dist_regs 按字节和小端拼出 size 字节值，越界忽略。

**位置**：`kernel/vmm/vdev/vgicv3/vgicd3.c:57`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicd3.c-67"></a>

### `reg_write`

```c
static void reg_write(vgic3_t *vgic, uint32_t off, uint8_t size, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把值按字节小端写入 vgic3 dist_regs，越界忽略。

**位置**：`kernel/vmm/vdev/vgicv3/vgicd3.c:67`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicd3.c-74"></a>

### `vgic3d_read`

```c
static uint64_t vgic3d_read(mmio_device_t *dev, uint64_t off, uint8_t size, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按偏移返回 CTLR（清 RWP）、TYPER、IIDR/PIDR2、使能/挂起/active 字与 ITARGETSR 的 0，其余回退 dist_regs。

<details>
<summary>函数内备注（5 条）</summary>

**第 82 行**

> RWP 恒 0：所有写操作立即生效

**第 87 行**

> ITLinesNumber=31 → 1024 个 INTID；CPUNumber=nr_vcpus-1；
> LPIS/ESPI/安全扩展全 0(没有 LPI, 也没有 SPI 之外的扩展)；
> IDbits=15 → 16 位 INTID, 与 QEMU virt 的 GICv3 一致。

**第 96 行（行尾）**

> ARM, GICv3 架构修订 3

**第 99 行（行尾）**

> 架构修订 = GICv3

**第 110 行**

> ARE 模式下 ITARGETSR 是 RES0 —— guest 读它只会得到 0,
> CPU 目标改从 IROUTER 读。

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicd3.c:74`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicd3.c-119"></a>

### `vgic3d_write`

```c
static void vgic3d_write(mmio_device_t *dev, uint64_t off, uint8_t size, uint64_t value, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按偏移处理 CTLR、set/clear 使能/挂起/active，ITARGETSR 忽略，其余路由等寄存器存入 dist_regs。

<details>
<summary>函数内备注（3 条）</summary>

**第 168 行**

> guest 手工置 active：只做记录, 不影响投递

**第 176 行（行尾）**

> ARE: RES0

**第 178 行**

> IROUTER：8 字节/SPI, 直接存进后备存储即可(本 VM 没有 SPI)

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicd3.c:119`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicd3.c-192"></a>

### `vgic3d_init`

```c
int vgic3d_init(mmio_device_t *dev, mmio_bus_t *bus, vgic3_t *vgic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

填 ops/priv 并注册 GICD MMIO 设备，成功后打印日志。

**位置**：`kernel/vmm/vdev/vgicv3/vgicd3.c:192`

<a id="mod-kernel-vmm-vdev-vgicv3-vgicr3.c"></a>

## kernel/vmm/vdev/vgicv3/vgicr3.c — 虚拟 GICv3 Redistributor(GICR)模拟

`kernel/vmm/vdev/vgicv3/vgicr3.c` · 4 个函数 · 4 个有说明 · 286 行

GICv3 里每个核有一个 128KB 的 Redistributor, 分成两个 64KB 帧：
RD_base  (0x00000)：GICR_CTLR / TYPER / WAKER / PROPBASER …
SGI_base (0x10000)：这个核的 SGI 和 PPI(INTID 0..31)

GICv2 把这些放在 GICD 的 banked 寄存器里, 所以这是 GICv3 新增的一块。
关键点：
- GICR_WAKER.ChildrenAsleep 必须读回 0, 否则 guest 的
gic_enable_redist() 会一直等下去
- GICR_TYPER.PLPIS/VLPIS 必须为 0(我们不实现 LPI),
Last 置位表示这是最后一个 redistributor
- guest 使能 PPI 27(虚拟定时器)时要把宿主侧的定时器注入路由打开

<a id="fn-kernel-vmm-vdev-vgicv3-vgicr3.c-48"></a>

### `redist_cpu`

```c
static int redist_cpu(uint64_t off, const vgic3_t *vgic, uint32_t vcpu_id)
```

取出本次访问落在哪个 vCPU 的 redistributor 上

<details>
<summary>函数内备注（1 条）</summary>

**第 55 行（行尾）**

> 偏移越界时退回访问者所属 vCPU

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicr3.c:48`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicr3.c-59"></a>

### `vgic3r_read`

```c
static uint64_t vgic3r_read(mmio_device_t *dev, uint64_t off, uint8_t size, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 SGI/RD 帧区分偏移，返回 group、使能/挂起/active、优先级、ICFGR 与 redistributor 的 CTLR/TYPER/WAKER 等值，未知返回 0。

<details>
<summary>函数内备注（10 条）</summary>

**第 62 行**

> 本核窗口内的偏移(0x00000..0x1ffff), 不能只取低 16 位 ——
> 那会把 SGI 帧的 0x10000 选择位一起掩掉, 所有 SGI/PPI 访问都会
> 落进 RD 帧分支被静默丢弃。

**第 77 行**

> ── SGI 帧 ──

**第 79 行（行尾）**

> GICR_IGROUPR0

**第 82 行（行尾）**

> ISENABLER0 / ICENABLER0

**第 85 行（行尾）**

> ISPENDR0 / ICPENDR0

**第 88 行（行尾）**

> ISACTIVER0 / ICACTIVER0

**第 117 行**

> ── RD_base 帧 ──

**第 120 行**

> EnableLPIs(bit0) 强制读回 0 —— 本模型不支持 LPI(GICR_TYPER.PLPIS=0,
> GICD_TYPER.LPIS=0), 按规范这一位就是 RAZ/WI。
> RWP(bit31) 恒 0：所有写立即生效。

**第 130 行**

> [4]=Last(每个核一个 redistributor, 最后一个是自己)；
> PLPIS/VLPIS=0；ProcessorNumber/位图放在 [31:8]/[63:32]。

**第 137 行**

> ChildrenAsleep(bit2) 恒 0：唤醒立即完成(背后没有真实硬件)。
> ProcessorSleep 按 guest 写进去的值回读, 语义与硬件一致。

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicr3.c:59`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicr3.c-158"></a>

### `vgic3r_write`

```c
static void vgic3r_write(mmio_device_t *dev, uint64_t off, uint8_t size, uint64_t value, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 SGI/RD 帧偏移写入 group、set/clear 使能与挂起、优先级、ICFGR，并把 RD 的 CTLR/WAKER/PROPBASER/PENDBASER 存入 vCPU。

<details>
<summary>函数内备注（7 条）</summary>

**第 178 行（行尾）**

> GICR_ISENABLER0

**第 185 行（行尾）**

> GICR_ICENABLER0

**第 191 行（行尾）**

> GICR_ISPENDR0

**第 197 行（行尾）**

> GICR_ICPENDR0

**第 201 行（行尾）**

> GICR_ISACTIVER0

**第 207 行（行尾）**

> GICR_ICACTIVER0

**第 245 行**

> 只跟踪 ProcessorSleep；写 0 表示唤醒, 之后 ChildrenAsleep 读到 0

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicr3.c:158`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicr3.c-272"></a>

### `vgic3r_init`

```c
int vgic3r_init(mmio_device_t *dev, mmio_bus_t *bus, vgic3_t *vgic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

填 ops/priv 并注册 GICR MMIO 设备，成功后打印日志。

**位置**：`kernel/vmm/vdev/vgicv3/vgicr3.c:272`

<a id="mod-kernel-vmm-vdev-vgicv3-vgicv3.c"></a>

## kernel/vmm/vdev/vgicv3/vgicv3.c — VM 级虚拟 GICv3 核心

`kernel/vmm/vdev/vgicv3/vgicv3.c` · 32 个函数 · 32 个有说明 · 689 行

职责划分(对应 GICv2 版的 vgic.c + vgicc.c 的 LR 部分)：
- 维护 guest 中断的 pending / enabled / active 软件状态
- 把这些状态通过 ICH_LR<n>_EL2 推给硬件(guest 侧由 ICV_* 直接消费)
- 退出 guest 后从 LR.State / ICH_ELRSR_EL2 回推软件状态

GICv2 版是「VMM 替代 guest 应答」(GICC_IAR 被陷入后由软件返回 IRQ 号)；
GICv3 的 CPU interface 是系统寄存器, VMM 无法陷入, 只能做上面这套
「写入 LR + 回读状态」的硬件辅助模型。

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-31"></a>

### `valid_vcpu`

```c
static int valid_vcpu(const vgic3_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 vgic3 非空且 vcpu_id 小于其 vCPU 数。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:31`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-36"></a>

### `valid_irq`

```c
static int valid_irq(uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回中断号是否小于 VGIC3_MAX_IRQS。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:36`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-41"></a>

### `irq_bit`

```c
static uint32_t irq_bit(uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 1 << (irq & 31)。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:41`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-48"></a>

### `lr_vintid`

```c
static uint32_t lr_vintid(uint64_t lr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 LR 值中取出 INTID 并返回。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:48`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-54"></a>

### `lr_state`

```c
static uint32_t lr_state(uint64_t lr)
```

返回 State 字段的原始取值(0..3), 不是掩码

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:54`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-66"></a>

### `vgic3_lr_value`

```c
static uint64_t vgic3_lr_value(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

vgic3_lr_value — 组一个「纯软件注入」的 Group1 中断 LR

State=Pending, HW=0(没有对应的物理中断需要硬件联动),
优先级取 guest 自己给该 INTID 配的值 —— guest 的 ICH_VMCR_EL2.VPMR
也是它自己写的, 两者同源才能保证「配置合法就该投递」。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:66`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-79"></a>

### `lr_has_irq`

```c
static int lr_has_irq(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历有效 LR，存在非零且 INTID 匹配的项则返回 1。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:79`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-89"></a>

### `lr_empty_slot`

```c
static int lr_empty_slot(const vgic3_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在硬件 LR 数量范围内找第一个值为 0 的槽，找不到返回 -1。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:89`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-105"></a>

### `vmm_vgic3_init`

```c
int vmm_vgic3_init(vgic3_t *vgic, uint32_t nr_vcpus)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验并限制 vCPU 数，清零结构，给每 vCPU 置 SGI 使能、默认优先级、group 与 VMCR/AP1R0 初值。

<details>
<summary>函数内备注（2 条）</summary>

**第 117 行**

> SGIs 永远使能(GICv3 下 SGI 的使能位同样是 banked 的, 但 Linux
> 从不单独使能 SGI, GICD_TYPER 也不报告它们)；PPI 初始为禁止。

**第 125 行**

> ICH_VMCR_EL2 的初值(每个 vCPU 一份)。

> 注意： 不能全 0：VPMR(bit[31:24], guest 的 ICC_PMR_EL1)在 ARM 里
> 是优先级屏蔽, 值 0 表示&quot;屏蔽一切优先级&quot;—— 全 0 的初值会让
> guest 在写下自己的 PMR 之前一个中断都收不到, 于是 vm1 直接卡在
> 启动早期(实测：改成全 0 后连单 VM 都起不来)。

> 全放行 = VPMR=0xFF(最低优先级), VENG1=1(Group1 使能)。

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:105`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-164"></a>

### `vmm_vgic3_hw_init`

```c
void vmm_vgic3_hw_init(void)
```

vmm_vgic3_hw_init — 打开本 CPU 的 ICH_HCR_EL2.En

不开这个位, 硬件在任何 ICC_IAR1_EL1 读上都只会返回 spurious, guest 永远
收不到虚拟中断。

注意： ICH_HCR_EL2 是每 CPU 的系统寄存器, 必须在真正跑 vCPU 的那个核上
设置 —— 它不是「VM 创建时设一次」的东西。两者并不总是同一个核：
- vcpu_task_create() 把 vCPU 任务钉在 CPU0(`task_set_cpu_affinity(t, 0)`)；
- 而 vm_create() 跑在调用者的核上。从 /dev/vmm 启动时, 那是用户态 helper
的 write 系统调用所在的核, SMP>1 时完全可能是 CPU1。

实测(GIC=v3 + SMP=2, QEMU)：En=1 落在 CPU1, guest 跑在 CPU0 → guest 永远
收不到虚拟中断, 卡在启动中途。症状很有迷惑性：guest 照常启动到设备探测完,
然后停住；宿主侧看到的是 vtimer 注入计数一路涨, 而 guest 的 CNTV_CVAL 再也
不变(它压根没收到中断, 自然没重编程)。SMP=1 时只有一个核, 所以从没暴露。

因此本函数在每次进入 guest 前调用(见 el2_run.c 的
vmm_arch_restore_guest_ctx)。对同一个核重复写是幂等的, 代价只有几条指令。

<details>
<summary>函数内备注（2 条）</summary>

**第 168 行**

> En=1；维护中断相关位保持 0 —— 我们在每次 VM exit 主动轮询
> ICH_ELRSR_EL2/ICH_EISR_EL2, 不需要 guest EOI 时打断自己。

> TDIR=1：guest 的 ICC_DIR_EL1/ICV_DIR_EL1(split EOI 模式的
> deactivate)必须陷入。active 中断在 vCPU 切换时会被移出 LR, 硬件
> DIR 找不到 LR 就会把这次 deactivation 丢掉, guest 无法恢复。
> 由 vmm_vgic3_deactivate() 在软件里收口。

**第 180 行**

> 只打第一次。带上 cpu 号：本函数的调用核就是 vCPU 的运行核, 上面那个
> SMP&gt;1 的 bug 里, 「En 设在哪一核」是唯一的线索。

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:164`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-194"></a>

### `vmm_vgic3_set_dist_enabled`

```c
void vmm_vgic3_set_dist_enabled(vgic3_t *vgic, int enabled)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若 vgic3 存在则设置 dist_enabled。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:194`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-200"></a>

### `vmm_vgic3_dist_enabled`

```c
int vmm_vgic3_dist_enabled(const vgic3_t *vgic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 dist_enabled，空返回 0。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:200`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-205"></a>

### `vmm_vgic3_set_sgi_pending`

```c
void vmm_vgic3_set_sgi_pending(vgic3_t *vgic, uint32_t vcpu_id, uint32_t source_vcpu, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 vCPU 与 SGI 号，置源位图位与 pending0 位。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:205`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-218"></a>

### `vmm_vgic3_deactivate`

```c
void vmm_vgic3_deactivate(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 INTID 在 LR 中找 active 项：仍 pending 则退回 pending，否则清 LR，并清软件 active；LR 无该项时只清软件 active。

<details>
<summary>函数内备注（4 条）</summary>

**第 226 行**

> DIR 的语义：Active -&gt; Invalid；Active+Pending -&gt; Pending。
> LR 里没有对应 INTID 时(active 状态此前被移到软件位), 硬件什么
> 也不做(只增加 EOICount), 所以下面无论如何都要清软件 active。

**第 239 行（行尾）**

> Pending 上的 DIR 是 UNPREDICTABLE, 忽略

**第 245 行**

> 软硬件都记着：DIR 后它仍然 pending。

**第 258 行**

> LR 里没有：这正是 split EOI 模式下 vCPU 切换造成的情况 ——
> active 中断已经被移到 active0/spi_active。清掉软件 active 位。
> 如果同一个 INTID 在 active 期间又来了新边沿, pending 位保持不动,
> 下一次 sync_entry 会重新注入。

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:218`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-267"></a>

### `vmm_vgic3_set_pending`

```c
void vmm_vgic3_set_pending(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验后按 SGI/PPI/SPI 分别置 SGI 源位与 pending0 或 spi_pending 位。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:267`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-281"></a>

### `vmm_vgic3_set_enabled`

```c
void vmm_vgic3_set_enabled(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq, int enabled)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略 SGI，PPI 改该 vCPU enabled0，SPI 改全局 enabled 字，按值置位或清位。

<details>
<summary>函数内备注（1 条）</summary>

**第 284 行（行尾）**

> SGI 使能位不参与投递判定, 忽略

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:281`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-300"></a>

### `vmm_vgic3_enabled_word`

```c
uint32_t vmm_vgic3_enabled_word(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t word)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 返回该 vCPU enabled0|SGI 掩码，其它返回全局 enabled 字。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:300`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-309"></a>

### `vmm_vgic3_pending_word`

```c
uint32_t vmm_vgic3_pending_word(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t word)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 返回 pending0，其它返回该 vCPU spi_pending 字。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:309`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-318"></a>

### `vmm_vgic3_active_word`

```c
uint32_t vmm_vgic3_active_word(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t word)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 返回 active0，其它返回该 vCPU spi_active 字。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:318`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-327"></a>

### `vmm_vgic3_clear_pending_word`

```c
void vmm_vgic3_clear_pending_word(vgic3_t *vgic, uint32_t vcpu_id, uint32_t word, uint32_t bits)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 清 pending0 位，否则清 spi_pending 字位。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:327`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-337"></a>

### `vmm_vgic3_clear_active_word`

```c
void vmm_vgic3_clear_active_word(vgic3_t *vgic, uint32_t vcpu_id, uint32_t word, uint32_t bits)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

word0 清 active0 位，否则清 spi_active 字位。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:337`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-349"></a>

### `set_active`

```c
static void set_active(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按中断号在 active0 或 spi_active 中置对应位。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:349`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-357"></a>

### `clear_active`

```c
static void clear_active(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按中断号清 active0 或 spi_active 中对应位。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:357`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-365"></a>

### `clear_pending`

```c
static void clear_pending(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按中断号清 pending0（SGI 同时清源位）或 spi_pending 位。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:365`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-388"></a>

### `ich_read_vmcr`

```c
static uint64_t ich_read_vmcr(void)
```

── per-CPU 的 ICH_*_EL2 访问(见 vgic3_vcpu_t 里 ich_vmcr 的注释)─────

VMCR 和 AP1R0 都是 per-pCPU 硬件, 但内容属于当前在上面跑的 vCPU,
所以按 vCPU 保存/恢复。实测只按 VM 存一份时, 同 VM 的 vCPU 轮转会把
对方的 PMR/IGRPEN/active priority 覆盖掉：vtimer 的 LR 卡在 Active,
guest idle 后再也醒不过来 —— vCPU 越多(2 分 8)越容易命中。

注意： 只碰 AP1R0。ICH_AP1R1/2/3_EL2 在 QEMU 上实测会触发 "Unknown reason"
(EC=0x0), 连单 VM 都起不来；当前优先级位数下 AP1R0 已覆盖。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:388`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-395"></a>

### `ich_write_vmcr`

```c
static void ich_write_vmcr(uint64_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用内联汇编把参数写入 ICH_VMCR_EL2。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:395`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-400"></a>

### `ich_read_ap1r0`

```c
static uint64_t ich_read_ap1r0(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用内联汇编读取 ICH_AP1R0_EL2 并返回。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:400`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-407"></a>

### `ich_write_ap1r0`

```c
static void ich_write_ap1r0(uint64_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用内联汇编把参数写入 ICH_AP1R0_EL2。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:407`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-442"></a>

### `vmm_vgic3_lr_switch_in`

```c
void vmm_vgic3_lr_switch_in(vgic3_t *vgic, vcpu_t *vcpu)
```

vmm_vgic3_lr_switch_in — 进入 guest 前调用：确保本核的 LR 属于给定的
vCPU, 然后把可投递的中断排进去。

归属变化时的顺序不能反：
① 先 sync_exit(上一个) —— 把硬件里仍 active/pending 的项回收进上一个
VM 自己的软件镜像(它的 guest 可能已经 ack 但还没 EOI, 状态不能丢)；
② 再清空全部 LR —— 让新 VM 从一个干净的硬件状态开始；
③ 最后 sync_entry(当前) 排入当前 VM 的中断。

反过来(先清再 sync_exit)会把上一个 VM 尚未 EOI 的中断状态直接抹掉。

<details>
<summary>函数内备注（4 条）</summary>

**第 455 行**

> 持上一个 vCPU 的 irq_lock。

> 它的 exit 路径已经 sync 过一次, 但这里还要再同步：远端 CPU
> 可能正对着它调用 vmm_vcpu_inject_irq(SGI/控制台/NPU), 而
> sync_exit 会对 active 的 LR 执行 clear_pending。不加锁的话,
> 刚置上的 pending 会被这次&quot;补同步&quot;抹掉 —— 表现是 guest 丢了
> 一次 IPI, SMP 启动或 RCU 握手随机卡死。

**第 467 行**

> 把本 CPU 的 ICH_*_EL2 硬件状态存回上一个 vCPU 自己,
> 稍后它被调度回来时再装回去(见下面的 restore)。
> 顺序：先 sync_exit(回收 LR 语义)→ 再存 ICH → 再清 LR。

**第 475 行**

> 接下来要清空本核硬件 LR。对仍在 LR 里, 还没被 guest ack
> 的边沿 SGI, 镜像里只有它, 软件 pending 已经在注入时消费掉了；
> 把 pending 位放回软件状态, 切回来才能重新投递。

> 必须放在这里(确定要切走时), 不能放在 sync_exit：同一个
> vCPU 正常退出再进入时不会清 LR, 那时重放 pending 会让同一个
> SGI 在 EOI 后被重复投一次。

**第 500 行**

> 装入当前 vCPU 的 ICH 状态(首次进入时是 init 给的默认值)

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:442`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-511"></a>

### `vmm_vgic3_sync_entry`

```c
void vmm_vgic3_sync_entry(vgic3_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 ELRSR 回收空 LR，补装软件 active 但 LR 缺失的中断，再把 pending&enabled&~active 项装入空槽，SGI 入槽即清 pending。

<details>
<summary>函数内备注（7 条）</summary>

**第 518 行**

> 1) 先按 ICH_ELRSR_EL2 回收槽位。
> ELRSR[i]=1 表示硬件认为 LR i 已空(guest 已 EOI, 或从未被占用),
> 此时清掉镜像, 后续才可能把新的中断排进去。

**第 530 行**

> 1b) 把软件记着 active, 但硬件 LR 里已经没有的中断重新装回 LR,
> 状态只置 Active(不带 Pending)。

> 为什么必须补这一步(实测第二个 VM 卡在 init 的根因)：
> 多 VM 分时跑时, 切换 VM 必须清空硬件 LR 让给下一个 VM, 于是
> &quot;guest 已 ack, 但还没 EOI&quot;的中断只能记在软件 active 位里。等它被
> 调度回来时, 若不把那个中断装回 LR, guest 随后写的
> ICC_EOIR1_EL1 落在一个空槽上 —— 硬件无事发生, ELRSR 不会置位,
> 我们也收不到通知 ⇒ 软件 active 位永远没人清。
> 而 sync_entry 的排队条件是 `pending &amp; enabled &amp; ~active`,
> 于是这个 INTID 被永久堵死：宿主侧看 vtimer 注入计数一路涨,
> guest 的 CNTV_CVAL 却再也不变。

> 装回去之后闭环就自洽了：guest EOI → 硬件清 LR, ELRSR 置位 →
> 下次 sync_exit 的 &quot;槽位已空&quot; 分支 → clear_active。

> 注意： 这一段正是「中断接口只统一到线, 不统一控制器」的理由(见
> include/vmm/vmm_virq.h §③)：sync_entry/sync_exit 是 vGIC 独有的
> 硬件协助阶段, PLIC/vLAPIC 没有对应物。若把它们塞进一张通用 ops 表,
> 缺的只是空实现, 将来有人在那张表上写调用点会得到静默无操作
> 而不是编译错误 —— 这条不变量的成因就是「某个动作没做而没人报错」。

**第 564 行（行尾）**

> 已经在 LR 里了

**第 568 行（行尾）**

> 槽位不够, 留给下一轮

**第 577 行**

> 2) 扫全部 INTID, 把 pending &amp; enabled &amp; !active 且尚未排入 LR 的
> 写进空槽位。

**第 604 行（行尾）**

> LR 用满

**第 610 行**

> SGI 是边沿触发：这个 pending 位在写进 LR 的这一刻就被
> 「消费」了。必须现在清掉, 之后 guest 再发同一 INTID 的 SGI
> 才能重新把 pending 置起来(否则两次 IPI 会被合并成一次)。

> PPI/SPI 不走这条：它们是电平语义, pending 要保留到 guest
> ack(sync_exit 里清), 设备侧每轮 entry 会重新拉线。

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:511`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-640"></a>

### `vmm_vgic3_sync_exit`

```c
void vmm_vgic3_sync_exit(vgic3_t *vgic, uint32_t vcpu_id)
```

LR.State 由硬件维护：
Pending          —— 已排入, guest 还没应答
Active&Pending   —— guest 已读 IAR(ack), 还没写 EOIR
(槽位消失)        —— guest 已写 EOIR(硬件把 LR 置 Invalid,
ELRSR 对应位置 1)

所以：
槽位仍在 && State 含 Active  → 这次 guest 应答了 → pending 转 active
槽位消失 && 之前有镜像        → guest 完成了 EOI    → 清 active,
等设备侧下次置 pending 再投

<details>
<summary>函数内备注（3 条）</summary>

**第 656 行**

> 槽位已空：guest 走完了 ack+EOI(或硬件丢弃)

**第 663 行**

> 槽位仍被占用：回读硬件状态

**第 668 行**

> guest 已应答。

> PPI/SPI(电平)：挂起位转 active 记帐, 避免重复投递。
> SGI(边沿)：不能清 pending —— 注入时已经清过一次,
> 这里再清会把「active 期间新到的同号 IPI」抹掉, guest 就会
> 丢一次跨核唤醒(SMP 启动/RCU 握手随机卡死的经典来源)。

</details>

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:640`

<a id="fn-kernel-vmm-vdev-vgicv3-vgicv3.c-686"></a>

### `vmm_vgic3_inject_timer`

```c
void vmm_vgic3_inject_timer(vgic3_t *vgic, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 VIRQ_VTIMER 置为 pending。

**位置**：`kernel/vmm/vdev/vgicv3/vgicv3.c:686`

<a id="mod-kernel-vmm-vdev-vlapic.c"></a>

## kernel/vmm/vdev/vlapic.c — x86_64 虚拟 LAPIC(xAPIC / MMIO 模式)

`kernel/vmm/vdev/vlapic.c` · 25 个函数 · 25 个有说明 · 767 行

移植自 tgoskits `virtualization/x86_vlapic`(Rust, ~4400 行), 只取
「Linux 能启动」所必需的最小子集。上游那些完整实现(vIOAPIC/vPIC/PIT/
x2APIC/MSI)留着, 等需要外部设备中断时再按同一结构补。

传输方式：MMIO @0xFEE00000。
EPT 把 guest RAM 之外的所有 GPA 都置成无效(x86_ept_enable_mmio_trap),
而 0xFEE00000 正在其中 —— 于是 guest 的每次 LAPIC 访问天然变成
EPT violation, 走到这里, 不需要 VMX 的 APIC-access page 机制。

定时器：宿主 tick 驱动(轮询, 不注册宿主定时器)。
每次进 guest 前调 vlapic_timer_poll(), 看虚拟 deadline 是否已过。
因为开了 PIN_EXTINT, 宿主 100Hz 的时钟中断本身就会让 guest 每 10ms
退出来一次 —— 这个频率正好充当轮询节拍, 不需要额外的定时器基础设施。

多 vCPU（x86 SMP guest）：
- INIT/SIPI 由 guest 写 ICR 触发, SIPI 经 vmm_vcpu_start() 放行
睡在 completion 上的 AP 任务(实模式 CS base = vector << 12)；
- fixed/lowest IPI 按 physical、logical flat 与 shorthand 解析目标,
经 vmm_vcpu_inject_vector() 投递(持目标 irq_lock 后 kick)；
- 每个 vCPU 一份 LAPIC 状态(ID/LDR/IRR/ISR/定时器), 由 vlapic_of()
按 (slot, vcpu_id) 取。

有意没做的(第一版不需要)：
- x2APIC：APIC base MSR 的 EXTD 位一律清掉, Linux 探测到之后会
安心走 MMIO 路径(tgoskits 的官方 Linux 配置也是 `nox2apic`)。
- 对已 RUNNING 的 vCPU 的 INIT(复位/热插拔)：不复位 VMCS。
- TPR/PPR 门控：IRR → 投递这一步不看 TPR/PPR(上游也不看), 只按
向量编号取最大的。Linux 的 TPR 常为 0, 等价；真要做实时优先级, 得把
update_ppr() 的结果接进 vlapic_take_pending()。
注意 accept_interrupt 是在投递那一刻置 ISR 位的(不是 guest 取走
之后), 所以 ISR 反映的是「我们发过, 还没收到 EOI」。
- LVT LINT0/LINT1/THERMAL/PMI/CMCI：只存值, 不产生行为。上游同样,
PIC 的中断不经过 LINT0, 而是 hypervisor 主动注入。

移植时改掉的上游坑(照抄会打挂宿主)：
- 上游对未列出的寄存器偏移直接 panic；这里一律「读回 0, 写忽略」。
- 上游 ICR 用 lowest-priority 模式会 `unimplemented!()`；这里降级为忽略。

<a id="fn-kernel-vmm-vdev-vlapic.c-124"></a>

### `vlapic_of`

```c
static vlapic_state_t *vlapic_of(const vm_t *vm, uint32_t vcpu_id)
```

取 (vm, vcpu_id) 的 LAPIC 状态。

除范围检查还要比对 owner —— vm_free 会把整个 vm_t memset 成 0(含 slot),
并发窗口里已释放的 vm 指针会索引到 0 号槽(另一个 VM 的 LAPIC)。
owner 由 init 写入, destroy 清成 NULL。

返回 NULL 时调用方必须能容忍(见各调用点的守卫)。

**位置**：`kernel/vmm/vdev/vlapic.c:124`

<a id="fn-kernel-vmm-vdev-vlapic.c-144"></a>

### `vlapic_now_ns`

```c
uint64_t vlapic_now_ns(void)
```

── 时间源 ─────────────────────────────────────────────────

用宿主已标定的 TSC(g_tsc_freq_hz, 由 lapic_timer_init 经 PIT 测出)
换算成纳秒。毫秒级的 timer_get_uptime_ms() 太粗 —— Linux 的 LAPIC
timer 周期通常只有 1~10ms, 用毫秒粒度会让它的校准量出错误的值。

除数先化成 MHz 再乘, 避免 tsc(2.4e12) * 1e9 溢出。

<details>
<summary>函数内备注（1 条）</summary>

**第 153 行（行尾）**

> 还没标定：调用方按 0 处理

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:144`

<a id="fn-kernel-vmm-vdev-vlapic.c-165"></a>

### `vlapic_add_exec_ns`

```c
void vlapic_add_exec_ns(vm_t *vm, uint32_t vcpu_id, uint64_t ns)
```

Guest 执行时间累加。定时器的一切 "now" 都用这个时钟，不用墙钟 ——
原因见 vmm_vlapic.h 里 exec_ns 的说明。

**位置**：`kernel/vmm/vdev/vlapic.c:165`

<a id="fn-kernel-vmm-vdev-vlapic.c-176"></a>

### `vec_highest`

```c
static int vec_highest(const uint32_t *bits)
```

在 256 位位图里找编号最大的置位向量(-1 = 一个都没有)

**位置**：`kernel/vmm/vdev/vlapic.c:176`

<a id="fn-kernel-vmm-vdev-vlapic.c-186"></a>

### `isr_highest`

```c
static int isr_highest(const vlapic_state_t *v)
```

找到 ISR 中编号最大的已置位向量(EOI 从这里开始退)

**位置**：`kernel/vmm/vdev/vlapic.c:186`

<a id="fn-kernel-vmm-vdev-vlapic.c-191"></a>

### `priority_class`

```c
static uint8_t priority_class(uint32_t vec)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取中断向量号高 4 位作为优先级类返回。

**位置**：`kernel/vmm/vdev/vlapic.c:191`

<a id="fn-kernel-vmm-vdev-vlapic.c-197"></a>

### `update_ppr`

```c
static void update_ppr(vlapic_state_t *v)
```

PPR = max(TPR, 当前正在服务的最高优先级)

**位置**：`kernel/vmm/vdev/vlapic.c:197`

<a id="fn-kernel-vmm-vdev-vlapic.c-206"></a>

### `vlapic_accept_interrupt`

```c
void vlapic_accept_interrupt(vm_t *vm, uint32_t vcpu_id, uint32_t vector, int level_triggered)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验向量后置对应 ISR 位，电平触发时同时置 TMR，并更新 PPR。

**位置**：`kernel/vmm/vdev/vlapic.c:206`

<a id="fn-kernel-vmm-vdev-vlapic.c-218"></a>

### `process_eoi`

```c
static void process_eoi(vlapic_state_t *v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

找 ISR 中最高优先级向量，清其 ISR 与 TMR 位并更新 PPR。

**位置**：`kernel/vmm/vdev/vlapic.c:218`

<a id="fn-kernel-vmm-vdev-vlapic.c-238"></a>

### `dcr_to_shift`

```c
static uint32_t dcr_to_shift(uint32_t dcr)
```

DCR 编码 → 分频指数(Intel SDM Vol 3A 表 11-19)

注意：这张表曾经整体错位一格(0x3 当成 ÷8)，于是 Linux 按 ÷16 算出来的
INIT 值在这里被当成 ÷8 执行 —— guest 的 tick 快一倍，jiffies 也快一倍。
单 vCPU 时只是"有点怪"，多 vCPU 共享宿主核时会把 CPU 全吃在 tick /
softirq 上，guest 的 CPU bring-up 迟迟推进不了。编码 0xB 是 ÷1。

<details>
<summary>函数内备注（9 条）</summary>

**第 242 行（行尾）**

> ÷2

**第 244 行（行尾）**

> ÷4

**第 246 行（行尾）**

> ÷8

**第 248 行（行尾）**

> ÷16

**第 250 行（行尾）**

> ÷32

**第 252 行（行尾）**

> ÷64

**第 254 行（行尾）**

> ÷128

**第 256 行（行尾）**

> ÷1

**第 258 行（行尾）**

> 保留编码：按 ÷1

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:238`

<a id="fn-kernel-vmm-vdev-vlapic.c-262"></a>

### `timer_start`

```c
static void timer_start(vlapic_state_t *v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读初始计数与 LVT 模式；为 0 或被屏蔽则不启动，否则按 shift 换算间隔（周期模式设下限），置 deadline 并激活。

<details>
<summary>函数内备注（4 条）</summary>

**第 271 行（行尾）**

> 写 0 = 停

**第 273 行（行尾）**

> LVT 屏蔽着：不启动

**第 275 行**

> interval_ns = (init &lt;&lt; shift) / VLAPIC_TIMER_HZ * 1e9
> 由于 VLAPIC_TIMER_HZ 就是 1e9, tick 数直接等于纳秒数。

**第 281 行（行尾）**

> 周期模式的下限, 照抄 KVM

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:262`

<a id="fn-kernel-vmm-vdev-vlapic.c-288"></a>

### `timer_stop`

```c
static void timer_stop(vlapic_state_t *v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把定时器 active 标志清零。

**位置**：`kernel/vmm/vdev/vlapic.c:288`

<a id="fn-kernel-vmm-vdev-vlapic.c-294"></a>

### `timer_fire`

```c
static void timer_fire(vm_t *vm, uint32_t vcpu_id, vlapic_state_t *v)
```

定时器到点 → 排一个待注入向量(投给拥有这个定时器的那颗 vCPU)

<details>
<summary>函数内备注（2 条）</summary>

**第 298 行（行尾）**

> 向量合法性(≥16)在内部把关

**第 301 行**

> 追赶：落后多个周期时不要补发一堆, 直接跳到未来的第一个点

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:294`

<a id="fn-kernel-vmm-vdev-vlapic.c-315"></a>

### `vlapic_timer_poll`

```c
int vlapic_timer_poll(vm_t *vm, uint32_t vcpu_id)
```

vlapic_timer_poll — 每次进 guest 前调用

返回 1 表示这次轮询产生了待注入的定时器中断。

<details>
<summary>函数内备注（1 条）</summary>

**第 323 行（行尾）**

> APIC 被软件关掉：什么都不投

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:315`

<a id="fn-kernel-vmm-vdev-vlapic.c-336"></a>

### `vlapic_init`

```c
void vlapic_init(vm_t *vm, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取本 VM/vCPU 的 vLAPIC 槽并清零，设 ID、版本、DFR/LDR、APIC base（BSP 位仅 0 号），更新 PPR 并置 enabled。

<details>
<summary>函数内备注（6 条）</summary>

**第 343 行**

> 直接取槽位：of() 要校验 owner, 而 owner 正是本函数要写的

**第 350 行**

> ID：Linux 用它做拓扑/自检。我们的 APIC ID 约定等于 vcpu_id
> (MP 表、CPUID.1.EBX[31:24] 与这里必须是同一个值)。
> 注意 ID 寄存器只读 —— 写它必须被忽略, 否则 Linux 会认为
> 「APIC ID 可写」而走进不该走的分支。

**第 357 行**

> VERSION：0x14 = 版本 20, bits 16-23 = 最大 LVT 项编号 6。
> Linux 用 bit24 判断「EOI 广播抑制」是否支持 —— 不置位。

**第 362 行**

> ── 逻辑 destination（flat 模型）─────────────────────────────

> Linux 在核数 &lt;= 8 时用 apic_flat：IPI 走 logical 模式, ICR 高 8 位
> 是「CPU 位图」, 目标 CPU 的 LDR 在同一个位上。原来的 0x01000000 把
> 所有 vCPU 的逻辑 ID 都设成 0 —— 单核没事, 多核时每一条 logical IPI
> 都会投给所有 vCPU(或只投给 vcpu0)，而且目标是靠 guest 自己写 LDR
> 纠正的, 早期 IPI 会丢。
> guest 之后会自己写 LDR, 这里给的是与 APIC ID 一致的默认值。

**第 372 行（行尾）**

> flat 模型

**第 375 行**

> 上电默认值：基址 0xFEE00000 | EN(11) | BSP(8)。EXT D 位恒 0 —— 本
> 实现不做 x2APIC, Linux 探测到之后会安心走 MMIO 路径。
> BSP 位只有 0 号 vCPU 置位：AP 的 IA32_APIC_BASE 里这一位是只读的
> 拓扑信息, 全置 1 会让 Linux 认为每颗 CPU 都是 boot CPU。

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:336`

<a id="fn-kernel-vmm-vdev-vlapic.c-393"></a>

### `vlapic_destroy`

```c
void vlapic_destroy(vm_t *vm)
```

vlapic_destroy — 归还本 VM 的全部 LAPIC 槽位
(由 vmm_arch_vm_destroy → vm_free 调用)

幂等, 且对"从未 init 过的槽"安全(槽位是全零 static)。
本设备没有锁, 所以不需要 vpl011_destroy 里那条"永不 memset 锁"的约束。

<details>
<summary>函数内备注（1 条）</summary>

**第 403 行（行尾）**

> of() 随即失效

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:393`

<a id="fn-kernel-vmm-vdev-vlapic.c-407"></a>

### `vlapic_apic_base`

```c
uint64_t vlapic_apic_base(vm_t *vm, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回该 vCPU vLAPIC 的 APIC base，找不到返回 0。

**位置**：`kernel/vmm/vdev/vlapic.c:407`

<a id="fn-kernel-vmm-vdev-vlapic.c-414"></a>

### `vlapic_set_apic_base`

```c
void vlapic_set_apic_base(vm_t *vm, uint32_t vcpu_id, uint64_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若 vLAPIC 存在则写入其 APIC base 字段。

**位置**：`kernel/vmm/vdev/vlapic.c:414`

<a id="fn-kernel-vmm-vdev-vlapic.c-434"></a>

### `vlapic_raise_irq`

```c
void vlapic_raise_irq(vm_t *vm, uint32_t vcpu_id, uint32_t vector)
```

拉高一条中断线(电平触发语义)

三个来源都走这里进 IRR：LAPIC 定时器, ICR 自发中断, 外部设备(vUART)。
IRR 是位图不是单个槽 —— 之前只存一个 pending_vec, 定时器和设备中断
会互相覆盖：谁后到谁赢, 输的那个要等下次事件才补得回来(周期定时器甚至
会因此整拍丢掉)。

位在 vlapic_take_pending() 取走时才清, 所以电平触发的设备(UART RX
FIFO 里还有字节)必须每次入口重新拉一次, 与 vPLIC/vGIC 侧的约定一致
(见 vmm_console_irq_asserted 的注释)。

<details>
<summary>函数内备注（2 条）</summary>

**第 441 行（行尾）**

> 0-15 是异常, 不能当普通中断投

**第 443 行（行尾）**

> APIC 被软件关掉：什么都不投

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:434`

<a id="fn-kernel-vmm-vdev-vlapic.c-465"></a>

### `vlapic_take_pending`

```c
int vlapic_take_pending(vm_t *vm, uint32_t vcpu_id, uint32_t *vec)
```

待注入向量取走：IRR 里编号最大的, 且不在服务中的那个(取走后清位)

「不在服务中」这一条不能省：硬件的投递优先级是
向量优先级(编号) > 正在服务的最高优先级(ISR) > TPR
同一向量在 ISR 里挂着时, 新的中断只在 IRR 里排队, 要等 guest 写 EOI
才投得出去。

电平触发的设备(vUART 的 RX/THRE)会把线一直拉着, 少了这个门控就
变成「每个 VM-entry 投一次」—— 空闲时 guest 每条 HLT 退出都会吃一个
串口中断(实测 2.8 万次/秒量级), 串口驱动被中断风暴拖死。有了门控,
速率自然回落到「每次 EOI 一个」, 与真硬件一致。

注意： ISR 位是在投递那一刻由 vlapic_accept_interrupt() 置的, 所以这里
的前提是 guest 一定会 EOI(Linux 的中断处理开头就 EOI)。若某个向量投出去
而 guest 从不 EOI, 它会被永久挡住 —— 这是有意的：真硬件也是这个行为。

**位置**：`kernel/vmm/vdev/vlapic.c:465`

<a id="fn-kernel-vmm-vdev-vlapic.c-484"></a>

### `vlapic_sw_enabled`

```c
int vlapic_sw_enabled(vm_t *vm, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 vLAPIC 已建立、enabled 且 SVR enable 位已置。

**位置**：`kernel/vmm/vdev/vlapic.c:484`

<a id="fn-kernel-vmm-vdev-vlapic.c-493"></a>

### `reg_read`

```c
static uint32_t reg_read(vlapic_state_t *v, uint32_t idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按偏移返回 ID/PPR/ISR/TMR/IRR/定时器当前计数等值，只读项返回 0，未知偏移在范围内回存储值否则 0。

<details>
<summary>函数内备注（4 条）</summary>

**第 501 行（行尾）**

> 只读占位, 恒 0

**第 531 行**

> 真正回位图(上游是恒 0)。guest 用 IRR 判断「还有没有中断挂着」,
> 恒 0 会让它在中断处理里误判。注意这是只读的 —— 清位只有
> vlapic_take_pending()(投递)和 SVR 关闭两条路。

**第 538 行**

> 当前计数 = 剩余纳秒 &gt;&gt; shift(与写入时的换算互逆)

**第 543 行**

> 未实现/保留偏移：读回 0, 绝不 panic(上游会打挂宿主)

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:493`

<a id="fn-kernel-vmm-vdev-vlapic.c-562"></a>

### `icr_collect_targets`

```c
static int icr_collect_targets(vm_t *vm, uint32_t src, uint32_t lo, uint32_t hi, uint32_t *out, int max)
```

── ICR：目标 vCPU 解析 ──────────────────────────────────────

把一次 ICR 写翻译成「要投给哪些 vCPU」。APIC ID 约定等于 vcpu_id,
所以物理 destination 直接当 vCPU 编号用。

覆盖 Linux 实际会用到的三种形态：
- shorthand：self / all / all-but-self（IPI 广播、self-IPI）
- physical destination：apic_physflat（>8 核）与 INIT/SIPI 的目标
- logical flat：apic_flat（<=8 核）的 IPI，ICR[31:24] 是 CPU 位图

返回目标个数(0 = 没有匹配)。

<details>
<summary>函数内备注（3 条）</summary>

**第 583 行（行尾）**

> 保留编码

**第 588 行**

> flat 模型：LDR[31:24] 的每一位对应一个逻辑 ID

**第 596 行**

> 物理广播：xAPIC 里 0xFF 是「所有 CPU」

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:562`

<a id="fn-kernel-vmm-vdev-vlapic.c-605"></a>

### `reg_write`

```c
static void reg_write(vm_t *vm, uint32_t vcpu_id, vlapic_state_t *v, uint32_t idx, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按偏移处理 TPR/EOI/SVR/ESR/ICR 与 LVT、初始计数、分频：SVR 关闭时停表清 IRR，ICR 解析目标后发 SIPI 或注入 IPI。

<details>
<summary>函数内备注（10 条）</summary>

**第 609 行（行尾）**

> 只读

**第 623 行**

> 软件关掉 APIC：定时器一起停, 挂起的也丢掉(上游 write_svr 同此语义)

**第 627 行（行尾）**

> 重新使能 → 按原初值重启

**第 632 行（行尾）**

> 写 = 清挂起位

**第 647 行**

> ── INIT / SIPI：AP 启动 ──────────────────────────────────

> 写 ICR 的永远是 guest 的某颗 vCPU(这里就是 vcpu_id)。目标按
> destination/shorthand 解析, 与 IPI 同一套规则。

> INIT 在本实现里只起「把目标置于 wait-for-SIPI」的语义 —— 而
> AP 任务本来就在 completion 上睡到 SIPI, 所以对 OFFLINE 的 AP
> 什么都不用做。对已 RUNNING 的 vCPU 的 INIT(关机/热插拔)不复位,
> 真机语义需要重新初始化 VMCS, 当前不需要。
> SIPI 才是真正放行 AP 的那一步：vmm_vcpu_start 填实模式入口并
> 完成 completion。第二次 SIPI(目标已 RUNNING)返回 -1, 忽略。

**第 673 行（行尾）**

> SMI/NMI/ExtINT 等：未实现(上游会 panic)

**第 677 行**

> lowest-priority 的语义是「在候选里挑优先级最低的一颗」; 本实现
> 不做 PPR 仲裁, 简化为投给第一个候选。Linux 的 flat/physflat
> 驱动都走 fixed, 这条只是兜底。

**第 688 行**

> 走 vmm_vcpu_inject_vector 而不是直接 vlapic_raise_irq：
> 目标可能在另一颗宿主核上跑 guest, 必须持它的 irq_lock
> 写 IRR 再 kick, 否则 IPI 要等它下次自然退出才被看见。

**第 704 行（行尾）**

> 解除屏蔽 → 按初值重启

**第 719 行（行尾）**

> LVT LINT0/1, error 等：只存值

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:605`

<a id="fn-kernel-vmm-vdev-vlapic.c-729"></a>

### `vlapic_mmio_handle`

```c
int vlapic_mmio_handle(vm_t *vm, uint32_t vcpu_id, uint64_t addr, int is_write, uint8_t size, uint64_t *val)
```

vlapic_mmio_handle — EPT violation 转发进来的 LAPIC 访问

返回 1 = 已处理。addr 是 guest 物理地址(0xFEE00000 起)。

<details>
<summary>函数内备注（7 条）</summary>

**第 733 行（行尾）**

> 16 字节步长 → 寄存器编号

**第 736 行（行尾）**

> 未处理 —— 调用方按未映射设备兜底

**第 738 行（行尾）**

> 非对齐访问：不是 LAPIC 语义

**第 741 行（行尾）**

> xAPIC 只支持 32 位访问

**第 744 行（行尾）**

> 保留区：静默吞掉, 读 0

**第 751 行**

> TEMP-DBG：前 60 次 LAPIC 访问 —— 看内核到底读/写了哪些寄存器,
> 我的 vLAPIC 回了什么。APIC_ID(0x20) / SPIV(0xf0) / LVT0(0x35) /
> LVT1(0x36) / TPR(0x08) / SVR(0x0f) 是关键。

**第 759 行**

> KLOG_WARN(&quot;[LAPIC] #%u off=0x%03x idx=0x%02x %s val=0x%llx\n&quot;,
> n, off, idx, is_write ? &quot;WR&quot; : &quot;RD&quot;,
> (unsigned long long)(is_write ? *val : *val));

</details>

**位置**：`kernel/vmm/vdev/vlapic.c:729`

<a id="mod-kernel-vmm-vdev-vplic.c"></a>

## kernel/vmm/vdev/vplic.c — 虚拟 RISC-V PLIC 实现

`kernel/vmm/vdev/vplic.c` · 12 个函数 · 12 个有说明 · 367 行

移植自 x-kernel: virt/kvmm/src/vdev/riscv64/irq.rs, 适配 Avatar OS：
- Rust Atomic 数组 → 普通静态数组
- 仅支持前 64 源, context N == vCPU N

── 多 VM：状态与锁都搬进了 vm_t ─────────────────────────────

从前状态是个文件级 static(g_vplic), 整机只有一份 —— 第二个 VM 的
vplic_init() 一句 memset 就把第一个 VM 的 priority/enable/threshold 全部
清掉, 两个 VM 从此共用一个中断控制器且毫无报错。现在每 VM 一份。

锁是新加的(老代码一把都没有)：单 VM 时所有调用点确实都在同一个 vCPU
任务里, 但状态既然按 VM 分开了, 就该按"可能跨上下文"来写。加锁的形状与
vpl011/uart16550 一致, 且把 `_locked` 内部版和公开版分开, 避免 MMIO 回调
里出现"自己锁自己"。

注意： 约定：所有 `_locked` 后缀的函数要求调用方已持 vplic_lock。

<a id="fn-kernel-vmm-vdev-vplic.c-54"></a>

### `vplic_of`

```c
static vplic_slot_t *vplic_of(const vm_t *vm)
```

取本 VM 的槽位。除范围检查还要比对 st.owner —— vm_free 会把整个 vm_t
memset 成 0(含 slot), 并发窗口里已释放的 vm 指针会索引到 0 号槽
(另一个 VM 的 PLIC)。owner 由 init 写入, destroy 清成 NULL。

**位置**：`kernel/vmm/vdev/vplic.c:54`

<a id="fn-kernel-vmm-vdev-vplic.c-87"></a>

### `vplic_next_deliverable_locked`

```c
static uint32_t vplic_next_deliverable_locked(const vplic_state_t *s, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 context 后取 pending&enable&~active 候选，遍历按优先级阈值找最高优先级中断，无可投递返回 0。

<details>
<summary>函数内备注（4 条）</summary>

**第 92 行**

> pending 是源级的(全局), 本 context 只用 enable/active/threshold 过滤

**第 99 行**

> 注意：freestanding 构建下 __builtin_ctzll 会生成对 __ctzdi2 的调用
> (libgcc 未链接), 故用「低位清零」循环代替(与 hext_run.c 同风格)。

**第 105 行（行尾）**

> 取最低置位

**第 109 行（行尾）**

> 位序号

</details>

**位置**：`kernel/vmm/vdev/vplic.c:87`

<a id="fn-kernel-vmm-vdev-vplic.c-122"></a>

### `vplic_claim_locked`

```c
static uint32_t vplic_claim_locked(vplic_state_t *s, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

找到可投递中断后清全局 pending 位、置该 context active 位并返回中断号，否则 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 133 行**

> claim 清的是**全局**挂起位：这是 PLIC 的 claim 语义(对全体 context
> 生效), 也是多 vCPU 下同一个源不会被两个 context 各领一次的关键。

</details>

**位置**：`kernel/vmm/vdev/vplic.c:122`

<a id="fn-kernel-vmm-vdev-vplic.c-142"></a>

### `vplic_complete_locked`

```c
static void vplic_complete_locked(vplic_state_t *s, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验参数后清除该 context 对应中断的 active 位。

**位置**：`kernel/vmm/vdev/vplic.c:142`

<a id="fn-kernel-vmm-vdev-vplic.c-150"></a>

### `vplic_set_pending`

```c
void vplic_set_pending(vm_t *vm, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在锁保护下把指定源级中断置入全局 pending 位图。

<details>
<summary>函数内备注（1 条）</summary>

**第 163 行**

> 源级挂起：置一次, 所有 context 可见(可见性由各自的 enable 决定)

</details>

**位置**：`kernel/vmm/vdev/vplic.c:150`

<a id="fn-kernel-vmm-vdev-vplic.c-168"></a>

### `vplic_next_deliverable`

```c
uint32_t vplic_next_deliverable(vm_t *vm, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁调用 locked 版本查询可投递中断并返回其号。

**位置**：`kernel/vmm/vdev/vplic.c:168`

<a id="fn-kernel-vmm-vdev-vplic.c-182"></a>

### `vplic_claim`

```c
uint32_t vplic_claim(vm_t *vm, uint32_t vcpu_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁调用 claim_locked 认领中断并返回中断号。

**位置**：`kernel/vmm/vdev/vplic.c:182`

<a id="fn-kernel-vmm-vdev-vplic.c-196"></a>

### `vplic_complete`

```c
void vplic_complete(vm_t *vm, uint32_t vcpu_id, uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

加锁调用 complete_locked 标记指定中断完成。

**位置**：`kernel/vmm/vdev/vplic.c:196`

<a id="fn-kernel-vmm-vdev-vplic.c-209"></a>

### `vplic_read`

```c
static uint64_t vplic_read(mmio_device_t *dev, uint64_t off, uint8_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在锁下按偏移解析 priority/pending/enable/threshold 区，读 claim/complete 寄存器时执行认领并返回中断号。

<details>
<summary>函数内备注（6 条）</summary>

**第 212 行**

> priv 仍指向 st(语义不变), 锁在包装器里 —— 用 container_of 找回

**第 221 行**

> priority：每源 4 字节

**第 228 行**

> pending：每 32 源 1 字

**第 239 行**

> enable：按 context 分块

**第 247 行**

> context 区：threshold / claim-complete

**第 255 行（行尾）**

> 读 claim = 认领

</details>

**位置**：`kernel/vmm/vdev/vplic.c:209`

<a id="fn-kernel-vmm-vdev-vplic.c-265"></a>

### `vplic_write`

```c
static void vplic_write(mmio_device_t *dev, uint64_t off, uint8_t size, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在锁下按偏移写 priority、enable、threshold，写 claim/complete 时执行完成，pending 区忽略。

<details>
<summary>函数内备注（5 条）</summary>

**第 276 行**

> priority

**第 284 行**

> enable

**第 296 行**

> context：threshold / complete

**第 304 行（行尾）**

> 写 complete = 完成

**第 309 行**

> pending 区只读(guest 不能直接写挂起)

</details>

**位置**：`kernel/vmm/vdev/vplic.c:265`

<a id="fn-kernel-vmm-vdev-vplic.c-322"></a>

### `vplic_init`

```c
int vplic_init(vm_t *vm, mmio_bus_t *bus, uint32_t nr_vcpus)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取本 VM 槽位、按 vCPU 数（限上限）清状态并填 owner，设 ops/priv 后注册到 MMIO 总线。

<details>
<summary>函数内备注（2 条）</summary>

**第 326 行**

> 设备对象在池里, 不再由调用方传进来

**第 336 行（行尾）**

> 不碰锁, 见 vplic_destroy

</details>

**位置**：`kernel/vmm/vdev/vplic.c:322`

<a id="fn-kernel-vmm-vdev-vplic.c-353"></a>

### `vplic_destroy`

```c
void vplic_destroy(vm_t *vm)
```

vplic_destroy — 归还本 VM 的槽位(由 vmm_arch_vm_destroy → vm_free 调用)

三条硬要求同 vpl011_destroy：幂等, 对"从未 init 过的槽"安全,
永不 memset 锁对象。必须在 g_vm_pool_lock 外面调用。

<details>
<summary>函数内备注（1 条）</summary>

**第 363 行（行尾）**

> 含 st.owner —— of() 随即失效

</details>

**位置**：`kernel/vmm/vdev/vplic.c:353`

<a id="mod-kernel-vmm-vm.c"></a>

## kernel/vmm/vm.c — VM 对象：池, 生命周期, 创建

`kernel/vmm/vm.c` · 11 个函数 · 11 个有说明 · 299 行

从 vmm.c 拆出来的。这里只管「一个 guest VM 这个对象本身」：
- 静态 VM 池与槽位分配(vm_alloc / vm_get / vm_free / vm_count_used)
- 生命周期状态机(VM_FREE → LOADING → RUNNING → DYING → FREE)
- 停止请求与"有没有 VM 在跑"的查询
- vm_create：按架构转发到各后端的初始化

不在这里的：
- vCPU 内核任务(vcpu.c)—— 它只是执行这个 VM 的载体
- 主循环 vmm_run_vcpu(vcpu.c)
- 各架构的初始化实现(aarch64/vm_init.c, x86_64/vmx.c, riscv64/hext_run.c)

注意： 本文件不得依赖任何具体的中断控制器 —— 架构状态由 vm_t 里的守卫块承载,
控制器注册集中在各架构的 vm_init 里。(第二轮做中断抽象时的落点。)

<a id="fn-kernel-vmm-vm.c-44"></a>

### `vm_create`

```c
int vm_create(vm_t *vm)
```

vm_create — 建一个 VM

真正的实现在各架构的 vmm_arch_vm_init()(见 include/vmm/vmm.h 的架构钩子节)：
aarch64 → kernel/vmm/aarch64/vm_init.c
x86_64  → kernel/vmm/x86_64/vmx.c
riscv64 → kernel/vmm/riscv64/hext_run.c

这里只是一层转发 —— 编译期就解析成一条直接调用, 没有运行期分派。
从前这里是 `#if/#elif` + 两处就地 extern(其中 aarch64 那个函数还内联在
vmm.c 里), 三个架构三种放法；钩子化之后三边对称。

**位置**：`kernel/vmm/vm.c:44`

<a id="fn-kernel-vmm-vm.c-71"></a>

### `vm_alloc`

```c
vm_t *vm_alloc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在池锁下找首个 VM_FREE 槽，清零并置 LOADING、分配自增 vmid（255 后回绕 1），无空槽告警返回 NULL。

**位置**：`kernel/vmm/vm.c:71`

<a id="fn-kernel-vmm-vm.c-98"></a>

### `vm_get`

```c
vm_t *vm_get(uint32_t vmid)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在池锁下按 vmid 查找状态非 FREE 的槽并返回指针，未找到返回 NULL。

**位置**：`kernel/vmm/vm.c:98`

<a id="fn-kernel-vmm-vm.c-117"></a>

### `vm_state_set`

```c
void vm_state_set(vm_t *vm, int state)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若 VM 状态不是 FREE/DYING，则在池锁下更新其状态。

**位置**：`kernel/vmm/vm.c:117`

<a id="fn-kernel-vmm-vm.c-137"></a>

### `vm_vcpu_started`

```c
void vm_vcpu_started(vm_t *vm)
```

vm_vcpu_started — vcpu_task_create() 在 task_create 之前调用

必须先加计数再建任务：新任务可能立刻在另一颗核上跑起来并退出,
若先建任务再加计数, 那次 vm_vcpu_stopped() 会把计数减成负数, 并误判
「我可以销毁 VM」。

**位置**：`kernel/vmm/vm.c:137`

<a id="fn-kernel-vmm-vm.c-151"></a>

### `vm_vcpu_start_abort`

```c
void vm_vcpu_start_abort(vm_t *vm)
```

任务创建失败时回滚 vm_vcpu_started()。不销毁 VM —— 调用方还在用。

**位置**：`kernel/vmm/vm.c:151`

<a id="fn-kernel-vmm-vm.c-170"></a>

### `vm_vcpu_stopped`

```c
void vm_vcpu_stopped(vm_t *vm)
```

vm_vcpu_stopped — vCPU 任务退出时调用(由 vcpu_task_fn 收尾)

只有最后一个退出的 vCPU 才触发 vm_free()。多 vCPU 时第一个退出
不能拆 stage-2 —— 兄弟 vCPU 还在用。

**位置**：`kernel/vmm/vm.c:170`

<a id="fn-kernel-vmm-vm.c-199"></a>

### `vm_free`

```c
void vm_free(vm_t *vm)
```

vm_free — 拆一个已经没有任何存活 vCPU 的 VM

正常路径由 vm_vcpu_stopped() 在最后一颗 vCPU 退出时调用；错误路径
(vm_create/装载失败, 一个任务都没建)由调用方直接调用。

目前只回收 stage-2(连同它按需分配的那些物理页)。vGIC / vpl011 的状态
都是 vm_t 的一部分, 跟着槽位一起被 vm_alloc 的 memset 清掉。

<details>
<summary>函数内备注（5 条）</summary>

**第 206 行**

> state = DYING 同时充当「销毁进行中」的排他标记：
> - nr_vcpu_alive != 0：还有 vCPU 在跑, 绝不能拆；
> - 已经是 DYING/FREE：另一个调用者已经/正在拆, 重复调用直接返回。
> 拆完之前不把槽位置成 FREE —— 否则 vm_alloc 会立刻把它分给别人。

**第 221 行（行尾）**

> 释放按需页 + L3 表

**第 223 行（行尾）**

> 释放按需页 + L0 表

**第 225 行（行尾）**

> 释放按需页 + PT 表

**第 228 行**

> 归还设备槽位。

> 注意： 必须在 memset 之前, 且在池锁外面：
> - 之前：下面那句 memset 会把 vm-&gt;slot 清成 0, 之后就没法索引池了；
> - 外面：各设备的 destroy 靠自己的锁串行化(池锁里不该做设备操作)。
> 从前设备状态就嵌在 vm_t 里, 那句 memset 顺手就把它们清了 —— 搬到
> 静态池之后 memset 碰不到池子, 必须显式归还。

</details>

**位置**：`kernel/vmm/vm.c:199`

<a id="fn-kernel-vmm-vm.c-247"></a>

### `vm_count_used`

```c
int vm_count_used(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在池锁下统计状态非 VM_FREE 的槽位数并返回。

**位置**：`kernel/vmm/vm.c:247`

<a id="fn-kernel-vmm-vm.c-260"></a>

### `vm_request_stop`

```c
void vm_request_stop(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

置 stop_req，对每个 vCPU：OFFLINE 且有任务时完成 start 闸门，再 kick，使停在 guest 里的兄弟核尽快退出。

<details>
<summary>函数内备注（2 条）</summary>

**第 267 行**

> 把「等下一次自然 exit」缩短到「现在退出」。

> 单 vCPU 时这只是省一次等待；多 vCPU 时它是必需的：停在 guest 里的
> 兄弟 vCPU 可能正跑一个长时间不陷入的循环, 只置标志会让整个 VM
> 迟迟收不齐, 槽位也回收不了。kick 走 resched IPI, 目标在 guest 里
> 会被硬件转成一次 VM-exit, 主循环顶部看到 stop_req 后返回。

**第 277 行**

> 还没被 CPU_ON 放行的 AP 任务睡在 start completion 上, kick 对
> 它无效(state 还是 OFFLINE)。必须完成闸门把它叫醒, 让它在
> vmm_run_vcpu 顶部看到 stop_req 后退出；否则 VM 永远等不齐所有
> vCPU, 槽位也回收不了。

</details>

**位置**：`kernel/vmm/vm.c:260`

<a id="fn-kernel-vmm-vm.c-296"></a>

### `vmm_guest_running`

```c
int vmm_guest_running(void)
```

vmm_guest_running — 是否有任意 VM 正在跑

保留这个"全局"语义是因为 /dev/vmm 的若干判断(还能不能往 guest 写,
bootlinux 要不要 -EBUSY)在单 VM 视角下就是这么用的。多 VM 的精细判断
走 vm_get(vmid)->state。

**位置**：`kernel/vmm/vm.c:296`

<a id="mod-kernel-vmm-vmm_console.c"></a>

## kernel/vmm/vmm_console.c — 直启模式下的宿主控制台轮询泵

`kernel/vmm/vmm_console.c` · 1 个函数 · 1 个有说明 · 70 行

从 vmm.c 拆出来的。只有直启模式(GUEST_LINUX=1, 没有宿主 shell)需要它：
此时没有别的消费者, 由 VMM 自己在每次退出 guest 后把宿主 UART 的 RX 字节
推进 guest 控制台 vdev。helper 模式下宿主 tty 层独占真实 UART, 本函数会
在 tx_channel 归属检查处直接让位(见函数内的注释)。

原型在 include/vmm/vmm_console.h —— 那个头自己的文档就写明宿主侧只有两处
需要控制台接口：/dev/vmm 和本文件的泵。

<a id="fn-kernel-vmm-vmm_console.c-45"></a>

### `vmm_console_pump`

```c
void vmm_console_pump(vm_t *vm)
```

vmm_console_pump — 宿主控制台 → guest 虚拟 UART 的输入桥

轮询宿主真实 UART 的 RX FIFO, 把用户按键推进 guest 控制台 vdev 的
RX FIFO(对标 kvmm 的 RxChannel::push；注入中断由 VMM 在进入 guest
前做, 见各架构的 vmm_arch_restore_guest_ctx)。

为什么轮询而不是让宿主收 RX 中断：guest 运行期间宿主中断是关的,
自己的 RX 中断根本进不来。而退出路径的调用频率足够高 ——
- guest 空闲：每条 WFI 都陷入 hypervisor, 实测 ~3 万次/秒；
- guest 满载：宿主定时器每 10ms 也会把它踹回宿主一次。
对交互式控制台来说, 最坏 10ms 的输入延迟完全够用。

uart_rx_ready() 在 UART 未开中断时直接查硬件 FIFO 状态位, 非阻塞,
所以这里必须先用它把关 —— uart_getc() 在没有数据时是阻塞的。

注意： 只在控制台 TX 通道关闭时才能跑。helper 模式(/bin/vmm-run)
下宿主 tty 层独占真实 UART：用户按键由 helper 从自己的 stdin 读走,
再 write() 到 /dev/vmm。此时若本函数也在跑, 两个消费者会从同一个硬件
FIFO 抢字节(tty.c 的 signal_check_uart() 是第一个, 本函数是第二个),
谁先跑谁拿到, 输入会随机丢给错误的一方。

<details>
<summary>函数内备注（2 条）</summary>

**第 47 行**

> 归属在别的 VM 手上就让位 —— 从前这是一个全局的 tx_channel 开关,
> 多 VM 之后每个 VM 各有自己的 console_owned：只有&quot;没有前台 VM&quot;时才
> 需要 VMM 自己去泵主机控制台, 否则宿主 tty 层(helper)独占真实 UART。

**第 55 行**

> 优先非阻塞取字节；ready() 与 getc() 之间可能被另一个宿主消费者
> 插进来，阻塞版 getc() 会一直等。DW 的 getc_nb 未实现时保留旧回退。

</details>

**位置**：`kernel/vmm/vmm_console.c:45`

<a id="mod-kernel-vmm-x86_64-guest_boot.c"></a>

## kernel/vmm/x86_64/guest_boot.c — x86_64 guest Linux 引导(bzImage / 64 位入口)

`kernel/vmm/x86_64/guest_boot.c` · 14 个函数 · 14 个有说明 · 947 行

与 arm64/riscv 的差别很大：那两个架构是「把 Image 拷到内存 + 传 DTB」,
而 x86 要走 Linux 的引导协议(Documentation/arch/x86/boot.rst)：

1. 读 bzImage 的 setup header(0x1f1 起), 校验 "HdrS"
2. 把引导扇区+setup 装到 0x10000；保护模式内核装到 pref_address
(为 0 时用 0x100000)
3. 在 0x70000 构造 boot_params(zero page)：setup_header 原样拷进去,
填命令行/initrd/E820
4. 自建临时页表(PML4→PDPT→PD, 2 MiB 大页 identity 映射前 1 GiB)
5. 自建 GDT(空 + 64 位代码段 + 数据段)
6. vCPU 直接以长模式起步, RIP = 内核装载地址 + 0x200(64 位入口点),
RSI = boot_params 的物理地址

注意： 本架构独有：GPA ≠ HPA。
guest 物理地址必须从 0 开始(Linux 假定低端有常规内存, 内核在 1 MiB),
而宿主的物理 0 显然不能给它。所以 GPA 由 EPT 翻译到宿主物理页上。
所有往 guest 内存写数据的地方都要过 gpa_ptr(vm, ...) —— 直接拿 GPA
去 phys_to_virt 就写到宿主的低端物理内存上了, 宿主当场崩。

注：早期实现是「预占宿主的一段固定窗口 + 线性偏移」(GUEST_X86_HPA_BASE),
改成按需分页后那个偏移没了 —— 每个 GPA 各自映射到一个现分配的宿主页,
由 EPT 查表翻译。GUEST_X86_HPA_BASE 只剩历史意义。

引导协议为什么选 64 位入口(而不是走 16 位实模式 setup)：
HdrS + xloadflags 的 XLF_KERNEL_64 表明内核自带 64 位入口, 直接跳过去
可以完全绕开实模式, BIOS 调用, VGA 这些我们没实现的东西。

<a id="fn-kernel-vmm-x86_64-guest_boot.c-111"></a>

### `gpa_ptr`

```c
static void *gpa_ptr(vm_t *vm, uint64_t gpa)
```

── GPA → 宿主内核可直接写的指针 ──────────────────────────

注意： 从前这里是 `phys_to_virt(GUEST_X86_HPA_BASE + gpa)` —— 常量基址 +
线性偏移。改成按需分页之后那条不变量没了：guest 的每一页都是缺页时
各自从 PMM 现分配的, GPA 与 HPA 之间不再有固定偏移, 而且可能还没分配。
所以一律走 guest_loader_gpa_ptr()(宿主碰 guest 内存的唯一入口, 它按
EPT 查表翻译, 未映射时现分配一页)。

注意： 另有一条同源的约束：不要对这个返回值做跨页的指针算术。见
include/vmm/guest_loader.h 里 guest_loader_write_guest() 那一族的说明。

<details>
<summary>函数内备注（1 条）</summary>

**第 113 行（行尾）**

> alloc

</details>

**位置**：`kernel/vmm/x86_64/guest_boot.c:111`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-134"></a>

### `mp_w`

```c
static void mp_w(void *base, uint32_t off, uint64_t v, int n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 64 位值按小端逐字节写到 base+off，共 n 字节。

**位置**：`kernel/vmm/x86_64/guest_boot.c:134`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-141"></a>

### `mp_sum`

```c
static uint8_t mp_sum(const uint8_t *p, uint32_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对 n 字节累加并返回低 8 位校验和。

**位置**：`kernel/vmm/x86_64/guest_boot.c:141`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-155"></a>

### `build_mptable`

```c
static void build_mptable(vm_t *vm)
```

注意： MP 表 0x800 字节, 起点 0x9F800, 跨页(0x9F000 与 0xA0000 两页)。
按需分页下必须先在宿主缓冲里建好再逐页写下去 —— 直接拿 gpa_ptr() 的返回值
当基址线性写的话, 0x9F800 之后的部分会落到宿主物理内存里。
(缓冲区 0x900 是 0x800 向上取整到页 + 少量余量。)

<details>
<summary>函数内备注（13 条）</summary>

**第 159 行**

> 注意： 浮点指针结构在 表内 0x400 处(MP_FP_GPA 0x9FC00 减去
> MP_TABLE_GPA 0x9F800), 不是表后面。写成 +0x800 的话整个 0x800 缓冲区
> 里那一块是空的, 写下去之后 guest 在 0x9FC00 读到一个全 0 的 `_MP_`
> 结构 —— 于是 `smp_check_mpc()` 拿着空的 mpc 指针解引用, 内核报
> `BUG: kernel NULL pointer dereference` @ `smp_check_mpc+0x2`。

**第 167 行（行尾）**

> 条目紧跟在 44 字节头之后

**第 178 行**

> ── type-0 处理器条目：每颗 vCPU 一条 ──────────────────────

> 这是 Linux 发现 AP 的唯一来源(没有 ACPI MADT)。entry 的字段
> (Intel MP Spec 1.4, 20 字节)：
> [0]    type = 0
> [1]    local APIC ID —— 约定 == vcpu_id, 必须与 vLAPIC 的
> ID 寄存器、CPUID.1.EBX[31:24] 三处一致
> [2]    local APIC version(0x14 = 集成 APIC; &lt;0x10 会被当成
> 82489DX 外置 APIC, 走另一套启动路径)
> [3]    CPU flags: bit0 enabled, bit1 BSP(只能有一个)
> [4..7] CPUID signature(EAX of leaf 1)
> [8..11] feature flags(EDX of leaf 1)
> 0x800 的缓冲区在 12 颗 CPU 时用 44 + 12*20 + 24 = 308 字节,
> 离 +0x400 的浮点指针结构还远。

**第 196 行（行尾）**

> APIC ID

**第 197 行（行尾）**

> APIC version

**第 198 行（行尾）**

> enabled; BSP

**第 199 行（行尾）**

> CPUID signature

**第 200 行（行尾）**

> feature flags

**第 214 行**

> type2：IOAPIC @0xFEC00000(symmetric I/O 模式要求它存在)

**第 223 行**

> 不写 type-3(ISA 中断源)条目：写了内核会判定为 symmetric I/O
> 模式并依赖 IOAPIC 投递中断, 而本 VMM 没有 IOAPIC 实体 ⇒ 中断全无 ⇒
> `Attempted to kill the idle task`。只留 LAPIC + 总线, 内核走
> virtual-wire(LAPIC 直连), 我们的 vLAPIC 定时器才起作用。

**第 236 行（行尾）**

> 校验和：整表求和 = 0

**第 241 行（行尾）**

> 长度(16B 单位), spec

**第 244 行**

> 整体落盘。0x800 字节从 0x9F800 起正好到 0x9FFFF 为止 —— 不跨页
> (0x9F000 页内), 所以一次 write_guest 就够。浮点指针结构在缓冲区
> +0x400 处, 随这 0x800 一起落下, 不必单独写。

</details>

**位置**：`kernel/vmm/x86_64/guest_boot.c:155`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-264"></a>

### `bp_put8`

```c
static void bp_put8(uint8_t *bp, uint32_t off, uint8_t v)
```

── 小工具 ──

一律走 memcpy：boot_params 的字段在结构体里是自然对齐的, 但用
`*(uint16_t *)(bp+off) = v` 这种写法会让 GCC 报 -Wstringop-overflow
(它跟丢 gpa_ptr 的来源后认为目标对象大小是 0), 而且将来万一有
非对齐字段就是踩坑。memcpy 语义清楚, 编译期也不再告警。

**位置**：`kernel/vmm/x86_64/guest_boot.c:264`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-268"></a>

### `bp_put16`

```c
static void bp_put16(uint8_t *bp, uint32_t off, uint16_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 memcpy 把 16 位值写入 boot params 缓冲偏移处。

**位置**：`kernel/vmm/x86_64/guest_boot.c:268`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-272"></a>

### `bp_put32`

```c
static void bp_put32(uint8_t *bp, uint32_t off, uint32_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 memcpy 把 32 位值写入 boot params 缓冲偏移处。

**位置**：`kernel/vmm/x86_64/guest_boot.c:272`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-277"></a>

### `le16`

```c
static uint16_t le16(const uint8_t *p)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从字节缓冲按小端读 16 位值。

**位置**：`kernel/vmm/x86_64/guest_boot.c:277`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-281"></a>

### `le32`

```c
static uint32_t le32(const uint8_t *p)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按小端从 4 字节读 32 位值。

**位置**：`kernel/vmm/x86_64/guest_boot.c:281`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-285"></a>

### `le64`

```c
static uint64_t le64(const uint8_t *p)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

组合两个小端 32 位读值返回 64 位值。

**位置**：`kernel/vmm/x86_64/guest_boot.c:285`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-291"></a>

### `build_e820`

```c
static void build_e820(uint8_t *bp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 boot params 填 4 项 e820（低端 RAM、保留区、1MiB 起 guest RAM、LAPIC 保留），写条数并打印。

<details>
<summary>函数内备注（2 条）</summary>

**第 307 行**

> guest RAM 的可扩展区(1 MiB 起, 到 192 MiB 顶)

**第 312 行**

> LAPIC 窗口：标成保留, 免得内核把这个区间当普通内存用

</details>

**位置**：`kernel/vmm/x86_64/guest_boot.c:291`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-329"></a>

### `build_pgtbl`

```c
static void build_pgtbl(uint8_t *pg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 guest 物理内存建 3 级恒等页表（0..1GiB），并额外映射高半区 0xffffffff80000000，最后打印。

<details>
<summary>函数内备注（9 条）</summary>

**第 337 行**

> 注意： 页表项里放的是 guest 物理地址, 不是宿主物理地址。

> 一开始顺手写了 GUEST_X86_HPA_BASE + ..., 于是 guest 把 0x20091000
> 当成下一页表的 GPA 去走 —— EPT 里当然没有这个 GPA, 每一步都缺页,
> 而缺页处理又&quot;解码失败就跳过指令&quot;, RIP 每次 +3, 看起来像 guest 在
> 乱跑。判据就是 EPT violation 报出来的 gpa 落在宿主窗口里(0x2xxxxxxx)。

**第 345 行（行尾）**

> P|RW → PDPT

**第 346 行（行尾）**

> P|RW → PD

**第 348 行（行尾）**

> 512 × 2 MiB = 1 GiB

**第 349 行（行尾）**

> P|RW|PS

**第 351 行**

> ── 高半区映射(缺了它 kernel 会在早期陷入自取异常的死循环)──

> 只有 identity 映射是不够的：Linux 的内核虚拟地址在
> __START_KERNEL_map = 0xffffffff80000000 起, `__startup_64` 之后
> 的代码就开始按这些虚拟地址取指/访存。此时 guest 的 CR3 还是
> 我们这张临时表, 上面没有高半区 → guest 自己 #PF → 它的处理程序
> 又踩同样的坑 → 无限循环。

> 判据(实测)：直方图里除了宿主 tick 什么都没有(异常在 guest 内部
> 消化, 根本不到 VMM), RIP 停在 guest 的陷阱入口(SAVE_ALL 的
> push 序列)反复出现。

> 做法与 QEMU 的 boot page tables 一致：
> VA 0xffffffff80000000 + X  →  物理 X
> PML4 索引 = (0xffffffff80000000&gt;&gt;39)&amp;0x1FF = 511
> PDPT 索引 = (&gt;&gt;30)&amp;0x1FF = 510, PD 索引 = 0

**第 374 行（行尾）**

> P|RW

**第 375 行（行尾）**

> P|RW

**第 377 行（行尾）**

> 2 MiB 页, 1 GiB

</details>

**位置**：`kernel/vmm/x86_64/guest_boot.c:329`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-384"></a>

### `build_gdt`

```c
static void build_gdt(uint8_t *gdt)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 Linux boot_gdt 布局填空、保留、0x10 代码段、0x18 数据段四个描述符并打印。

<details>
<summary>函数内备注（5 条）</summary>

**第 388 行**

> 注意： 必须用 Linux 自己的 boot_gdt 布局：
> GDT[0]=0x00 空  GDT[1]=0x08 (32 位代码, 占位)
> GDT[2]=0x10 64 位代码   GDT[3]=0x18 数据
> 因为内核的 `startup_64` 会用硬编码选择子重载段寄存器：
> CS=0x10, DS/ES/SS=0x18(见 arch/x86/boot/compressed/head_64.S 的
> 常量与 boot_gdt)。原来把代码段放在 0x08, 数据段放在 0x10,
> 内核加载 CS=0x10 时拿到的是数据段(不可执行 → #GP),
> 加载 DS=0x18 时又超出 GDT limit(→ #GP)——早期就崩,
> 表现正是「进了 64 位入口之后不久就异常循环」。

**第 400 行**

> 32 位保护模式的描述符(走 Linux 传统引导协议, 见下面的入口说明)。
> 数值直接对标 tgoskits `virtualization/axvm/src/arch/x86_64/boot/linux_boot.rs`
> 里的引导 stub, 以及 QEMU 的 load_linux()。

**第 405 行（行尾）**

> 0x08: 保留(32 位协议不用)

**第 406 行（行尾）**

> 0x10: 32 位代码

**第 407 行（行尾）**

> 0x18: 32 位数据

</details>

**位置**：`kernel/vmm/x86_64/guest_boot.c:384`

<a id="fn-kernel-vmm-x86_64-guest_boot.c-418"></a>

### `x86_guest_boot`

```c
int x86_guest_boot(vm_t *vm)
```

x86_guest_boot — x86_64 的 Linux 引导入口(由 guest_loader_run_linux 调用)

返回 0 = vCPU 任务已创建(由它跑 guest)。

<details>
<summary>函数内备注（61 条）</summary>

**第 425 行**

> ── 1. guest RAM ──────────────────────────────────────────

> 注意： 这里从前有两件事, 现在都删了：
> pmm_mark_allocated(192 MiB)   —— 把整段窗口从 PMM 里划走
> memset(phys_to_virt(...), 0, 192 MiB)  —— 整片清零
> 那是 identity 映射时代的要求：guest 的 GPA 就是宿主的一段真实物理内存,
> 不预先占住就会被宿主的分配踩掉。改成按需分页之后这两条前提都消失了
> —— guest 的每一页都是缺页时各自从 PMM 现分配的(分配时就清零, 见
> x86_ept_map_block 的 zero 参数), 所以：
> - 两个 VM 的内存天然隔离(落到不同的物理页, 与 GPA 无固定关系)；
> - 也不再需要&quot;预留一整段连续物理内存&quot;这个前提。
> 另一条：x86 的 guest 物理地址必须从 0 开始(Linux 假定低端有常规内存,
> 内核装载在 1 MiB), 而宿主的物理 0 不能给它 —— 老实现靠 EPT 的线性偏移
> 解决, 现在靠&quot;每个 GPA 各自映射到一个现分配的宿主页&quot;, 约束自然满足。

**第 444 行**

> ── 建 VM(EPT / MMIO 总线 / vCPU 都在这里就绪)──

> 注意： 必须在加载任何镜像之前。改为按需分页之后 EPT 初始是空的, 所有
> 往 guest 内存的写入都要经 guest_loader_gpa_ptr() 现分配 + 装映射, 而它
> 依赖 vm-&gt;ept —— 顺序反了会拿到一片 NULL, 症状是 `cannot map guest
> gpa=...` 之后紧跟一个写零地址的 #PF(实测 CR2=0, 错误码 W=1)。
> aarch64/riscv 两侧的 guest_loader 是同一条约束。

> 配置(mem_base/mem_size/nr_vcpus)由调用方填好, 这里不再自建。

> 注：这里从前还有一段 TEMP-DBG 哨兵(往 0x4000 / 0x5000000 埋魔数,
> 验证「宿主没踩掉 guest RAM」)。它当年是为 identity 时代的那个坑加的；
> 按需分页之后每页都是现分配的, 分配时清零, 那个坑不存在了, 所以删掉。

**第 464 行**

> ── 2. 读 bzImage 头 + 按范围装载 ──

**第 466 行**

> ── 读 bzImage 头 ───────────────────────────────────────

> 只读文件开头这一小段：setup_sects 决定各段多长, pref_address 决定
> 内核装到哪 —— 都得先知道才能装。

> 注意： 这里不再&quot;把整个文件装到暂存区(0x10000)再搬&quot;。
> 老写法(identity 映射时代)是：
> load_file(SETUP_GPA)                       # 整个 11 MB 装到 0x10000
> memmove(gpa_ptr(dst), gpa_ptr(src) + off, pm_len)
> 第二句在按需分页下根本不成立：每个 guest 页各自从 PMM 分配,
> 物理上不连续, `ptr + off` 走到的是宿主物理内存的下一页, 而不是
> guest 的下一页。数据错位且没有任何报错 —— 实测症状是 payload 末尾
> 的 GDT(0xAA7B40)成了垃圾, guest `lgdt` 之后 `mov %ax,%ds` 直接
> #GP, 而 IDT 还没建好 → triple fault, 日志里只有
> `guest triple fault rip=0x1000022` 和一段看着像代码的 dump。

> 现在按引导协议本来就分开的两段直接装到各自的最终地址, 既不
> 需要暂存区, 也不需要那趟 11 MB 的搬运(顺带还快了一点)：
> [0, setup_bytes)            → GPA 0x10000(引导扇区 + setup)
> [setup_bytes, klen)         → s_kernel_load(保护模式内核)
> 两段区间不重叠, 天然没有 memmove 语义的需求。

**第 497 行**

> 头在 0x1f1..0x271, 4 KiB 一定放得下；放不下说明文件不对

**第 503 行（行尾）**

> &quot;HdrS&quot; 小端

**第 524 行**

> 整个文件有多大：用 seek 到末尾取偏移(读文件头的 `syssize`
> 是 16 位时代的老账, 不可靠)。

**第 540 行**

> ── 2b. setup 段 → 0x10000 ──

**第 547 行**

> ── 2c. 保护模式内核 → s_kernel_load ──

**第 565 行**

> ── 4. initrd ──

**第 566 行**

> 同上：纯 GPA, 不带宿主窗口偏移

**第 574 行**

> ── 5. 命令行 ──

**第 578 行**

> ── 6. boot_params ──

**第 583 行（行尾）**

> setup_header 原样拷入

**第 585 行（行尾）**

> 0xff = 未知 bootloader

**第 586 行**

> loadflags 必须带上 CAN_USE_HEAP —— 解压器(extract_kernel)就是
> 靠它决定去用 boot_params 里声明的那块堆, 并据此算出「解压输出
> 往哪写」。少了这一位, 它算出来的目标指针会是空/错值, 现象是
> guest 在 64 位入口之后不久就 write 到 cr2=0(err=0x2 写缺页)
> 并陷入异常风暴 —— 而同一镜像用 QEMU 自己的 load_linux() 裸跑
> 完全正常(QEMU 是置了这两位的)。
> heap_end_ptr 取 QEMU 同款 0x8000-0x200：堆在 setup 区之后。

**第 596 行**

> heap_end_ptr：原值保留(tgoskits 同款)。32 位入口的栈就是
> 由内核用它算出来的(esp = heap_end_ptr + STACK_SIZE - 4),
> 自己编一个值会把栈挪到别处。

**第 606 行**

> 照搬 tgoskits boot_params.rs 的字段清单：
> sentinel(0x1ef)=0xff  ← 内核据此判断「装载器确实填过零页」；
> 为 0 时内核会走 sanitize_boot_params()
> 把 screen_info/ramdisk 等字段清掉。
> ext_cmd_line_ptr(0xc8) / ext_ramdisk_*(0xc0,0xc4)：高位, 我们地址 &lt;4G 故为 0。

**第 617 行**

> initrd 必须落在 initrd_addr_max 之下 —— 这个字段内核自己会
> 用它做检查, 取头里的值(通常 0x37FFFFFF)即可, 不要自己编。

> 注意： 其余头字段(kernel_alignment / cmdline_size / xloadflags /
> relocatable_kernel)一律不要改写：它们是内核自报的能力,
> 覆盖成我们以为的值会让解压/重定位阶段走进死循环 —— 典型症状
> 是 guest 在解压后的内核区域里纯 CPU 空转, 一次设备访问都没有
> (直方图上只有宿主 tick)。上面 hdr 已经整块拷进 boot_params,
> 这里只补「装载器才知道」的那几个字段。

**第 636 行（行尾）**

> init_size

**第 637 行（行尾）**

> pref_address

**第 646 行**

> ── 7. 临时页表 + GDT + 空 IDT/TSS ──
> IDT/TSS 全 0 即可：IDT 全 0 = 任何异常都 triple fault(与真机同阶段
> 行为一致)；TSS 在 guest 自己 lidt/ltr 之前不会被真正使用, 但
> VM-entry 要求 TR base 落在 guest 物理空间内。

**第 651 行**

> 注意： 页表有 20 KiB, 跨 5 个 guest 页。按需分页下不能拿一个
> `gpa_ptr()` 的返回值当基址线性写 -- 那样第 2 页之后写的是宿主物理内存的
> 下一页, 而不是 guest 的下一页。所以：先在宿主缓冲里建好, 再
> guest_loader_write_guest() 逐页落下去(DTB 补丁也是同一个套路)。

**第 668 行**

> ── vCPU 入口状态(在 vm_create 之后：vm_init 会 memset vcpu)──

**第 672 行**

> ── 入口探针 stub ──

> 把入口指向一段自己塞的机器码：先往 COM1(0x3F8) 写一个字符,
> 再跳真正的内核 64 位入口。它把问题一刀切成两半：
> 看到 &#x27;A&#x27;   → 长模式/页表/EPT/COM1 PIO 全通, 问题在内核内部
> 看不到 &#x27;A&#x27; → 入口路径本身没跑起来(#PF 死在更早的地方)
> 注意 stub 不能碰 %rsi(64 位引导协议里它就是 boot_params)。

**第 681 行**

> 入口探针 stub：默认关闭(走真入口)。

> 打开方式：把下面的 #define 改成 1。它把入口指向一段自写机器码,
> 用「有没有 reason=30(PIO exit)」当信号灯, 可以在不改 VMM
> 逻辑的前提下验证任何寄存器/内存假设 —— 调试期靠它证明了
> 「入口/长模式/页表/EPT/COM1 通路都正常」「%rsi=0x70000 正确」
> 「0x70000/0x2000000/0x2500000 三个地址都可读」。

**第 691 行**

> 入口 RIP：无条件设置。

> 注意： 这里踩过一次：g_rip 原来只写在 `#if GUEST_X86_ENTRY_PROBE`
> 分支里, 探针一关, g_rip 就保持 0, guest 从地址 0 开始执行 →
> 第 1 次 VM-exit 就是 triple fault(reason=2, rip=0x10000),
> 串口一个字都没有, 看起来像&quot;整个 VMM 没起来&quot;。探针只是可选的
> 跳板, 入口本身与它无关。

**第 700 行**

> ── 入口：走 Linux 32 位引导协议, 不是 64 位直启 ──

> 对标 tgoskits(`linux_boot.rs`)：进 32 位保护模式, 跳到
> `code32_start`(即解压器的 `startup_32`, 它在本镜像里 rva=0),
> 唯一要传的参数是 `esi = boot_params`。**32→64 位切换, GDT,
> 临时页表, identity map, 5 级页表探测全部由内核自己的
> `startup_32`/`startup_64` 完成 —— 与 QEMU 裸跑走的完全同一条路。

> 为什么放弃 64 位直启：那条路要求装载器把环境&quot;拼&quot;对(我们逐字节
> 比对了 7 个地址, 压缩数据, 代码布局都一致, 解压器仍然在
> `inflate_fast` 里跑飞), 说明还有某个直启契约没满足。换成内核
> 自己的路径后, 环境一致性由构造保证。

> g_cr3 = 0 是本文件的约定：非零 = 64 位直启, 0 = 32 位协议
> (vmx.c 的 vmx_vcpu_setup 据此设 guest 状态, 这样不用改头文件)。

**第 717 行（行尾）**

> bzImage: startup_32 / ELF: e_entry

**第 718 行（行尾）**

> 分页关闭(32 位保护模式)

**第 719 行（行尾）**

> esi = boot_params

**第 720 行（行尾）**

> 32 位协议要求 ebx/edi/ebp=0

**第 729 行**

> 探针1：入口活着

**第 732 行（行尾）**

> mov $0x3f8,%dx

**第 736 行**

> 探针2..4：测三个关键地址的可读性(读安全, 失败即 triple
> fault, 可观测)。每读通一个就写一个字符, 靠 reason=30 的
> 次数就能判断卡在哪个地址 —— 把「内核里的指针为空」与
> 「页表/EPT 覆盖有洞」彻底分开。
> &#x27;2&#x27; = boot_params (0x70000)
> &#x27;3&#x27; = 装载地址附近 (0x2000000)
> &#x27;4&#x27; = init_size 尾部 (0x2500000)

**第 745 行**

> 探针2：把 boot_params 关键字段的低两字节按 ASCII hex
> 打到串口(每个字段前先打一个标签字符)。这样日志里能直接
> 读出数值, 不用再猜「非零/为零」。
> 标签：i=init_size  h=heap_end_ptr  l=loadflags  e=e820_entries

**第 754 行**

> 打印 %bl 的低两字节(先高字节后低字节, 各两位 hex)

**第 757 行（行尾）**

> 低半字节 → 先存起来? 简化：逐 nibble 直接打

**第 761 行**

> 标签

**第 765 行**

> 取字段到 %rbx

**第 778 行**

> 打高字节(bits 15:8)再打低字节(bits 7:0)

**第 780 行**

> %al = (value &gt;&gt; (half?0:8)) &amp; 0xff

**第 782 行（行尾）**

> mov %bl,%al

**第 787 行（行尾）**

> shr $8,%al

**第 788 行**

> 高 nibble

**第 790 行（行尾）**

> shr $1,%al

**第 798 行（行尾）**

> and $0xf,%al

**第 800 行（行尾）**

> cmp $10,%al

**第 808 行**

> 低 nibble

**第 835 行（行尾）**

> movabs $imm64,%rax

**第 840 行（行尾）**

> mov (%rax),%rbx

**第 859 行（行尾）**

> GUEST_X86_ENTRY_PROBE

**第 861 行**

> 64 位直启路径(保留)：自带临时页表, 栈在 190 MiB 处

**第 863 行（行尾）**

> GPA：EPT 会翻译

**第 865 行**

> 32 位协议：分页关闭, 栈只要在低端可用内存里即可。
> 内核的 startup_32 会立刻用 boot_params-&gt;hdr.scratch 重设 esp。

**第 871 行（行尾）**

> 4 项：空/0x08/0x10/0x18

**第 873 行**

> 64 位引导入口的两个参数(缺一不可)：
> %rsi = boot_params 物理地址
> %rdi = 解压输出缓冲区的地址

> 少了 %rdi 的后果极具误导性：解压器拿到 output=NULL, 于是它那段
> 逐字拷贝循环 `movzwl (%rbp,%rcx,2) → mov %r8w,(%r11,%rcx,2)`
> 一路往地址 0 写 —— 实测 cr2=0, err=0x2(写缺页), 随后陷进
> 异常循环, 串口一个字都没有。而 QEMU 自己的 32 位路径会把这个
> 缓冲区准备好, 所以同一镜像裸跑完全正常。
> 输出地址就是内核装载地址(pref_address)。

**第 885 行（行尾）**

> IF=0, 初始不开中断

**第 891 行**

> ── 9b. 建 VMCS ──

> 玩具 guest(VMM_TEST)是测试自己调 vmx_vcpu_setup 的, Linux 路径没有
> 人替它调 —— 而 x86 的 guest 状态全在 VMCS 里(RIP/CR3/段/GDT…),
> 不建的话 vmlaunch 时 VMCS 是空的, vmptrld 直接失败：
> [VMX] vmptrld failed ... / guest entry failed
> 症状看着像&quot;VMX 坏了&quot;, 其实只是没人建表。
> entry 参数在 g_boot_linux 路径下不用(RIP 取自 vcpu-&gt;g_rip), 传 0 即可。

**第 901 行**

> 注意： 显式声明：vmx.c 里定义了 vmx_vcpu_setup() 但没有在头文件里声明
> (头文件侧要它就得前向声明 vcpu_t, 而 x86 的 vcpu_t 是匿名 struct 的
> typedef, 没法前向)。不声明时 GCC 只给一条 -Wimplicit-function-
> declaration 警告 —— 返回 int 时侥幸没事, 返回指针时就是低 32 位
> 截断(guest_loader_gpa_ptr 刚在同一个文件里栽过, 见它的注释)。
> 玩具 guest(VMM_TEST)那边也是这么声明的。

**第 916 行**

> ── 10. 交给 vCPU 任务 ──
> AP 任务先建：它们启动后睡在 vcpu-&gt;start(completion)上, 等 guest
> 在 BSP 上写 ICR 发 SIPI 时由 vmm_vcpu_start() 放行。BSP 最后建并
> 立刻进 guest —— 这样 Linux 在 BSP 上开始跑的时候, SIPI 要唤醒的
> 任务已经存在(aarch64/riscv 的 guest_loader 是同一顺序)。

> 注意：AP 的 VMCS 不在这里建 —— 必须在承载它的宿主核上建(见
> vmm_arch_restore_guest_ctx() 的 vmcs_ready 段)。这里只把架构无关的
> 标志置上, 让第一次 entry 走 Linux 路径的初始化。

</details>

**位置**：`kernel/vmm/x86_64/guest_boot.c:418`

<a id="mod-kernel-vmm-x86_64-vmx.c"></a>

## kernel/vmm/x86_64/vmx.c — Intel VMX 初始化, VMCS 管理与架构钩子

`kernel/vmm/x86_64/vmx.c` · 45 个函数 · 45 个有说明 · 3172 行

对标 AArch64 的 el2_run.c + stage2.c；实现 vmm.h 中声明的四个架构钩子。

简化设计(单核 QEMU, 不使用 EPT)：
- Guest 与 Host 共享 CR3(同一页表, identity map)
- HLT → VMM 步进 RIP, yield, resume
- VMCALL(rdi=0, HVC_DONE)   → VMM 正常退出
- VMCALL(rdi=1, HVC_PRINT)  → VMM 打印迭代计数, yield, resume
- 其余 exit → 打印诊断信息, 返回 EL2_EXIT

QEMU 启动需要 -enable-kvm -cpu host(KVM 硬件 VMX)。

<a id="fn-kernel-vmm-x86_64-vmx.c-82"></a>

### `vcpu_slot`

```c
static inline uint32_t vcpu_slot(const vcpu_t *vcpu)
```

vcpu → 引擎状态数组的下标

下面这些数组(VMCS / guest 栈 / MSR 加载表 / 诊断环形缓冲)从前按
vcpu_id 索引。而 vcpu_id 是每个 VM 内部从 0 开始的编号, 于是两个
VM 的 vcpu0 指向同一块 VMCS：第二个 VM 一启动就把第一个 VM 的 VMCS 覆盖
掉, 症状是"guest 随机跑飞"这种最难反查的一类。

改成 (vm->slot, vcpu_id) 展平。vm 可能是 NULL(直接建 vcpu 的玩具路径),
那时退回只用 vcpu_id —— 与改动前的行为一致。

**位置**：`kernel/vmm/x86_64/vmx.c:82`

<a id="fn-kernel-vmm-x86_64-vmx.c-124"></a>

### `vmx_bitmaps_init`

```c
static void vmx_bitmaps_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 MSR bitmap 与两张 IO bitmap 全部置 0xFF，即全拦截。

<details>
<summary>函数内备注（1 条）</summary>

**第 126 行**

> 两张表都全拦截。

> 全拦而不是按需拦, 是为了让 guest 绝无可能写到真实硬件：未知的 MSR
> 读回 0, 写忽略(见 msr_emulate_*), 未知端口按「无设备」处理。
> 代价是每次 MSR/端口访问一次 VM exit —— 对 Linux 启动路径来说
> 只有几千次, 可以接受；等启动完再按需放行(白名单)留作后续优化。

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:124`

<a id="fn-kernel-vmm-x86_64-vmx.c-148"></a>

### `vmx_inject_pending`

```c
static void vmx_inject_pending(vcpu_t *vcpu)
```

事件注入：把被 VMM 拦下的中断/异常送回 guest

VM-entry interruption-information(0x4016)是唯一通道。guest 当时
屏蔽着中断(RFLAGS.IF=0)就不能硬投 —— 开了 CPU_INTR_WINDOW 让硬件在
它开中断的瞬间(exit 7)再回来, 那时才投。

<details>
<summary>函数内备注（4 条）</summary>

**第 153 行（行尾）**

> 事件已消散, 撤掉窗口

**第 160 行**

> 外部中断(含 LAPIC timer)要等 guest 自己允许中断

**第 165 行**

> 注意： 两个条件缺一不可, 只看 IF 会被硬件打回(reason 33)：
> RFLAGS.IF = 1
> interruptibility state 的阻塞位全清
> bit0 STI 影子 / bit1 MOV SS 影子 / bit3 SMI
> (bit2 是「NMI 阻塞」, 只挡 NMI, 这里不能一起判。)

> 实测踩过：guest 的 default_idle 执行 `sti; hlt`, 而 hlt 恰好
> 落在 STI 影子里 —— VM-exit 时硬件把 bit0=1 一并存进 VMCS
> (exit 时 actv=0 intr_state=0x1)。我们只判 IF=1 就注入,
> 于是每一次 VM-entry 都被拒, guest 永远停在 idle。
> 正确做法和 IF=0 时一样：挂起 + 开 interrupt-window,
> 等影子被下一条指令清掉, 硬件再用 exit 7 把我们叫回来。

**第 188 行**

> 记进 vLAPIC 的 ISR：guest 处理完写 EOI 时要能退掉它

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:148`

<a id="fn-kernel-vmm-x86_64-vmx.c-276"></a>

### `vmx_msr_index`

```c
static int vmx_msr_index(uint32_t msr)
```

MSR 在 g_msr_list 里的下标(不在表里返回 -1)

**位置**：`kernel/vmm/x86_64/vmx.c:276`

<a id="fn-kernel-vmm-x86_64-vmx.c-286"></a>

### `vmx_msr_sync_guest`

```c
static void vmx_msr_sync_guest(vcpu_t *vcpu, uint32_t idx, uint64_t val)
```

guest 写了某个被拦截的 MSR 之后, 同步进 VM-entry 的装填区

**位置**：`kernel/vmm/x86_64/vmx.c:286`

<a id="fn-kernel-vmm-x86_64-vmx.c-294"></a>

### `vmx_msr_lists_init`

```c
static void vmx_msr_lists_init(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 vCPU 槽填 guest/host 的 MSR 装载列表与 GS 两个 MSR 的 exit-store 列表，并写 VMCS 计数与物理地址。

<details>
<summary>函数内备注（1 条）</summary>

**第 314 行**

> exit-store 区：独立缓冲, 只放需要往返回来的 GS 两兄弟。
> 注意： 不能与 entry-load 区(g_msr_guest)复用同一块内存 —— 重叠时
> VM-entry 直接以 reason 34(MSR loading)失败(实测
> &quot;Unhandled exit reason=34 rip=0x1000000&quot;)。

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:294`

<a id="fn-kernel-vmm-x86_64-vmx.c-331"></a>

### `msr_emulate_read`

```c
static int msr_emulate_read(vcpu_t *vcpu, uint32_t msr, uint64_t *val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对 EFER/STAR/LSTAR/CSTAR/SFMASK/FS/GS/PAT/APIC_BASE 返回影子值（GS 取硬件 exit-store），未知回 0。

<details>
<summary>函数内备注（2 条）</summary>

**第 352 行**

> 注意： GS 这一对要读硬件维护的那份(exit-store 写回的 g_msr_guest),
> 不能读 vcpu 里的影子：影子只在显式 wrmsr 时更新, 而 guest 的 `swapgs`
> 直接在硬件上交换这两个 MSR, 我们看不见。Linux 的 paranoid_entry 靠
> `rdmsr MSR_GS_BASE` 判断自己在哪个 GS, 给它过期影子就会判错 → 死循环。

**第 371 行（行尾）**

> 未知 MSR：当作「没有这个特性」

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:331`

<a id="fn-kernel-vmm-x86_64-vmx.c-376"></a>

### `msr_emulate_write_1`

```c
static int msr_emulate_write_1(vcpu_t *vcpu, uint32_t msr, uint64_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存 EFER/STAR/LSTAR/CSTAR/SFMASK/PAT/APIC base 等影子，把 EFER 与 FS/GS base 同步进 VMCS，APIC base 时清 x2APIC 并通知 vLAPIC。

<details>
<summary>函数内备注（6 条）</summary>

**第 380 行**

> Linux 会设 SCE(0)/NXE(11)/LME(8), 也用它切 LMA —— LMA 只读,
> 由硬件在 VM entry 时按 EFER.LME 决定, 这里只存 LME/NXE/SCE。

**第 399 行**

> 注意： FS/GS base 不只是「影子」——它们同时是 VMCS 字段
> (GUEST_BASE_FS/GS), 硬件在 VM-entry 时从这里装进寄存器。
> 只存影子不写 VMCS 的话, guest 写的 base 永远不会生效。

> FS 这条曾经漏掉(GS 修了, FS 没修, 见下面那段注释),
> 症状极具迷惑性：内核整个启动过程都正常, 一直到
> Run /init as init process
> init[1]: segfault at 0 ip ... error 4
> Kernel panic - Attempted to kill init!
> 才崩。因为 glibc/busybox 取 TLS 就一条
> mov %fs:0x0,%rax
> FS base 是 0 ⇒ 访问地址 0 ⇒ 用户态缺页。
> 内核态不用 FS, 所以前面 5 秒谁都没露馅。

**第 418 行**

> 同上：Linux 把 per-CPU 基址写进 GS_BASE, 之后所有 %gs 相对访问
> 都要求 VMCS 里的 GUEST_BASE_GS 同步 —— 漏写的实测症状是早期
> 启动里 cr2=0 的缺页 + 异常风暴(4000 次 exit 里 3990 次异常)。

**第 424 行（行尾）**

> 与 exit-store 同一份

**第 434 行**

> vLAPIC 的开关：EN(11) 决定 APIC 是否工作, EXTD(10) 是 x2APIC。
> 第一版只做 xAPIC, 所以 EXTD 一律清掉 —— Linux 探测到
> 「x2APIC 不可用」就会安心走 MMIO 路径。

**第 442 行（行尾）**

> 未知 MSR：写忽略

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:376`

<a id="fn-kernel-vmm-x86_64-vmx.c-455"></a>

### `vmx_cpuid_emulate`

```c
static void vmx_cpuid_emulate(const vcpu_t *vcpu, uint32_t leaf, uint32_t sub, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
```

CPUID 模拟

不模拟的后果：guest 看到的是宿主的 CPU —— 包括 AVX/AVX512(我们没有
保存 XSAVE 状态, 宿主也没开 FPU 上下文切换), VMX 位(guest 会试图嵌套),
以及宿主的核数拓扑。这里报一个保守但自洽的 CPU：SSE2 级别, 单核,
没有 XSAVE/AVX/VMX, 但有 APIC/TSC/PAE/NX/SYSCALL。

<details>
<summary>函数内备注（23 条）</summary>

**第 461 行**

> 本 vCPU 的 APIC ID（约定 == vcpu_id）。多 vCPU 下每颗核执行
> CPUID.1 都必须报自己的 ID：Linux 用它交叉校验 MP 表/APIC_ID
> 寄存器, 全报 0 会让 topology 与 IPI 目标全错。

**第 469 行（行尾）**

> &quot;Genu&quot;

**第 470 行（行尾）**

> &quot;ineI&quot;

**第 471 行（行尾）**

> &quot;ntel&quot;

**第 472 行**

> 最高基础叶 = 0x16。
> 注意： 这个值必须 ≥ 0x15, 否则 native_calibrate_tsc() 直接
> `if (boot_cpu_data.cpuid_level &lt; 0x15) return 0;` 出局,
> 内核掉进 quick_pit_calibrate() —— 那个循环 latch + 轮询
> PIT 通道 2 的高字节, 而我们的 8254 桩永远返回写入值,
> 于是跑满 50000 次 × 5 个端口 exit 才失败(看着像死机)。
> 仍然不暴露 7/0xB/0xD(那三个会给内核错误的 cache/topology 视图)。

**第 485 行（行尾）**

> family 6 / model 6 / stepping 2

**第 486 行**

> EDX：FPU PSE TSC MSR PAE CX8 APIC SEP PGE CMOV PAT CLFSH
> MMX FXSR SSE SSE2  —— 只放行这些 (值与 MP 表共用)

**第 490 行**

> ECX 全 0：特别是 VMX(5), XSAVE(26), OSXSAVE(27), AVX(28)

**第 492 行**

> 注意： EBX 也必须显式写！它的 [31:24] 是初始 APIC ID。
> 漏写会让内核读到未初始化的垃圾(实测读成 129/0x81),
> 于是 smpboot 报 &quot;Boot CPU (id 129) not listed by BIOS&quot;,
> CPU/中断拓扑错乱 ⇒ 后期 &quot;Attempted to kill the idle task!&quot; panic。
> [15:8] = CLFLUSH 行大小(8), [7:0] = brand index(0)。

> 多 vCPU：APIC ID 必须逐 vCPU —— 本 VMM 的约定是 APIC ID ==
> vcpu_id(MP 表、vLAPIC ID 寄存器、这里三处一致)。

**第 505 行**

> ── 0x15 / 0x16：TSC 频率 ──

> guest 跑在宿主的 TSC 上(嵌套 VMX, 我们没写 TSC_OFFSET),
> 所以「宿主的 TSC 频率」就是 guest 的正确频率。

> 先试透传宿主的真值：裸机跑 avatar 时那是精确值(本机物理
> CPUID 0x15 给 eax=2 ebx=126 ecx=38400000 ⇒ 38400*126/2 =
> 2419200 kHz)。但实测 QEMU/KVM 会把 0x15/0x16 抹成全 0,
> 而 avatar 平时就是跑在 QEMU 里的 —— 所以这条路基本走不通,
> 必须能自己合成。

> 合成方案：把晶体正好报成 1 GHz, 比例取 tsc/1e6。
> crystal_khz = ecx/1000            = 1000000
> tsc_khz     = crystal_khz * ebx/eax
> = 1000000 * (tsc_khz/1e6) / 1000 = tsc_khz
> (分子 1000000*2417 = 2.417e9 仍在 32 位无符号内, 不会溢出。)

> 注意： 为什么晶体一定要 1 GHz —— 这不是随便挑的：
> Linux 由它反推 LAPIC timer 频率：
> lapic_timer_period = crystal_khz * 1000 / HZ
> (它假定 LAPIC timer 就跑在晶体上), 而我们的 vLAPIC 模型是
> 「1 count = 1 ns」= 1 GHz(见 vlapic.c 的 timer_start)。
> 之前报 2.417 GHz, 两边差 2.4 倍, guest 每个 tick 都被拉长,
> 于是 clocksource watchdog 报 skew 过大, 把 tsc-early 判成 unstable。

**第 536 行（行尾）**

> 宿主不报(QEMU/KVM 全是 0)

**第 539 行（行尾）**

> 自己也没标定出来：保持全 0

**第 540 行（行尾）**

> 分母：比例 = mhz/1000

**第 541 行（行尾）**

> 分子

**第 542 行（行尾）**

> 晶体 = 1 GHz(对齐 vLAPIC 模型)

**第 559 行（行尾）**

> 基准频率 MHz

**第 570 行（行尾）**

> 最高扩展叶

**第 574 行**

> 注意： CPUID.80000001H 的位分布很容易记错：
> EAX = 扩展 family/model(这里给 0)
> EDX = SYSCALL/SYSRET(11) | NX(20) | LM(29)
> LM(长模式)在 EDX bit29, 不是 EAX —— 曾经写成
> `*eax = (1u&lt;&lt;29)`, 结果 guest 看到的 CPU「不支持长模式」,
> 内核 `startup_32` 里的 `verify_cpu` 检查不过, 直接跳进
> `hlt` 死循环(实测 235 万次 exit reason=12, RIP 原地不动,
> 串口一个字都没有), 非常难从现象反推。

**第 585 行（行尾）**

> SYSCALL/SYSRET

**第 586 行（行尾）**

> NX

**第 587 行（行尾）**

> PDPE1GB：1 GiB 大页。必须给 —— 缺了它
> 内核会走另一条直接映射路径(实测：裸跑打印
> &quot;Using GB pages for direct mapping&quot;,
> 我们这里没有), 随后在自己声明为 usable 的
> 物理地址上缺页(CR2=ffff888003200000)。

**第 593 行（行尾）**

> LM 长模式

**第 597 行（行尾）**

> 其余叶一律全 0

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:455`

<a id="fn-kernel-vmm-x86_64-vmx.c-607"></a>

### `msr_emulate_write`

```c
static int msr_emulate_write(vcpu_t *vcpu, uint32_t msr, uint64_t val)
```

写路径的统一入口：先走模拟, 成功的话把新值同步进 VM-entry 的 MSR
装填区 —— guest 会把 LSTAR/STAR/SFMASK 改成自己的 syscall 入口,
下次进 guest 时硬件要装的是新值；不同步的话 guest 的 syscall
会一直跳回宿主入口(MSR load list 存在的意义就在这里)。

**位置**：`kernel/vmm/x86_64/vmx.c:607`

<a id="fn-kernel-vmm-x86_64-vmx.c-641"></a>

### `gpa_to_host`

```c
static void *gpa_to_host(const vcpu_t *vcpu, uint64_t gpa)
```

guest 物理地址 → 本核可以直接读的宿主指针(查表, 不是算术换算)

注意： 按需分页之后 GPA 与 HPA 之间不再有固定偏移 —— 每一页都是各自从 PMM
分配的。老版本那种 `phys_to_virt(x86_guest_hpa_base() + gpa)` 会读到完全
无关的物理页(可能属于别的 VM 或宿主内核), 而且没有任何报错。

返回 NULL 表示这一页还没被 guest 碰过(按需分页下未映射 = 从没访问)。
调用方必须判 NULL —— 这几处都是诊断代码, 跳过即可, 不要当成功继续用。

**位置**：`kernel/vmm/x86_64/vmx.c:641`

<a id="fn-kernel-vmm-x86_64-vmx.c-651"></a>

### `guest_mem_size`

```c
static uint64_t guest_mem_size(const vcpu_t *vcpu)
```

本 VM 的 guest RAM 窗口大小(诊断里当越界判据用)。

**位置**：`kernel/vmm/x86_64/vmx.c:651`

<a id="fn-kernel-vmm-x86_64-vmx.c-657"></a>

### `guest_fetch_bytes`

```c
static int guest_fetch_bytes(const vcpu_t *vcpu, uint64_t gpa, uint8_t *buf, uint32_t n)
```

从 guest 内存取字节(经 EPT 翻译 GPA→HPA→内核直接映射)

**位置**：`kernel/vmm/x86_64/vmx.c:657`

<a id="fn-kernel-vmm-x86_64-vmx.c-669"></a>

### `guest_read_u64`

```c
static int guest_read_u64(const vcpu_t *vcpu, uint64_t gpa, uint64_t *out)
```

读 guest 的一个 64 位页表项(GPA → HPA → 直接映射)

**位置**：`kernel/vmm/x86_64/vmx.c:669`

<a id="fn-kernel-vmm-x86_64-vmx.c-699"></a>

### `guest_va_to_gpa`

```c
static int guest_va_to_gpa(const vcpu_t *vcpu, uint64_t va, uint64_t *gpa)
```

guest 虚拟地址 → guest 物理地址。

注意： 这一步必须有。guest 进内核态后 RIP 是内核虚拟地址
(实测 0xffffffff810608c6), 拿它直接当 GPA 去查 EPT 必然失败,
于是「取指令」失败 → 解码失败 → MMIO 读不回写目标寄存器,
guest 拿到的只是上一条指令的遗留值。症状极具迷惑性, 实测：
native_apic_mem_read(APIC_VERSION) 读出 0xc0
native_apic_mem_read(APIC_ID)       读出 129
于是内核打印 "BIOS bug: APIC version mismatch, boot CPU: c0",
把 CPU 拓扑判错成 flat/1 CPU —— 看着像 vLAPIC 坏了, 其实是解码器
根本没读到指令。解码器原先只在解压器阶段用过, 那时 guest 恒等映射
(VA==PA), 所以这个坑一直没暴露。

覆盖两档：
- CR0.PG=0(解压器早期 / 实模式尾巴)：恒等
- 4 级页表(EFER.LMA + CR4.PAE)：PML4→PDPT→PD→PT 真走一遍
其余(32 位 PAE / 2 级)按恒等兜底 —— guest 跑到 MMIO 密集期时
早就是 4 级页表了。

<details>
<summary>函数内备注（5 条）</summary>

**第 708 行（行尾）**

> 未开分页 / 非 4 级：恒等兜底

**第 719 行（行尾）**

> P=0：这一级没映射

**第 720 行（行尾）**

> PDPT 项指向 1 GiB 大页

**第 724 行（行尾）**

> PD 项指向 2 MiB 大页

**第 728 行**

> 注意：物理地址只取 bit 12-51。NX(bit 63)、软件位、PAT 等都在
> 高位/低位里 —— 原来写 `e &amp; ~0xFFF` 会把 NX 一起带进 GPA,
> 得到形如 0x800000000_0359f000 的&quot;物理地址&quot;, 用它查 EPT 必然失败
> (最常命中的是栈/数据页, 它们的 PTE 带 NX)。

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:699`

<a id="fn-kernel-vmm-x86_64-vmx.c-745"></a>

### `guest_fetch_code`

```c
static int guest_fetch_code(const vcpu_t *vcpu, uint64_t va, uint8_t *buf, uint32_t n)
```

取指令字节：rip 是虚拟地址, 先翻译成 GPA 再读。
x86 保证指令不跨页, 所以读满本页剩余部分就够；页尾之后补 0,
免得解码器读到未初始化的栈。

**位置**：`kernel/vmm/x86_64/vmx.c:745`

<a id="fn-kernel-vmm-x86_64-vmx.c-786"></a>

### `parse_modrm`

```c
static int parse_modrm(const uint8_t *p, uint32_t n, uint32_t *reg_field, uint32_t *bytes_consumed)
```

解析 ModRM 及其后续(SIB / disp), 返回操作数是否「使用寄存器直接寻址」。
MMIO 场景下操作数是内存操作数(mod != 3), 我们只关心它用了哪个寄存器
作为数据源/目标(reg 字段), 以及指令总长度。

<details>
<summary>函数内备注（6 条）</summary>

**第 801 行**

> 内存操作数：SIB + 位移

**第 802 行（行尾）**

> SIB

**第 808 行（行尾）**

> disp32

**第 810 行（行尾）**

> disp8

**第 812 行（行尾）**

> disp32

**第 814 行（行尾）**

> RIP 相对 disp32

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:786`

<a id="fn-kernel-vmm-x86_64-vmx.c-828"></a>

### `decode_mmio_access`

```c
static int decode_mmio_access(const vcpu_t *vcpu, uint64_t rip_va, x86_mmio_access_t *acc)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取 guest 指令字节，跳过前缀并解析 REX/操作数宽度，解码 MOV 读/写内存与立即数写内存三组指令，填方向、宽度、寄存器与长度。

<details>
<summary>函数内备注（15 条）</summary>

**第 837 行**

> ── 前缀扫描 ──

**第 843 行（行尾）**

> 操作数宽度

**第 846 行（行尾）**

> 地址宽度(忽略)

**第 848 行（行尾）**

> lock/rep

**第 850 行（行尾）**

> 段前缀

**第 851 行（行尾）**

> REX

**第 853 行（行尾）**

> 扩展 reg 字段：r8-r15

**第 856 行（行尾）**

> opcode

**第 868 行（行尾）**

> MOV r/m, r  → 写

**第 873 行**

> 源寄存器在 reg 字段, 带 REX.R(r8-r15)；0 是 RAX, 不是空值

**第 880 行（行尾）**

> MOV r, r/m  → 读

**第 885 行**

> 读方向：目标寄存器在 reg 字段, 同样要带 REX.R
> (原来错写成 rex_w —— 读进 %r8..%r15 的数据会落到 %rax..%rdi)

**第 894 行（行尾）**

> MOV r/m, imm → 写

**第 907 行（行尾）**

> 立即数写：无源寄存器

**第 913 行（行尾）**

> 不支持的 opcode

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:828`

<a id="fn-kernel-vmm-x86_64-vmx.c-922"></a>

### `decode_matches_qual`

```c
static int decode_matches_qual(uint64_t qual, const x86_mmio_access_t *acc)
```

校验解码出的方向/宽度与 EPT qualification 是否自洽。
qualification 是硬件给出的权威信息；解码结果不一致说明我们认错了指令,
此时宁可不解码也不要按错误宽度去访问设备。

**位置**：`kernel/vmm/x86_64/vmx.c:922`

<a id="fn-kernel-vmm-x86_64-vmx.c-932"></a>

### `x86_gpr_ptr`

```c
static uint64_t *x86_gpr_ptr(vcpu_t *vcpu, uint32_t reg)
```

把解码结果里的寄存器号映射到 vcpu->regs 的对应字段

<details>
<summary>函数内备注（1 条）</summary>

**第 944 行（行尾）**

> RSP 未保存在 x86_guest_regs_t 中(VMCS 自动
> 保存/恢复), MMIO 亦不从 RSP 取数据；返回 NULL
> 使调用方跳过读写回。

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:932`

<a id="fn-kernel-vmm-x86_64-vmx.c-989"></a>

### `vmx_check_support`

```c
static int vmx_check_support(void)
```

── VMXON / VMCS 操作 ──────────────────────────────────────────────────
vmxon / vmclear / vmptrld 是 include/arch/x86_64/vmx.h 里的 static inline。
它们的 m64 操作数是存放「物理地址」的内存位置；内核运行在
0xffff800000000000 偏移的高半区, 调用方必须先 virt_to_phys。

**位置**：`kernel/vmm/x86_64/vmx.c:989`

<a id="fn-kernel-vmm-x86_64-vmx.c-1035"></a>

### `vmx_global_init`

```c
static int vmx_global_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按本核固定位修正 CR0/CR4 并置 VMXE，使能 feature control，初始化本核 VMXON 区域并执行 VMXON，已开启则跳过。

<details>
<summary>函数内备注（5 条）</summary>

**第 1048 行**

> 满足 CR0/CR4 VMX 固定位要求

**第 1052 行**

> 本 CPU 已经开过就不用再来一次(见 s_vmx_on 的说明)；CR0/CR4 固定位
> 上面每次都会重新写一遍, 保持 VMX operation 的前置条件成立。

**第 1058 行**

> IA32_FEATURE_CONTROL 也是每个逻辑处理器一份的(KVM 里就是每个 vCPU
> 一份)。原来只有 vmx_check_support() 在 vmm_arch_vm_init() 里使能过一次,
> 那是在 helper 的核上 —— 于是只有那颗核被使能：
> SMP=1：helper 与 vCPU 同核, 看不出来；
> SMP=2：vCPU 所在核的 vmxon 被 KVM 判为「未使能」, 直接 #GP
> (handle_vmon 里 `msr_ia32_feature_control &amp;
> VMXON_NEEDED_FEATURES` 那一条)。
> 这也解释了「有概率」：helper 落在哪颗核, 决定哪颗核被使能。
> vmx_check_support() 幂等(已使能就直接返回), 这里补在真正 VMXON 的核上。

**第 1072 行**

> 写本核自己那块 VMXON 区域的版本号(区域是 per-CPU 的, 见其声明)

**第 1077 行**

> vmxon 需要物理地址

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1035`

<a id="fn-kernel-vmm-x86_64-vmx.c-1089"></a>

### `vmcs_init_ctrl`

```c
static void vmcs_init_ctrl(int want_ept)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读 VMX 能力 MSR，构造 pin/cpu/exit/entry 控制位与异常位图、IO/MSR bitmap 地址，按是否请求 EPT 协商并写入 VMCS。

<details>
<summary>函数内备注（8 条）</summary>

**第 1106 行**

> 期望控制位

> PIN_EXTINT：不置的话, 宿主时钟中断会在 non-root 里按 guest 的 IDT
> 投递 —— 宿主的 ISR 永远跑不到, 调度器饿死。置上之后外部中断一律
> 先退出到 VMM, 由 VMM 让出 CPU(见 exit reason 1), guest 自己的
> 中断则靠 vLAPIC + VM-entry 注入送回。

> CPU_IO_BITMAPS / CPU_USE_MSR_BITMAPS：见文件上方 bitmap 说明。

**第 1120 行**

> Secondary controls：启用 EPT(移植自 kvmm arch/x86_64/mod.rs)。
> 前提是 primary controls 允许位 31(CPU_SECONDARY)且 EPT 被允许；
> 否则回落到「共享 CR3, 无 EPT」的原路径(guest 仍可运行)。

> CPU2_UNRESTRICTED_GUEST：放宽 non-root 下的段/特权检查。Linux 启动
> 时会碰 CR0/CR4/段寄存器, 不放开就会收到一堆 #GP(而 #GP 又要我们
> 转发), 放开之后绝大多数交给硬件直接处理。它要求 EPT 已启用。

> 注意： CPU_EPT 必须跟着 want_ept, 不能只看能力位：
> vmm_arch_vm_init 只在 vm-&gt;cfg.mem_size != 0 时才写 EPT_POINTER。
> 若这里按能力位无条件开 EPT, 而 EPTP 还是 0(玩具测试就是这条路),
> 硬件会让 guest 的每次取指都报 EPT violation(exit 48),
> 而 exit 48 的处理是「解码不成就跳过指令」—— 结果 guest 在乱跑,
> 日志上看却只是在刷 EPT violation, 很容易误判成设备没实现。

**第 1141 行**

> 与 capability MSR 协商(allowed-1 &amp; allowed-0)

**第 1146 行**

> allowed-1/allowed-0 协商；若 CPU_SECONDARY 最终未置位则清除 ctrl1

**第 1158 行**

> 异常位图。默认 0(guest 自己的 IDT 处理一切)；Linux 引导路径额外
> 打开 bit14(#PF)—— 只为诊断：VM-exit 不保存/恢复 CR2, 所以
> 退出到 VMM 后 `mov %cr2` 读到的就是 guest 的缺页线性地址。
> 拿到地址后要把 #PF 原样注入回 guest(见 exit handler)。

**第 1164 行**

> 异常位图保持 0：guest 的异常由它自己的 IDT 处理(真机语义)。

> 调试期曾临时置成全 1 或 bit14(#PF), 用来把 guest 的缺页拦到 VMM 里
> 看 CR2 —— 那套诊断(EXC-DBG/EXC-STACK/PF-DBG)仍留在下面 exit
> handler 里, 按需打开即可, 但默认必须是 0：每个异常多一次世界
> 切换既改变时序, 也会让 guest 的异常处理走样。

**第 1172 行**

> 注意： 千万不要为了让 VMM 看到 guest 的 #PF 而在这里加 bit14！

> VM-exit 不保存/恢复 CR2, 而 VM-exit 之后宿主侧会跑一大堆代码
> (klog, 调度…), 宿主的缺页会把 CR2 冲掉。再注入回 guest 时,
> guest 自己的 do_boot_page_fault() 读到的 cr2 就是错的, 于是它
> 去身份映射了错误的 2MiB 区域 —— 真正出错的页永远修不好, guest
> 陷进无限 #PF(实测 60 秒 172 万次, RIP 原地不动, 无 HLT, 无输出)。

> 这条规则对所有异常都成立：异常位图必须为 0, 让 guest 用自己的
> IDT 处理, CR2 才是对的。要诊断就用宿主侧被动采样
> (见下面的 [RIP-SAMPLE] 退出采样), 不要抢异常。

**第 1189 行**

> bitmap 的物理地址(协商后确实启用才写)

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1089`

<a id="fn-kernel-vmm-x86_64-vmx.c-1203"></a>

### `vmcs_init_host`

```c
static void vmcs_init_host(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写宿主 EFER、控制字段、CR、段选择子、本核 TSS 基址、GDTR/IDTR、FS/GS base 与 VMX 返回 RIP。

<details>
<summary>函数内备注（4 条）</summary>

**第 1205 行**

> 读取当前 GDTR / IDTR

**第 1210 行**

> 宿主 TR 必须是本核的 TSS。

> 这里原来硬读 GDT[6](= cpu0 的 TSS 描述符, selector 0x30), 于是 VMCS
> 宿主区里存的就是 cpu0 的 TSS。VM-exit 时 CPU 会按这两个字段装回宿主的
> TR —— 而宿主区只会被装回, 不会被保存, 那个陈旧值就永远留在 VMCS 里。
> 后果：vCPU 在 cpu1 上退出一次之后, cpu1 的 TR 就指向 g_tss[0]。

> TR 是 ring3→ring0 换栈的依据(用 TSS.RSP0), 而 g_tss[0].rsp0 是
> cpu0 当前任务的内核栈顶。于是 cpu1 上任何一个用户态中断/异常
> (定时器, 用户缺页)都会把陷阱帧压进 cpu0 那个任务的栈里 —— 两颗核
> 同时写同一段栈。实测症状：宿主 busybox 恢复出一个错的 RIP 和一个垃圾
> 指针(`User PF CR2=0x1103`, RIP 处字节是数据不是代码), 而且只有
> SMP&gt;1 且 vCPU 跑在非 BSP 核上时才出现 —— 钉在 cpu0 时永远不复现。

**第 1248 行**

> 宿主的 FS/GS base 是 VMCS 宿主状态字段：VM-exit 时 CPU 会拿这里
> 的值装回 IA32_FS_BASE / IA32_GS_BASE。原来这两行写的是 0, 等于
> 「每从 guest 退出来一次, 就把宿主的 per-CPU 指针清零」——
> 宿主自己的 %gs 相对寻址随即全指向垃圾。实测症状：用户进程一发
> syscall, 宿主 SYSCALL 入口的 `push %gs:0x60` 就缺页到地址 0x60,
> 日志上看起来像「用户态程序把内核搞崩了」。

> 这里先取真实值；之后每次入口前还要再刷(见 vmx_refresh_host_state)。

**第 1267 行**

> HOST_RSP 每次 vmlaunch/vmresume 前在 vmx_run.S 中动态写入

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1203`

<a id="fn-kernel-vmm-x86_64-vmx.c-1272"></a>

### `vmcs_init_guest`

```c
static void vmcs_init_guest(vcpu_t *vcpu, void (*entry)(void))
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写 guest 的 CR/段/限制/AR 字段；Linux 路径填 RIP/RSP/CR3、CR4 mask/shadow、EFER 与自建 GDT/IDT/TSS（AP 另走实模式），其它路径沿用宿主状态。

<details>
<summary>函数内备注（27 条）</summary>

**第 1274 行**

> ── CR 寄存器(guest 与 host 共享 CR3, 不使用 EPT)──

**第 1276 行（行尾）**

> 共享 host 页表

**第 1281 行**

> ── 段选择子 ──

**第 1291 行**

> ── 段基址 ──

**第 1300 行**

> TSS base(与 host 相同)

**第 1313 行**

> ── 段限制 ──

**第 1321 行（行尾）**

> 103 bytes TSS

**第 1325 行**

> ── 段访问权限 ──

**第 1326 行（行尾）**

> 64-bit code: L=1, G=1, P=1, DPL=0

**第 1332 行（行尾）**

> LDT, present

**第 1333 行（行尾）**

> Busy TSS 64-bit

**第 1335 行**

> ── SYSENTER ──

**第 1340 行**

> ── RIP / RSP / RFLAGS ──
> Linux 引导路径(guest_boot.c)会填好 g_rip/g_rsp/g_cr3 并要求直接用；
> 玩具 guest(VMM_TEST)没填, 退回&quot;内核符号地址 + 静态栈&quot;的老行为。

**第 1348 行**

> guest 自己的 CR3(GPA)：下面是它自建的临时页表, 不是宿主的

**第 1351 行**

> ── CR4 与 VMXE：用 guest/host mask + read shadow, 不要改 GUEST_CR4 ──

> 两个约束看起来是矛盾的：
> (a) VMX 操作期间 CR4.VMXE 是固定位, 必须为 1 ——
> GUEST_CR4 里清掉 VMXE, VM-entry 直接以 reason 33
> 「invalid guest state」被拒(guest 一个指令都跑不到)；
> (b) 但 guest 看到的 CR4 不能有 VMXE —— Linux 早期启动会
> `mov %eax,%cr4`(只留 PAE|MCE), 在 non-root 下清 VMXE
> 触发 #GP, 那时它还没有 IDT → triple fault。

> 解法就是硬件为此提供的那对字段：
> CR4_MASK        = 1&lt;&lt;13  → guest 写 VMXE 位会陷入 VMM；
> CR4_READ_SHADOW = 真实值去掉 VMXE → guest 读 CR4 看到的是这个。
> 于是 GUEST_CR4 始终带 VMXE(满足 a), guest 读写看到的都没有
> (满足 b), 而 guest 对 CR4 的其它位改动照常生效。

**第 1372 行**

> 长模式：LME=1 且 LMA=1(后者硬件进入时置位, 但 VMCS 里要一起给,
> 否则 64 位 guest 的 vmresume 会因 EFER 非法而失败)

**第 1377 行**

> 注意： 三个描述符表(GDTR/IDTR/TR)的 base 必须落在 guest 物理空间内。

> 玩具 guest 直接用宿主的 GDTR/IDTR/TR 也能进, 是因为那条路径
> 没开 EPT —— 那时 base 被当作宿主物理地址解释。一旦开了 EPT,
> VM-entry 会按 guest 物理地址去检查它们, 而宿主的
> 0xffff8000_002001d0 这种内核虚拟地址远超 guest 物理宽度,
> 结果就是第一次 entry 直接被拒：exit reason 33「invalid guest
> state」, guest 一个指令都没执行。

> 所以这里给 guest 自建 GDT(含 64 位代码段), 空 IDT, 空 TSS,
> 三者都在 guest 低端内存里(0x98000/0x99000/0x9A000)。

**第 1395 行（行尾）**

> 104 字节, 64 位 TSS

**第 1396 行（行尾）**

> busy 64-bit TSS

**第 1398 行**

> 选择子必须是 Linux 硬编码的那两个：CS=0x10, DS/ES/SS=0x18
> (见 guest_boot.c 里 GDT 布局的说明)

**第 1419 行**

> ── AP：SIPI 之后的 16 位实模式起点 ────────────────────

> guest 在 BSP 上写 ICR 发 SIPI, vmm_vcpu_start() 记下
> CS base = vector &lt;&lt; 12 并放行 AP 任务; AP 任务第一次进
> guest 前在这里构造 VMCS。现场必须与真机 SIPI 后一致：

> CS.base = vector &lt;&lt; 12, RIP = 0, 其余段 base = 0
> CS/DS/... limit = 0xFFFF, 16 位代码/数据属性
> CR0 = ET|NE, PE=0, PG=0(实模式)
> EFER = 0, ENT_CONTROLS 清 IA-32e guest 位
> RFLAGS = 0x2(IF=0)

> 注意 CR0 里的 NE(bit5)：IA32_VMX_CR0_FIXED0 在去掉
> PE/PG(unrestricted guest 才允许清这两位)之后**仍要求 NE=1**。
> 照抄架构 reset 值 0x60000010 会在 VM-entry 被拒(reason 33,
> inst_error 不可靠地读成 0), 日志上只看到「invalid guest
> state」和一行完全合法的寄存器快照。KVM 的做法是
> GUEST_CR0 = (guest_cr0 &amp; ~(CD|NW)) | NE —— 这里等价于 0x30。
> 去掉 CD/NW 无副作用：Linux 的 trampoline 本来就会重写 CR0。

> CPU2_UNRESTRICTED_GUEST 是这条路的前提(见 vmcs_init_ctrl)：
> 没有它, CR0.PE=0 的 guest state 会被 VM-entry 以 reason 33
> 拒绝。Linux 的 trampoline 在这里从实模式自己切到保护/长模式,
> 与 QEMU/KVM 的 AP 启动路径一致。

> 注意：GDTR/IDTR 的 base 必须是 guest 物理空间内的值(全 0 合法,
> 实模式下 trampoline 会自己 lgdt/lidt)；TR 给 reset 默认的
> 32 位 busy TSS(AR=0x8b), LDTR 标成 unusable(AR bit16)。

**第 1451 行（行尾）**

> ET|NE, PE=0, PG=0

**第 1453 行（行尾）**

> VMXE(固定位), guest 不可见

**第 1500 行（行尾）**

> unusable

**第 1505 行**

> ── 32 位保护模式 guest(Linux 传统引导协议)──

> 上面的 64 位状态全部覆盖掉, 对标 tgoskits 的引导 stub 与
> QEMU 的 load_linux()：
> CR0  = PE|NE|ET(PG=0, 分页关闭 —— 靠
> CPU2_UNRESTRICTED_GUEST 才被允许)
> CR4  = VMXE(固定位, guest 看不到, 靠 READ_SHADOW 置 0)
> EFER = 0(LME/LMA=0)
> CS=0x10 / DS..GS=0x18, 32 位 flat 段(AR=0xc09a / 0xc092)
> 这样 guest 等价于「刚做完 16→32 位切换」的机器, 剩下的
> 32→64 位切换, 页表, identity map, 5 级页表探测全部由内核
> 自己的 startup_32/startup_64 完成 —— 与 QEMU 裸跑同一条路。

> 判定依据 g_cr3 == 0 是 guest_boot.c 里的约定(非零=64 位直启)。

**第 1531 行**

> 注意： AR 必须带 accessed 位(0x9b/0x93)。GDT 描述符里写的
> 是 0x9a/0x92, 但 VMCS 里存的是已加载段的缓存属性 ——
> 硬件在加载段时会把 A 位置 1。写成 0x9a/0x92 的后果是
> VM-entry 直接被拒：exit reason 33「invalid guest state」,
> guest 一条指令都跑不到(实测)。

**第 1544 行**

> 32 位保护模式：清掉 ENT_GUEST_64(IA-32e guest)。
> ENT_LOAD_EFER 保留 —— GUEST_EFER 里的 0x801(SCE|NXE, LME=0)
> 对 32 位 guest 是合法值(KVM 同款), 而关掉 load 反而会让
> guest 带着宿主的 EFER(LMA=1)跑 32 位模式, 自相矛盾。

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1272`

<a id="fn-kernel-vmm-x86_64-vmx.c-1564"></a>

### `vcpu_vmcs_init`

```c
static int vcpu_vmcs_init(vcpu_t *vcpu, void (*entry)(void))
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化虚拟 MSR 影子与 APIC base 并初始化 vLAPIC，清零后 VMCLEAR/VMPTRLD 本 vCPU 的 VMCS，配置控制/宿主/guest 字段、EPT 指针与 MSR 列表。

<details>
<summary>函数内备注（8 条）</summary>

**第 1571 行**

> 虚拟 MSR 影子初始化(MSR load list 的初始化在 vmcs_load 之后,
> 因为那几个字段是写进当前被装载的 VMCS 的)。

> EFER 抄宿主的(SCE/NXE/LME 都是 64-bit guest 需要的), 之后由
> msr_emulate_write 维护 —— guest 看到的 EFER 恒等于 vcpu-&gt;msr_efer,
> 而真实 MSR 只有宿主在用。

> APIC base 预置成上电默认值 0xFEE00900：EN(bit11)=1, BSP 位(bit8)=1,
> 基址 0xFEE00000。vLAPIC 靠读它判断 guest 是否已经打开 APIC。

**第 1582 行**

> AP 由 SIPI 启动, 初始 EFER 必须为 0(实模式)：宿主的 EFER 带着
> LME/LMA, 交给实模式 guest 会让 VM-entry 直接判非法。

**第 1594 行（行尾）**

> 上电默认 PAT

**第 1595 行**

> IA32_APIC_BASE：BSP 位只有 0 号 vCPU 有(只读拓扑信息)

**第 1617 行**

> 若已协商启用 EPT, 写入 EPTP(EPT 根页表指针)

**第 1622 行**

> Flush: 把 VMWRITE 写的字段从 CPU 内部缓存写回内存,
> 同时将 launch state 重置为 &quot;clear&quot;,
> 这样后续 vmptrld + vmlaunch 才能正确工作。
> (参考 Intel SDM 和 ref/bare-vm/arch/x86_64/vm.c:arch_vm_init)

**第 1628 行**

> MSR 自动换入换出区：guest 的 syscall MSR 必须由硬件在进出时装卸,
> 否则 guest 的 `syscall` 会跳到宿主内核的入口(见 vmx_msr_lists_init)。
> 必须在 vmcs_clear 之前写 —— 那几个字段属于当前装载的 VMCS。

**第 1633 行**

> 踩坑记录(2026-09-25)：这两个地址字段原本写成 0x2012/0x2016,
> 都是错的。症状极具误导性：guest 进去就再也没出来, 但
> 没有 VM-entry failure, exit handler 一次都没进, 宿主最终 IF=0
> 停住 —— 看起来像&quot;这套机制不能用&quot;。实际是地址指到了别的控制字段上,
> 硬件从错误的地方读装填表。
> 权威编码查 `arch/x86/include/asm/vmx.h`：
> VM_EXIT_MSR_LOAD_ADDR  = 0x2008
> VM_ENTRY_MSR_LOAD_ADDR = 0x200a
> (计数字段 0x400e/0x4010/0x4014 原本就是对的。)

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1564`

<a id="fn-kernel-vmm-x86_64-vmx.c-1717"></a>

### `chipset_of`

```c
static inline x86_chipset_state_t *chipset_of(const vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 VM 的 chipset 指针，无 VM 时返回全局后备 chipset。

**位置**：`kernel/vmm/x86_64/vmx.c:1717`

<a id="fn-kernel-vmm-x86_64-vmx.c-1722"></a>

### `ioapic_reg_read`

```c
static uint32_t ioapic_reg_read(x86_chipset_state_t *chip, uint32_t reg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 IOAPIC ID=0、版本与最大重定向项、ARB=0 或重定向表项，越界返回 0。

<details>
<summary>函数内备注（2 条）</summary>

**第 1726 行（行尾）**

> ID = 0

**第 1730 行（行尾）**

> ARB(只读)

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1722`

<a id="fn-kernel-vmm-x86_64-vmx.c-1738"></a>

### `ioapic_reg_write`

```c
static void ioapic_reg_write(x86_chipset_state_t *chip, uint32_t reg, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅接受重定向表范围的写入并保存，只读寄存器忽略。

<details>
<summary>函数内备注（1 条）</summary>

**第 1740 行**

> ID/VER/ARB 只读, 写忽略；只接受重定向表

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1738`

<a id="fn-kernel-vmm-x86_64-vmx.c-1776"></a>

### `pit_now`

```c
static uint16_t pit_now(x86_chipset_state_t *chip, int ch)
```

状态在 vm->chipset

<details>
<summary>函数内备注（2 条）</summary>

**第 1785 行（行尾）**

> 标定未就绪/回绕：当作刚装载

**第 1788 行（行尾）**

> mode 0：到 0 停住

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1776`

<a id="fn-kernel-vmm-x86_64-vmx.c-1792"></a>

### `pit_read`

```c
static uint8_t pit_read(x86_chipset_state_t *chip, int ch)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

有 latch 快照时按先低后高返回并清除，否则读当前计数，每次翻转高低字节选择位。

<details>
<summary>函数内备注（2 条）</summary>

**第 1798 行（行尾）**

> latch 快照：先低后高

**第 1807 行（行尾）**

> 内部 LSB/MSB 翻转触发器

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1792`

<a id="fn-kernel-vmm-x86_64-vmx.c-1811"></a>

### `pit_write_ctrl`

```c
static void pit_write_ctrl(x86_chipset_state_t *chip, uint8_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略 read-back 命令；latch 命令冻结当前计数，否则记录读写模式并复位字节指针与 latch 状态。

<details>
<summary>函数内备注（3 条）</summary>

**第 1816 行（行尾）**

> read-back 命令：忽略

**第 1818 行（行尾）**

> Latch：冻结当前计数

**第 1825 行**

> 装/卸模式(RW 位)并复位字节指针；mode/BCD 位忽略 —— 我们只做
> 「递减到 0」这一种, 够内核标定用。

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1811`

<a id="fn-kernel-vmm-x86_64-vmx.c-1834"></a>

### `pit_write_data`

```c
static void pit_write_data(x86_chipset_state_t *chip, int ch, uint8_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 rw 模式写计数低/高字节，RW=3 时写 MSB 才装载，装载后置 running、记录起始纳秒并复位读状态。

<details>
<summary>函数内备注（4 条）</summary>

**第 1838 行（行尾）**

> 只写 LSB

**第 1840 行（行尾）**

> 只写 MSB

**第 1842 行（行尾）**

> RW=3：先 LSB 后 MSB, 写 MSB 才装载

**第 1852 行（行尾）**

> 真 8254 是在写 MSB 那一刻起算

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1834`

<a id="fn-kernel-vmm-x86_64-vmx.c-1858"></a>

### `x86_pio_handle`

```c
int x86_pio_handle(vcpu_t *vcpu, uint32_t port, int is_in, uint32_t bytes, uint64_t *val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按端口模拟 COM1（转 vUART）、8259 屏蔽字、8254 PIT、端口 0x61、CMOS 与 PCI 配置（全 1），未识别端口返回 0。

<details>
<summary>函数内备注（10 条）</summary>

**第 1862 行（行尾）**

> 端口访问一律按字节语义处理

**第 1865 行**

> ── COM1(16550A)：复用 vUART 的寄存器状态机 ──
> x86 Linux 的 8250 驱动读写的就是这 8 个端口。

**第 1882 行**

> ── 8259 PIC 桩 ──
> 中断走 vLAPIC, 这里没有真控制器；但 Linux 要读写它做探测/屏蔽。
> 给「全屏蔽, 无中断挂起」的一致视图：读 IMR 回写过的掩码,
> 读 IRR/ISR 回 0, 写 ICW/OCW2(EOI) 忽略。

**第 1909 行**

> ── 8254 PIT ── 实现见上面 pit_* 一组函数。
> 只做计数, 不产生任何中断(guest 的 tick 由 vLAPIC timer 提供)。

**第 1926 行**

> ── 0x61(端口 B / NMI 状态)──
> 内核写它是为了「抬高通道 2 的 gate, 关掉喇叭」, 读它是为了取样
> 通道 2 的输出位。bit5 = 输出(mode 0 下数到 0 才置 1)。
> bit4 是输出的反相副本, 老代码用它做「计数器已归零」检测。

**第 1937 行（行尾）**

> 保留 gate/speaker

**第 1939 行（行尾）**

> bit5: 计数到 0

**第 1944 行**

> ── 0x70/0x71(CMOS / RTC)──
> 回 0：Linux 读到的 RTC 是 1970, 但不会挂。

**第 1955 行**

> ── PCI 配置空间(0xCF8/0xCFC)──
> 全 1 =「没有这个设备」, Linux 会判定无 PCI 总线。

**第 1970 行（行尾）**

> 诊断口：历史上什么都不接

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1858`

<a id="fn-kernel-vmm-x86_64-vmx.c-1989"></a>

### `vmx_exit_handler`

```c
static int vmx_exit_handler(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

记退出计数/直方图/环形轨迹并保存 rflags，按 reason 处理 HLT（清影子、步进、yield）、VMCALL、CPUID、EPT violation 等。

<details>
<summary>函数内备注（49 条）</summary>

**第 1999 行**

> 每颗 vCPU 的前 10 次退出打详细。

> 注意：原来这里是一个整机共享的 `static unsigned n`, 前 25 次退出
> 才打印 —— 多 vCPU 时 BSP 的启动早期就把配额用光了, AP 的退出
> 一条都看不见, 排查 AP 启动会完全瞎掉。

**第 2015 行**

> 环形记录(见 g_exit_trace 的注释)—— 只在 triple fault 时才回放

**第 2025 行**

> 保存 guest rflags(从 VMCS 读取, 不从寄存器)

**第 2032 行**

> 注意： 清掉 STI / MOV-SS 影子位。

> guest 的 default_idle 就是 `sti; hlt`：hlt 落在 sti 的影子里,
> 于是 VM-exit 时硬件把 interruptibility state 的 bit0=1 一并存进
> VMCS。我们步进 RIP 跳过 hlt 再进去时, 那个 bit 还是 1 —— 硬件
> 认为「影子还没走完」, 于是：
> - 注入外部中断 → VM-entry 直接被拒(reason 33)
> - 开 interrupt-window → 窗口条件要求「无阻塞」, 永远不成立
> 实测退化成 870 万次 HLT 退出的纯自旋(guest 再也收不到 tick)。

> 语义上清掉是对的：影子要求「sti 的下一条指令执行完才认中断」,
> 而 hlt 已经执行完了(我们正是因此才拿到 HLT 退出)。

**第 2048 行**

> HLT 类似 WFI：步进 RIP, yield, resume。
> 指令长度取自 VMCS EXI_INST_LEN(移植自 kvmm handle_hlt),
> 而非硬编码 +1 —— HLT 实际为 1 字节, 但统一用 inst_len 更稳妥。

**第 2053 行**

> kvmm 在让出前执行 vmclear：VMCS 的「当前加载」状态不能跨线程
> 存续, 清除后由 vmm_arch_enter_guest 重新 vmptrld。

**第 2063 行**

> VMCALL: rdi = hypercall no, rsi = arg1

**第 2066 行**

> 步进 RIP 跳过 vmcall(长度取自 VMCS, 移植自 kvmm handle_vmcall)

**第 2071 行**

> 留在 INFO(test-vmm 的通过证据), 抽稀成有界输出

**第 2087 行**

> 报告一个保守自洽的 CPU, 而不是把宿主的 CPUID 原样透给 guest
> (见 vmx_cpuid_emulate 的说明)。

**第 2092 行（行尾）**

> TEMP-DBG：前 12 次 CPUID + 每次 0x15/0x16(TSC 频率)

**第 2111 行**

> EPT 违规 → 设备 MMIO 模拟(移植自 kvmm handle_ept_violation)。

> EPT violation 是指令级陷入：RIP 指向出错指令, 需手动步进。
> 访问方向/宽度从 EXI_QUALIFICATION 解码, 读写数据经 guest 寄存器
> (GPR 由 vmx_run.S 在退出时保存到 vcpu-&gt;regs)传递。

> qualification 位：0=读,1=写,2=取指,3=读权限,4=写权限,5=取指权限,
> 6=因 EPT 页表项无效(vs 权限不足)。

**第 2125 行**

> ── 先分 RAM / MMIO ────────────────────────────────────────

> 按需分页之后 EPT 初始是空的, 所以 guest 每一次落到新页都会走到
> 这里。RAM 窗口内的走缺页分配, 窗口外的才是设备 MMIO。

> 注意： 这个分支必须在最前面。老版本没有它 —— 因为那时 guest RAM 是
> 预先整段映射好的, 任何 EPT violation 按定义都必是非 RAM。改成按需
> 分页后若不加这一条, guest 取指/访存第一次碰到新页就会掉进下面的
> MMIO 解码路径：解码失败 → 落到最底下的兜底 → 只推进 RIP, 不写
> 目标寄存器, 于是 guest 拿着上一条指令的残留值继续跑。那种故障
> 表现为&quot;guest 随机跑飞&quot;, 比直接崩掉难查得多。

> 注意： 不推进 RIP —— 这正是&quot;缺页&quot;与&quot;MMIO 模拟&quot;的分野：MMIO 那条路
> 是 VMM 替 guest 完成了这次访问, 所以要把 RIP 挪过去；这里只是把内存
> 补上, 访问本身还得 guest 自己重做一次。两条路走反了的表现分别是
> &quot;死循环&quot;和&quot;跳过一条随机指令&quot;。

**第 2145 行（行尾）**

> zero

**第 2147 行**

> 注意：n == 0 不一定失败。多 vCPU 下兄弟 vCPU 可能刚把同一个
> 2 MiB 块装好(map_block 幂等跳过 512 个已映射页 ⇒ 返回 0),
> 这时 lookup 命中就是成功。判据必须是 lookup 的结果, 不能是
> map_block 的返回值 —— 否则「两个 vCPU 同时启动」会被误判成
> PMM 耗尽, 直接把 VM 停掉。aarch64 上这条是 §5.3 的一部分。

**第 2171 行（行尾）**

> TEMP-DBG：前 8 次 EPT violation 的 gpa, 看 guest 在碰什么

**第 2174 行**

> KLOG_INFO(&quot;[EPT-DBG] gpa=0x%llx %s rip=0x%llx\n&quot;,
> (unsigned long long)gpa, is_write ? &quot;W&quot; : &quot;R&quot;,
> (unsigned long long)guest_rip);
> ne++;

**第 2185 行**

> 清零：imm 仅立即数写路径才赋值, 其余路径保持 0

**第 2188 行**

> 解码出错指令(GUEST_RIP 处, CISC 变长)以取得
> 「数据寄存器 + 访问宽度 + 源/目标方向」

**第 2192 行（行尾）**

> 立即数写

**第 2194 行**

> 注意： 判据必须是 has_reg, 不能写 `acc.reg != 0` —— 0 就是
> %rax, 那样会把所有从 RAX 传值的写当成立即数 0(见结构体
> 注释里的实测症状)。

**第 2204 行**

> vLAPIC 的 MMIO 窗口先于总线处理。
> 0xFEE00000 在 guest RAM 之外 → EPT 里是无效项 → 每次
> LAPIC 访问天然走到这里, 不需要 VMX 的 APIC-access page。

**第 2209 行**

> IO-APIC(0xFEC00000, 同样在 guest RAM 之外)。
> 我们只投 vLAPIC 的 timer, 没有任何真外部中断, 所以要给
> Linux 一个「所有重定向项都屏蔽」的自洽视图：它读 IOAPICVER
> 拿到版本和表项数, 读写重定向表做屏蔽/解屏蔽, 然后认定
> 没有可用的外部中断 —— 这正是我们要它相信的。

> 没有这个桩时每次访问都落到下面的「未处理 EPT」, 只靠
> 步进 RIP 蒙混过去：目标寄存器根本没人写, guest 读回
> 的是上一条指令的残留值(实测 IOAPICVER 读成 0, 内核于是
> 把表项数算错, 打出 &quot;version 0, GSI 0-95&quot;)。

**第 2227 行**

> 注意： 必须按页内偏移区分两个寄存器。曾经写成
> `(gpa &amp; ~0xFFF) == BASE &amp;&amp; (gpa &amp; 0xf) == 0` ——
> 0xFEC00010 也满足 `&amp; 0xf == 0`, 于是所有 IOWIN
> 访问都被当成 IOREGSEL, 数据寄存器永远读不回真值,
> Linux 拿它去建 irq_cfg 直接空指针崩在 setup_IO_APIC。

**第 2236 行（行尾）**

> IOREGSEL

**第 2241 行（行尾）**

> 0x10 IOWIN

**第 2271 行**

> 读：写回目标 GPR

**第 2288 行**

> 未映射且不是已知设备：记一笔再吞掉。

> 注意： 只推进 RIP 是不够的 —— 读操作的目标寄存器会保留上一条指令
> 的残留值, guest 于是拿着一个看似合法, 实则陈旧的数继续跑。老版本
> 就是这么写的, IOAPICVER 读成 0 那类 bug 正是它养出来的
> (见本文件上面 IO-APIC 那段的注释)。这里至少把目标寄存器写成 0,
> 让&quot;读到一个设备返回 0&quot;这件事显式化。

**第 2313 行**

> CR 访问陷入。目前只有 CR4.VMXE 一个位被 mask 住(见
> vmcs_init_guest 的说明), 所以这里只需处理「写 CR4」：
> 把 guest 想要的其它位照收, 但 VMXE 必须保持 1(否则下一次
> VM-entry 又会 reason 33), 并把 guest 可见的影子同步过去。

> qualification 低 4 位 = CR 编号, bits 5:4 = 访问类型(0=写CR,
> 1=读CR, 2=写CR8 hmm 见 SDM), 不是 CR4 就原样放行。

**第 2326 行（行尾）**

> MOV CR4, r

**第 2340 行**

> IN/OUT：I/O bitmap 全拦, 所以 guest 每次端口访问都到这里。
> qualification 给端口号/方向/宽度, 指令长度由 VMCS 提供。

**第 2354 行**

> 无设备端口：按「总线浮空」返回全 1(PC 上的惯例)

**第 2366 行**

> rdmsr 结果进 EDX:EAX

**第 2382 行**

> 宿主外部中断。PIN_EXTINT 置位后, non-root 里的物理中断一律先
> 退到这里 —— 否则硬件会按 guest 的 IDT 投递, 宿主的时钟
> ISR 永远跑不到, 调度器饿死。
> 让出 CPU 让宿主 ISR 跑完, 再原样回 guest。
> guest 自己的中断走另一条路：vLAPIC → VM-entry 注入。

**第 2390 行**

> 让出 CPU：宿主的外部中断 ISR, 调度器都要借这次退出跑完。
> 不 yield 的话 vcpu 任务会一直占着宿主 CPU —— 宿主时钟 ISR
> 仍在推进, 但别的任务(含 guest 控制台轮询)永远排不上队。

**第 2399 行**

> guest 刚把 RFLAGS.IF 从 0 变成 1：挂起的事件现在可以投了。
> 真正的写入在下次 VM-entry 前(vmm_arch_restore_guest_ctx
> → vmx_inject_pending)。

**第 2409 行**

> 回放最近 VMM_EXIT_TRACE_MAX 次退出(最旧在前)。triple fault
> 往往是&quot;前面某个异常没被正确处理&quot;的终点, 这一段比终点本身有用。

**第 2421 行**

> 把出错 RIP 附近的 guest 字节 dump 出来。guest RAM 就在宿主窗口里,
> 可以直接读 —— 没有这一手就只能对着 &quot;triple fault&quot; 干瞪眼。

**第 2430 行**

> guest_rip 是线性地址：guest 内核 text 在 0xffffffff80000000
> 之上, 直接拿它跟 guest 物理窗口(0..192MiB)比永远不成立 ——
> 早先的写法就是这样, 于是最需要 dump 的时候一行都没打。
> 按 __pa_symbol 的公式换算：phys = rip - __START_KERNEL_map +
> phys_base, 其中 phys_base = 0x1000000(bzImage 的 pref 地址)。

**第 2465 行**

> 诊断：guest 的 #PF(异常位图只开了 bit14)。CR2 此刻还是 guest
> 的值 —— VM-exit 不保存/恢复 CR2, 这是唯一能看到缺页地址的机会。

**第 2514 行**

> dump guest 栈顶 12 个 qword(物理地址可读 → 可符号化)

**第 2528 行**

> TEMP: 前 3 次缺页的完整现场(缺页处理器拿到的就是这些)。
> 之后不再打印, 避免宿主代码把 CR2 冲掉影响后续判断。

**第 2549 行**

> 缺页现场把 inflate_fast 的栈帧读出来：序言把
> end 存在 0x00(%rsp) (aa9017)
> beg 存在 0x10(%rsp) (aa9033)
> 于是 op = out - beg 可以直接算, 判定是 dist 野还是 beg 歪。

**第 2559 行**

> 按需分页下这几个地址可能还没被 guest 碰过 —— 判 NULL 跳过

**第 2581 行**

> 原样注回 guest：硬件异常, 带 error code(如果需要)

**第 2582 行（行尾）**

> 注回 guest：硬件异常原样送回

**第 2599 行（行尾）**

> VM-entry failure: invalid guest state

**第 2613 行**

> 这三样是「注入外部中断被拒」的头号嫌疑：
> actv=1        guest 处于 halted(执行过 hlt)
> intr_state    bit0=STI 屏蔽 bit1=MOV-SS 屏蔽 bit3=SMI
> inst_error    硬件给出的具体失败原因(SDM 表 30-1)
> 注意 inst_error 是「上次失败的 VM-entry/VMREAD」留下的,
> 要在这里立刻读, 任何其它 vmread/vmwrite 都可能覆盖它。

**第 2622 行**

> inst_error 必须最后读：读任何其它 VMCS 字段都可能把它覆盖

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:1989`

<a id="fn-kernel-vmm-x86_64-vmx.c-2632"></a>

### `vmm_arch_vm_init`

```c
int vmm_arch_vm_init(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 vCPU 数，检查 VMX 支持并执行 VMXON；有独立内存时建 EPT 空表并注册 16550A；逐 vCPU 清零初始化，并复位本 VM 的 chipset 与诊断缓冲。

<details>
<summary>函数内备注（8 条）</summary>

**第 2642 行**

> 检查 VMX 硬件支持

**第 2646 行**

> 全局 VMX 初始化(VMXON)

**第 2650 行**

> EPT 二级地址翻译(移植自 kvmm mm/ept.rs)：仅在 VM 请求了独立
> guest RAM(cfg.mem_size != 0)时建立。

> 注意： 建出来的是空表(&quot;空表即全 trap&quot;)：guest RAM 不再预映射, 首次
> 访问才由 EPT violation 处理分配物理页并装映射。宿主自己要写的那几块
> (bzImage 各段 / initrd / boot_params / 初始页表)由 guest_boot.c 用
> x86_ept_map_range() 显式映射 —— 与 aarch64/riscv 完全同构。

> 注：即使建表, 也需 CPU_EXEC_CTRL1 的 CPU_EPT 位真正置位才生效
> (见 vmcs_init_ctrl 协商结果)。

**第 2666 行**

> 注册失败 = 静默死控制台, 必须让建 VM 失败(同 aarch64/riscv)

**第 2679 行**

> 初始化每个 vCPU

**第 2686 行（行尾）**

> 还没跑过：0 是合法 CPU, 不能拿 0 当&quot;未运行&quot;

**第 2694 行**

> exit 诊断环形缓冲与 vCPU 同生命周期 —— 不清的话第二次启动回放出来的
> 是上一次启动的尾巴(和 static 限流计数是同一类坑)。
> 只清本 VM 这一格：这些数组现在按 (vm-&gt;slot, vcpu_id) 索引, 全清会
> 把另一个正在跑的 VM 的诊断信息一起抹掉。

**第 2708 行**

> ── 复位虚拟设备桩的可变状态 ────────────────────────────────

> 语义上每次 boot 本就该等于设备上电。从前这些是文件级 static,
> 生命周期是整个内核, 于是第二次启动会带着第一次停止那一刻的残留：

> - ioapic_inited==1 ⇒ 「把所有重定向项设成屏蔽」那段初始化被整段
> 跳过(它是 if (!inited) 守卫的)；
> - ioapic_sel / ioapic_rt[] ⇒ 上次的寄存器选择与路由表；
> - PIC 屏蔽字, PIT 通道, 端口 61 同理。

> 症状(x86_64 + helper + SMP=4 实测)：第二次启动的 guest triple fault,
> RIP 每次都是同一个地址 —— 确定性崩溃, 只是&quot;是否踩中&quot;取决于上次停止时
> 残留了什么, 所以表现为概率性。

> 注意： 多 VM 之后这段不能再写成全清：vm_t 里每 VM 一份 chipset, 但这段
> 代码是在新 VM 启动时跑的, 全清就会把另一个正在跑的 VM 的 PIC/PIT/
> IOAPIC 状态一起抹掉(单 VM 下看着对, 多 VM 下就是串台)。
> (vLAPIC 和 UART 桩各自在 vlapic_init()/uart16550_init() 里已经 memset,
> 不在这里重复 —— 那两个函数每次 boot 都会被调到。)

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:2632`

<a id="fn-kernel-vmm-x86_64-vmx.c-2750"></a>

### `vmm_arch_vm_destroy`

```c
void vmm_arch_vm_destroy(vm_t *vm)
```

vmm_arch_vm_destroy — 与 vmm_arch_vm_init 严格逆序

本架构目前还没有池化的设备：uart16550 / vlapic / chipset 的状态仍在 vm_t
里, 由 vm_free 的 memset 顺带清掉, 所以这里暂时无事可做。等它们搬进
静态池之后在这里补(顺序与 init 相反)。

**位置**：`kernel/vmm/x86_64/vmx.c:2750`

<a id="fn-kernel-vmm-x86_64-vmx.c-2760"></a>

### `vmx_vcpu_setup`

```c
int vmx_vcpu_setup(vcpu_t *vcpu, void (*entry)(void))
```

vmx_vcpu_setup — 为 vCPU 设置 VMCS(含 guest 入口地址)
必须在 vcpu_task_create 之前调用(或在 vCPU 任务启动时调用)。

**位置**：`kernel/vmm/x86_64/vmx.c:2760`

<a id="fn-kernel-vmm-x86_64-vmx.c-2802"></a>

### `vmm_arch_irq_raise`

```c
void vmm_arch_irq_raise(vcpu_t *vcpu, virq_t irq)
```

vmm_arch_irq_raise — x86_64 的「拉线」适配器(见 include/vmm/vmm_virq.h)

irq.line 是 IO-APIC GSI, 不是 vLAPIC 的 vector —— 向量由 guest 写进
重定向表, VMM 只负责查表。IO-APIC 是纯桩(只保存 guest 写进来的值), 所以
guest 还没 unmask 之前这条线等于没接上, 这里直接返回是对的。

注意： 这条路径从前硬编码 `ioapic_rt[4 * 2]`(GSI 4), 而 vmm_console.h 的
`VMM_CONSOLE_IRQ` 在 x86 上展开成 `UART16550_IRQ`(10) —— 两个是不同的数。
之所以一直没爆, 是因为那个宏在 x86 上没有使用点。收拢到 VIRQ_CONSOLE
时一并改掉了：现在索引由 irq.line 决定, 写死 4 的隐患不存在了。

<details>
<summary>函数内备注（5 条）</summary>

**第 2816 行（行尾）**

> guest 还没 unmask / 非 fixed 模式

**第 2819 行（行尾）**

> 0-15 是异常向量, 不能当普通中断投

**第 2821 行**

> ── 目标 vCPU 过滤 ─────────────────────────────────────────

> 多 vCPU 之后每颗 vCPU 都会在入口处调这里重拉电平线; 不按 IO-APIC
> 重定向表的目的地过滤的话, 一条只路由给 CPU0 的串口中断会在每颗
> vCPU 上各投一次 —— AP 收到本不属于它的中断, 白白进一次 ISR。

> 目的地编码与 ICR 相同：
> bit11 = 1 → logical(flat), dest 是逻辑 ID 位图
> bit11 = 0 → physical,      dest 是 APIC ID(0xFF = 广播)
> 本 VMM 的 APIC ID 约定等于 vcpu_id。

**第 2834 行（行尾）**

> logical

**第 2841 行**

> 不做 SVR 门控：那是 vlapic_raise_irq 自己的事(它在 raise 时刻判)

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:2802`

<a id="fn-kernel-vmm-x86_64-vmx.c-2845"></a>

### `x86_console_irq_on_entry`

```c
static void x86_console_irq_on_entry(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若控制台中断处于置起状态则向本 vCPU 拉 VIRQ_CONSOLE。

**位置**：`kernel/vmm/x86_64/vmx.c:2845`

<a id="fn-kernel-vmm-x86_64-vmx.c-2852"></a>

### `vmm_arch_restore_guest_ctx`

```c
void vmm_arch_restore_guest_ctx(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

确保本核 VMXON，首次完整初始化并装载本 vCPU 的 VMCS，轮询 vLAPIC 定时器、拉控制台中断、取 pending 向量注入后进 guest。

<details>
<summary>函数内备注（11 条）</summary>

**第 2854 行**

> ── ① 先让这颗核进入 VMX operation(VMXON 是每 CPU 一次的)──

> 注意： 必须在下面那步之前：VMCS 初始化里的 VMCLEAR/VMPTRLD 是 VMX 指令,
> 没开 VMX operation 直接 #UD。helper 模式下做 VMXON 的是 /bin/vmm-run
> 所在的核, 而 vCPU 钉在 CPU0, SMP&gt;1 时不是同一颗 —— 实测就是
> `CPU exception #6 at RIP=vcpu_vmcs_init+0x156`(#UD), backtrace
> 指向 vmm_arch_restore_guest_ctx。vmm_arch_enter_guest() 里那次仍然保留
> (幂等：s_vmx_on[cpu] 查表), 失败那条路由它去报。

**第 2867 行**

> ── ② 把本 vCPU 的 VMCS 装成&quot;当前 VMCS&quot; ──────────────────────

> 注意： 必须在 vmx_inject_pending() 之前：那个函数会用 vmcs_write() 写
> VM_ENTRY_INTR_INFO, 而 VMREAD/VMWRITE 操作的永远是本核当前装载的
> 那块 VMCS —— 不是&quot;你想操作的那块&quot;。

> 单 VM 时当前 VMCS 恰好一直是它的, 所以看不出问题；多 VM 共用一颗核
> 时就不是了：vm1 的 vCPU 任务退出 → 让给 vm2 → vm2 进来时当前 VMCS
> 还是 vm1 的, 于是 vm2 的中断信息被写进了 vm1 的 VMCS；而
> vlapic_accept_interrupt() 已经把 ISR 位置在了 vm2 的 vLAPIC 上。
> 结果：那个向量既没送到 vm2, vm1 也不会去 EOI 它 —— ISR 位永久卡住,
> 之后 vlapic_take_pending()(irr &amp; ~isr)再也挑不出这个向量。

> 实测症状正是这样：起第二个 VM 之后, 第一个 VM 的定时器在第 501 次
> 触发处停住, LAPIC 快照是 `isr[7]=0x1000`(236 = LOCAL_TIMER_VECTOR)
> 而 `irr` 全 0 —— 输入能进 guest(那是 IO-APIC 的另一条向量), 但
> guest 拿不到 tick, tty 的 workqueue 不跑, 于是&quot;能输入, 没回显&quot;。

> 把装载挪到这里, restore 之后的每一次 vmcs_write/vmcs_read(注入,
> ENTRY-DBG, vmlaunch 本身)都作用在正确的 VMCS 上。

**第 2893 行**

> ── VMCS 初始化搬到跑 vCPU 的这颗核上做(每个 VM 一次)────────

> vmx_vcpu_setup() 内部就是 `VMCLEAR → VMPTRLD → 写全部字段`, 这正是
> 唯一正确的顺序。问题只在于它在哪颗核上跑：

> - VMCLEAR 只对本核 current 的 VMCS 有效。原路径在 /bin/vmm-run
> 所在核(helper 核)上 clear, 随后 vCPU 核一 VMPTRLD, 那块 VMCS
> 就&quot;搬&quot;过去了 —— 再在 helper 核上 clear 是空操作, launch state
> 永远停在 launched ⇒ 第二次启动 VMLAUNCH 报 inst_error=0x4
> (VMLAUNCH with non-clear VMCS), guest 一个字节都不输出。
> - VMCLEAR 会把这 VMCS 复位成&quot;上次退出时的快照&quot;。所以它必须发生在
> 写字段之前：先 clear 再 setup 不能反。(曾经把 clear 单独挪到
> 这里, 排在 setup 之后 —— 结果是刚写好的入口状态被快照覆盖, guest
> 从上次断点继续跑, 内存却已被 memset 清零 ⇒ 连第一次启动都 triple
> fault。)

> 所以：在这里(vCPU 核上)整个重做一遍, vmcs_ready 保证每个 VM 生命
> 周期只做一次。HLT yield 那条路径也会 clear + launched=0, 但它清的是
> 运行中的 VMCS, 快照即最新状态, 不需要(也不该)重做初始化。

> 只对 Linux 引导路径做：玩具 guest(VMM_TEST)的入口在 vmx_vcpu_setup()
> 的 entry 参数里, 这里拿不到, 且它每次 QEMU 只启动一次, 没有这个坑。

**第 2918 行（行尾）**

> clear → load → 写全部字段

**第 2919 行（行尾）**

> 刚 clear 过 ⇒ 必须走 VMLAUNCH

**第 2923 行**

> 无论走上面哪条路, 进 guest 之前都必须让本核的 current VMCS 是这一块：
> - vmx_vcpu_setup() 结尾还有一次 flush 用的 VMCLEAR(把 VMWRITE 的结果
> 真正写回内存), 执行完 VMCS 就不再是 current；
> - HLT yield 那条路径也会 VMCLEAR。
> 少了这一下, 后面的 VMREAD/VMLAUNCH 全在&quot;没有 current VMCS&quot;的状态下执行,
> 症状极具误导性：ENTRY-DBG 读出来的是栈上的垃圾, inst_error 是个非法值
> (实测 0x4400), 而日志里只会看到一句 guest entry failed。

**第 2935 行**

> 本函数返回 void, 这里没有把失败传给调用方的途径 ——
> 原先是 return 0(在 void 里等于丢弃, -Wreturn-type)。
> 语义上这是个遗留缺口, 改成 return 只是把既有行为写明。

**第 2943 行**

> 每次进入 guest 前投递积压的事件(中断/异常)。
> guest 屏蔽着中断时 vmx_inject_pending 会改成开 interrupt-window
> exiting, 等它开中断的那一刻(exit 7)再回来投。

**第 2948 行**

> vLAPIC 虚拟定时器：看 deadline 过没过, 过了就排一个外部中断。
> 轮询节拍由 PIN_EXTINT 提供 —— 宿主 100Hz 时钟本身就让 guest
> 每 10ms 退出来一次, 不需要额外的宿主定时器基础设施。

**第 2955 行**

> 控制台：电平触发, 每次入口按设备状态重拉(见上面 x86_console_irq_on_entry)

**第 2966 行**

> 其余 guest 系统寄存器由 VMCS 自动保存/恢复

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:2852`

<a id="fn-kernel-vmm-x86_64-vmx.c-2995"></a>

### `vmx_refresh_host_state`

```c
static void vmx_refresh_host_state(vcpu_t *vcpu)
```

vmx_refresh_host_state — 把宿主的易变状态刷进 VMCS 宿主区

这三个字段 VMCS 不会自动跟着宿主变, 而宿主在两次 VM-entry 之间可能已经
换过进程(CR3), 换过 CPU 或改过寄存器(FS/GS base)：

HOST_CR0/CR4  — 宿主开关 CR0/CR4 位(WP/SMEP/SMAP)后必须同步,
VM-exit 是从这里装回宿主 CR 的。
HOST_CR3      — 切进程就变。不刷的话 VM-exit 会把宿主装回旧页表,
宿主立刻在别人的地址空间里跑, 症状是随机的 #PF。
HOST_BASE_FS  — 宿主用户态线程的 TLS 基址。
HOST_BASE_GS  — per-CPU 指针。最关键的一个：宿主的 syscall 入口,
中断入口都靠 %gs 取 per-CPU 数据, 装回旧值就全错。
HOST_TR       — 本核的 TSS(选择子 + 基址)。TSS 每核一份, 而上面的
CR3/GS 是"跟着任务变", TR 是"跟着核变" —— 见下面那段。

注意： 除了 VMCS 宿主区, VM-exit 的 MSR load 表里也有宿主的值, 而且
硬件是最后装那张表的(在宿主状态字段之后), 所以表里的值会盖掉
HOST_BASE_GS。GS 那一对(GS_BASE/KERNEL_GS_BASE)在 g_msr_list 里
(guest 的 swapgs 往返要用), 因此这里必须把它们一起刷：表里是
vmx_msr_lists_init() 时的一次性快照, 不刷就等于把宿主装回旧 %gs ——
实测症状是宿主立刻崩, QEMU 反复重启(SeaBIOS → 埋 sentinel → 崩)。

代价是两条 rdmsr + 三条 vmwrite, 相对一次世界切换可以忽略,
所以每次入口前无条件刷(x-kernel 的 refresh_host_state 同理)。

<details>
<summary>函数内备注（5 条）</summary>

**第 2998 行**

> 只有 KERNEL_GS_BASE 需要在 entry-load 表里占个位置；GS_BASE 走
> GUEST_BASE_GS 字段(原因见函数末尾), 所以没有对应的 index 变量。

**第 3009 行**

> HOST_SEL_TR / HOST_BASE_TR —— 本核的 TSS。

> 和 CR3/GS 是同一类问题, 但方向相反：TR 不是&quot;跟着任务变&quot;, 而是跟着核
> 变, VMCS 宿主区又只在创建时抓一次快照。TSS 是每核一份的
> (boot/x86_64/tss.c 的 g_tss[], 选择子 0x30 + cpu_id*0x10), 而 VMCS
> 是每个 VM 一份 —— vCPU 任务被调度到哪颗核上跑, 宿主区里那份 TSS 就
> 得是哪颗核的。

> 不刷的后果见 vmcs_init_host() 里那段注释：非 BSP 核上的用户态陷阱会
> 用 cpu0 任务的栈, 两核同时写同一段栈 → 宿主进程随机崩。

> 代价是一条 str + 一次数组寻址 + 两条 vmwrite, 可以忽略。

**第 3031 行**

> 宿主 MSR 载入表(g_msr_host)里的值也必须是本核的。

> VM-exit 时硬件按这张表把宿主 MSR 装回去, 而 vmx_msr_lists_init() 只在
> VM 初始化时, 在调用者所在的核上抓过一次快照 —— helper 模式下那是
> /bin/vmm-run 的核, 而 vCPU 钉在 BSP 上。于是 SMP&gt;1 且 helper 落在另一颗
> 核时, 每次从 guest 出来宿主都被装上了别的核的 MSR；其中
> MSR_KERNEL_GS_BASE 就是那颗核的 per-CPU 指针(见 g_msr_list)。

> 宿主自己的 swapgs(SYSCALL 入口)随即把内核的 %gs 指到别的核上,
> task_current() 于是返回别的核的任务。实测连锁反应(SMP=2)：
> - execve 的 fd 继承把 idle 任务那张没初始化过的 fd_table 当父进程,
> 128 个 fd 池槽被一次吃光(之后宿主里所有 open() 都失败)；
> - execve 的 wrapper 把 idle/1 当自己退出掉 → 该核没有 idle → 整机卡死。
> SMP=1 时 helper 与 vCPU 同核, 快照恰好是对的, 所以一直没暴露。

**第 3052 行**

> `swapgs` 往返的另一半：guest 里的 swapgs 直接改硬件 MSR, VMM 看不见；
> VM-exit 的 store 表把它存进 g_msr_store, 这里搬进 entry-load 表
> (g_msr_guest), 下次进入 guest 时再装回去。

> 少了这一步, guest 的 swapgs 等于没执行：Linux `paranoid_entry` 用
> `rdmsr MSR_GS_BASE` 判断自己在哪个 GS 就会判错 → 异常死循环(实测卡在
> `Run /init as init process`, rip 恒为 paranoid_entry+0x93, reason 恒为
> RDMSR=31, 12 万次采样一动不动)。

**第 3065 行**

> GS_BASE 不能走 VMX 的 load 表(放进去 VM-entry 直接以 reason 34
> &quot;MSR loading&quot; 失败), 改走 GUEST_BASE_GS 字段 —— 效果一样：把
> exit-store 捞回来的, guest swapgs 之后的真实值装回去。

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:2995`

<a id="fn-kernel-vmm-x86_64-vmx.c-3072"></a>

### `vmm_arch_enter_guest`

```c
int vmm_arch_enter_guest(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

确保本核 VMXON，刷新宿主状态，头两次打印入口 VMCS 状态后执行 VMLAUNCH/VMRESUME，早期失败则读取并打印 VM-entry 错误码。

<details>
<summary>函数内备注（4 条）</summary>

**第 3074 行**

> 跑 vCPU 的这个 CPU 必须自己处于 VMX operation(VMXON 是每 CPU 一次
> 的)：helper 模式下做 VMXON 的是 /bin/vmm-run 所在的 CPU, 而 vCPU 任务
> 钉在 CPU0 —— 不补这一下, SMP&gt;1 时第一条 vmwrite 就 #UD。
> 幂等：只是一次按 CPU 的数组查表。

**第 3083 行**

> 注意： VMCS 的装载(含 vmcs_ready 那次初始化)不在这里 —— 它被提到了
> vmm_arch_restore_guest_ctx() 的最前面。见那边的注释：本函数之前会有
> 一次 vmcs_write()(中断注入), 那次写必须落在本 vCPU 的 VMCS 上。

**第 3090 行（行尾）**

> 入口状态：每次 VM 启动的头 2 次 entry 打印, 用来还原「第二次启动
> 时 VMCS 里的 guest 状态到底是什么」。计数是 per-vCPU 且随 VM 生命
> 周期清零 —— 见 g_entry_dbg_n 的注释。

**第 3118 行**

> vmlaunch/vmresume 早期失败。VMCS 此刻还装载着, 可以直接读出
> 具体原因 —— 没有这一行的话失败是完全静默的(vCPU 任务直接结束,
> 日志上看只剩「task started」然后就没了)。
> 编码见 Intel SDM Vol 3C 附录 C。

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:3072`

<a id="fn-kernel-vmm-x86_64-vmx.c-3133"></a>

### `vmm_arch_exit_handler`

```c
int vmm_arch_exit_handler(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

退出时自检本核 TR 是否等于期望 TSS 选择子，不符则每核告警一次，然后转 vmx_exit_handler。

<details>
<summary>函数内备注（2 条）</summary>

**第 3135 行**

> 不变量自检(每核最多报一次)：TR 此刻装的是这一次 VM-exit 从 VMCS
> 宿主区装回来的值。它必须等于本核 TSS 的选择子 —— x86_64 宿主的 IDT 门
> ist 全是 0(boot/x86_64/exception.c), ring3→ring0 一律靠 TSS.RSP0
> 换栈；装回了别的核的 TSS, 本核的用户态中断/异常就会把陷阱帧压进
> 那颗核当前任务的栈里。

> 放在退出路径上而不是入口：入口处 TR 还没被这次 VM-exit 覆盖,
> 看到的是上一轮的值, 抓不到&quot;刚被装回来的是什么&quot;。

**第 3154 行**

> 期望值走&quot;纯编码&quot;那条路, 不走写入用的 x86_tss_current()

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:3133`

<a id="fn-kernel-vmm-x86_64-vmx.c-3168"></a>

### `vmm_arch_save_guest_ctx`

```c
void vmm_arch_save_guest_ctx(vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，x86 的 VMCS 自动保存/恢复 guest 状态。

<details>
<summary>函数内备注（1 条）</summary>

**第 3171 行**

> x86 VMCS 自动保存/恢复, 无需手动操作

</details>

**位置**：`kernel/vmm/x86_64/vmx.c:3168`
