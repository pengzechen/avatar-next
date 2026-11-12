# lib — C API

[← 返回总览](index.md) · 25 个函数 · 说明覆盖率 100.0%

<a id="mod-lib-bitmap.c"></a>

## lib/bitmap.c — 位图管理实现

`lib/bitmap.c` · 3 个函数 · 3 个有说明 · 92 行

<a id="fn-lib-bitmap.c-19"></a>

### `bitmap_find_first_free`

```c
size_t bitmap_find_first_free(const bitmap_t *bitmap)
```

bitmap_find_first_free - 查找第一个空闲位
@bitmap: 位图结构指针

返回：第一个空闲位的索引, 失败返回 (size_t)-1

**位置**：`lib/bitmap.c:19`

<a id="fn-lib-bitmap.c-50"></a>

### `bitmap_find_contiguous_free_from`

```c
size_t bitmap_find_contiguous_free_from(const bitmap_t *bitmap, size_t count, size_t start)
```

bitmap_find_contiguous_free_from - 从 start 开始查找连续的空闲位

与 bitmap_find_contiguous_free 的区别：从 @start 起扫, 而不是每次都从 0。

为什么需要(实测)：PMM 用它分配物理页。低地址被内核和 rootfs 保留区
占着(约 66048 页), 而每次都从 0 开始找的话, 每分配一页都要白扫那 66048
位 —— 加载 38 MB 内核要 9437 次分配, 累计约 6 亿次位测试, TCG 下白白
多花近 2 秒(guest 自报启动 1.8s, 墙上却有 3.6s, 差额就是它)。

语义与调用方的回绕约定：只在 [start, size) 里找；调用方负责在失败时
从 0 再找一遍, 保证不会漏掉 start 之前的空洞。

<details>
<summary>函数内备注（2 条）</summary>

**第 56 行（行尾）**

> 起点太靠后, 交给调用方回绕

**第 63 行（行尾）**

> 跳过已占位, 别一位一位挪

</details>

**位置**：`lib/bitmap.c:50`

<a id="fn-lib-bitmap.c-74"></a>

### `bitmap_find_contiguous_free`

```c
size_t bitmap_find_contiguous_free(const bitmap_t *bitmap, size_t count)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从低到高扫描位图，返回第一段长度为 count 的全空闲位起点；参数非法或找不到返回 (size_t)-1。

<details>
<summary>函数内备注（1 条）</summary>

**第 88 行（行尾）**

> 找到足够的连续空闲位

</details>

**位置**：`lib/bitmap.c:74`

<a id="mod-lib-klog.c"></a>

## lib/klog.c — 内核日志系统实现

`lib/klog.c` · 11 个函数 · 11 个有说明 · 283 行

<a id="fn-lib-klog.c-51"></a>

### `klog_putchar`

```c
void klog_putchar(char c)
```

klog_putchar - 直接输出一个字符到 UART

不在此处加锁：调用者按整条消息加锁(klog_write), 避免每字符锁开销 +
多核穿插。除 klog_write 内部与 panic 路径外不要直接调它 —— 见 klog.h。

**位置**：`lib/klog.c:51`

<a id="fn-lib-klog.c-62"></a>

### `klog_write`

```c
void klog_write(const char *buf, size_t len)
```

klog_write - 往物理 UART 输出的唯一入口(见 include/klog.h)

整段持 g_klog_lock。所有写者(内核日志 / sys_write / tty_write / VMM 的
guest 控制台)都要经过它, 否则锁不住 —— 只锁自己那条路等于没锁。

<details>
<summary>函数内备注（2 条）</summary>

**第 69 行**

> 崩溃路径不取锁：panic 可能正发生在别的 CPU 持锁的时候, 取锁会死等。
> 这里的输出可能与别的核交错 —— 那是刻意接受的代价。

**第 79 行**

> 异步路径：只把消息 memcpy 进环形缓冲（几微秒，持 IRQ-off），
> UART 输出交给 klogd，在中断打开的状态下慢慢写。缓冲满/异步未启动
> 时退回下面的同步路径。

</details>

**位置**：`lib/klog.c:62`

<a id="fn-lib-klog.c-97"></a>

### `klog_flush`

```c
void klog_flush(void)
```

klog_flush - 空操作(保留用于兼容性)

<details>
<summary>函数内备注（1 条）</summary>

**第 99 行**

> 直接输出模式, 无需刷新

</details>

**位置**：`lib/klog.c:97`

<a id="fn-lib-klog.c-102"></a>

### `klog_panic_begin`

```c
void klog_panic_begin(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把全局 panic 标志置为真，供日志路径识别当前处于 panic 状态。

**位置**：`lib/klog.c:102`

<a id="fn-lib-klog.c-113"></a>

### `kvprintf`

```c
int kvprintf(const char *fmt, va_list va)
```

kvprintf - 格式化输出到内核日志

多核下整条消息持锁输出, 防止 `[INFO][C0] foo[C1] bar` 这种交错。
使用 IRQ-safe 锁, 避免中断处理也调用 klog 导致同核重入死锁。

<details>
<summary>函数内备注（2 条）</summary>

**第 121 行**

> my_vsnprintf 与 C99 snprintf 同语义：截断时返回&quot;本该写多长&quot;,
> 因此这里 len 可能大于缓冲区。必须 clamp —— 否则下面的循环会越界读
> 栈上 buf 之后的内容(返回地址, 保存的寄存器…)并原样打到串口。
> 单行超过 511 字节就会触发(长路径, 大 %s, 参数多的 trace)。

**第 131 行**

> 整行一次交出：取锁 / panic 判定都在 klog_write 里统一处理

</details>

**位置**：`lib/klog.c:113`

<a id="fn-lib-klog.c-155"></a>

### `klog_name_eq`

```c
static int klog_name_eq(const char *tok, int len, const char *name)
```

tok[0..len) 是否等于 name(tok 不是 NUL 结尾的, 来自逗号切分)

**位置**：`lib/klog.c:155`

<a id="fn-lib-klog.c-165"></a>

### `log_set_modules_by_name`

```c
int log_set_modules_by_name(const char *names)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐项解析逗号分隔的模块名（含空白、all/none 与空项处理），生成掩码写入全局，返回无法识别的名字个数。

<details>
<summary>函数内备注（2 条）</summary>

**第 183 行**

> 去掉两端空白, 允许 &quot;uart, gic&quot;

**第 192 行**

> 空项(如 &quot;uart,,gic&quot; 或结尾逗号)：忽略

</details>

**位置**：`lib/klog.c:165`

<a id="fn-lib-klog.c-220"></a>

### `klog_append`

```c
static void klog_append(char *out, int len, int *n, const char *s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在剩余空间内逐字符追加字符串并始终补 '\0'，超出容量即截断。

**位置**：`lib/klog.c:220`

<a id="fn-lib-klog.c-227"></a>

### `log_modules_to_string`

```c
int log_modules_to_string(char *out, int len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把全局模块掩码转成逗号分隔的名字写入 out，掩码为 0 输出 none，仅含表外位输出 unknown，返回写入长度。

<details>
<summary>函数内备注（1 条）</summary>

**第 249 行（行尾）**

> 掩码里只有表外的位

</details>

**位置**：`lib/klog.c:227`

<a id="fn-lib-klog.c-255"></a>

### `klog_init`

```c
void klog_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 LOG_MODULES_DEFAULT 设置模块掩码并报告无法识别的名字，随后打印当前日志级别与模块列表。

**位置**：`lib/klog.c:255`

<a id="fn-lib-klog.c-273"></a>

### `kprintf`

```c
int kprintf(const char *fmt, ...)
```

kprintf - 格式化输出到内核日志

**位置**：`lib/klog.c:273`

<a id="mod-lib-libc.c"></a>

## lib/libc.c

`lib/libc.c` · 2 个函数 · 2 个有说明 · 55 行

<a id="fn-lib-libc.c-10"></a>

### `atoi`

```c
int atoi(const char *s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

跳过前导空白和可选正负号后逐位累加十进制数字，空指针或无数字返回 0，不做溢出检查。

**位置**：`lib/libc.c:10`

<a id="fn-lib-libc.c-36"></a>

### `qsort`

```c
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 256 字节临时缓冲实现插入排序，数量不足 1、元素为 0 或元素过大时直接返回。

**位置**：`lib/libc.c:36`

<a id="mod-lib-string.c"></a>

## lib/string.c — 字符串/内存函数的外部符号版本

`lib/string.c` · 3 个函数 · 3 个有说明 · 41 行

绝大多数调用点是 include/string.h 里的 static inline 版本(会被内联,
或者退化成调用点自己的副本)。但编译器自己生成的调用 —— 大结构体整体
赋值, 循环→memcpy 变换等 —— 会直接发一个对本文件这些符号的引用：

0xc0c: bl memcpy

所以这里必须提供外部符号, 而且必须和 inline 版本走同一份实现
(string_internal.h 里的 *_arch), 否则就会出现"源码里写的 memcpy 是 NEON,
编译器生成的 memcpy 是逐字节循环"这种分裂 —— fork/clone 拷贝 816 字节
trap frame 正好踩过这个坑。

注意：本文件包含的是实现层头, 不是 string.h —— 后者的 static inline
memcpy/memset 与本文件要定义的外部符号同名, 同一翻译单元里内部链接与外部
链接同名是未定义行为(C11 6.2.2p7)。详见 include/string_internal.h 顶部。

<a id="fn-lib-string.c-28"></a>

### `memcpy`

```c
void *memcpy(void *dst, const void *src, size_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转调架构特化的 memcpy_arch 完成内存拷贝。

**位置**：`lib/string.c:28`

<a id="fn-lib-string.c-33"></a>

### `memset`

```c
void *memset(void *s, int c, size_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转调架构特化的 memset_arch 完成内存填充。

**位置**：`lib/string.c:33`

<a id="fn-lib-string.c-38"></a>

### `memmove`

```c
void *memmove(void *dst, const void *src, size_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

转调 memmove_generic 处理可能重叠的内存搬移。

**位置**：`lib/string.c:38`

<a id="mod-lib-vsnprintf.c"></a>

## lib/vsnprintf.c — Simple vsnprintf implementation for freestanding environment

`lib/vsnprintf.c` · 6 个函数 · 6 个有说明 · 260 行

<a id="fn-lib-vsnprintf.c-30"></a>

### `addchar`

```c
static void addchar(pstream_t *p, char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在剩余容量允许时写入一个字符并递减剩余量，同时始终累加已写计数。

**位置**：`lib/vsnprintf.c:30`

<a id="fn-lib-vsnprintf.c-39"></a>

### `print_str`

```c
static void print_str(pstream_t *p, const char *s, strprops_t props)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 npad 正负在字符串前用 pad 字符左填充，或在字符串后用空格右填充（负宽度时应忽略 0 填充）。

<details>
<summary>函数内备注（1 条）</summary>

**第 56 行（行尾）**

> ignore &#x27;0&#x27; flag with &#x27;-&#x27; flag

</details>

**位置**：`lib/vsnprintf.c:39`

<a id="fn-lib-vsnprintf.c-65"></a>

### `print_int`

```c
static void print_int(pstream_t *ps, long long n, int base, strprops_t props)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把有符号数按指定进制转成数字串，处理负号与零值并反转，再交给 print_str 带填充输出。

**位置**：`lib/vsnprintf.c:65`

<a id="fn-lib-vsnprintf.c-99"></a>

### `print_unsigned`

```c
static void print_unsigned(pstream_t *ps, unsigned long long n, int base, strprops_t props)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把无符号数转成指定进制字符串，alternate 且十六进制时补 0x（0 填充时提前输出前缀并修正宽度），反转后经 print_str 输出。

**位置**：`lib/vsnprintf.c:99`

<a id="fn-lib-vsnprintf.c-137"></a>

### `fmtnum`

```c
static int fmtnum(const char **fmt)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析格式串中可选的负号与数字宽度，用 atol 取值并按已消费长度前移格式指针。

**位置**：`lib/vsnprintf.c:137`

<a id="fn-lib-vsnprintf.c-153"></a>

### `my_vsnprintf`

```c
int my_vsnprintf(char *buf, int size, const char *fmt, va_list va)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环解析格式串，处理 %% %c %d %u %x %p %s 及宽度、0/-/# 与 l/z/t 修饰，写入缓冲区并返回总输出字符数。

<details>
<summary>函数内备注（2 条）</summary>

**第 189 行**

> fall through

**第 200 行**

> Here we only care that sizeof(size_t) == sizeof(long).
> On a 32-bit platform it doesn&#x27;t matter that size_t is
> typedef&#x27;ed to int or long; va_arg will work either way.
> Same for ptrdiff_t (%td).

</details>

**位置**：`lib/vsnprintf.c:153`
