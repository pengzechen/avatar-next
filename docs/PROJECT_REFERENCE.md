# Avatar OS 项目参考文档

## 目录

1. [项目概述](#项目概述)
2. [架构设计](#架构设计)
3. [构建系统](#构建系统)
4. [核心模块详解](#核心模块详解)
5. [架构抽象层](#架构抽象层)
6. [开发环境配置](#开发环境配置)
7. [编程规范](#编程规范)
8. [测试与调试](#测试与调试)
9. [性能优化](#性能优化)
10. [常见问题](#常见问题)

---

## 项目概述

### 项目定位

Avatar OS 是一个**64位操作系统内核**项目，专注于**跨架构支持**和**底层抽象设计**。项目遵循 Linux 内核的设计理念，提供了完整的底层基础组件。

### 核心特性

- **跨架构支持**：AArch64 (ARM 64-bit)、RISC-V 64-bit、x86_64 (AMD64/Intel 64)
- **Freestanding 环境**：不依赖标准库，完全自包含实现
- **零成本抽象**：编译时配置，运行时零开销
- **类型安全**：利用编译器扩展实现类型安全的宏
- **内存安全**：集成内存屏障的同步和I/O操作

### 技术栈

- **编译器**：GCC (交叉编译 musl 工具链)
- **汇编**：内联汇编实现关键操作
- **开发环境**：VSCode + IntelliSense
- **版本控制**：Git
- **构建工具**：Make

---

## 架构设计

### 分层架构

```
┌─────────────────────────────────────┐
│     应用层（未来实现）              │
├─────────────────────────────────────┤
│     内核层（Kernel/）               │
│   - 任务管理                        │
│   - 内存管理                        │
│   - 驱动框架                        │
├─────────────────────────────────────┤
│     基础设施层（include/）          │
│   - 同步原语（spinlock）            │
│   - 内存屏障（barrier）              │
│   - 日志系统（klog）                │
│   - 断言系统（assert）              │
│   - 数据结构（list）                │
├─────────────────────────────────────┤
│     架构抽象层（arch/）              │
│   - AArch64 实现                    │
│   - RISC-V 实现                     │
│   - x86_64 实现                     │
├─────────────────────────────────────┤
│     硬件层                          │
│   - CPU                             │
│   - 内存                            │
│   - 设备                            │
└─────────────────────────────────────┘
```

### 模块依赖关系

```
types.h (基础类型)
    ↓
arg.h (可变参数)
    ↓
arch.h (架构检测)
    ↓
barrier.h (内存屏障)
    ↓
spinlock.h (自旋锁) ← mmio.h (内存映射I/O)
    ↓                   ↓
klog.h (日志系统) ────┘
    ↓
list.h (链表)
    ↓
assert.h (断言系统)
    ↓
string.h (字符串操作)
cache.h (缓存操作)
```

### 设计原则

#### 1. **内联优先原则**

所有头文件中的函数必须是 `static inline`，避免链接错误。

```c
/* ✅ 正确 */
static inline void operation(void) {
    // 实现
}

/* ❌ 错误 */
extern void operation(void);
void operation(void) { /* 实现 */ }
```

**原因**：Freestanding 环境下，每个编译单元独立编译，extern 函数会导致多重定义。

#### 2. **架构抽象模式**

统一接口 + 架构特化实现：

```c
/* module.h */
#ifndef MODULE_H
#define MODULE_H

#include "arch.h"

// 声明通用接口
static inline void common_operation(void);

// 包含架构特定实现
#if defined(__aarch64__)
    #include "aarch64/module_impl.h"
#elif defined(__x86_64__)
    #include "x86_64/module_impl.h"
#elif defined(__riscv)
    #include "riscv64/module_impl.h"
#endif

#endif
```

```c
/* aarch64/module_impl.h */
static inline void common_operation(void) {
    __asm__ volatile("/* AArch64 特定指令 */" ::: "memory");
}
```

#### 3. **编译时优化**

通过宏参数控制功能，实现零开销：

```bash
# 开发模式：完整日志和断言
make LOG=debug ASSERT=panic

# 发布模式：零日志开销，零断言开销
make LOG=none ASSERT=off
```

#### 4. **类型安全宏**

使用 GCC 的 `__typeof__` 扩展：

```c
#define MIN(a, b) __extension__ ({            \
    __typeof__(a) _a = (a);                   \
    __typeof__(b) _b = (b);                   \
    _a < _b ? _a : _b;                        \
})
```

**好处**：
- 避免多次求值副作用
- 类型检查
- 支持不同类型比较

---

## 构建系统

### Makefile 参数

#### 架构选择

```bash
ARCH=aarch64   # ARM 64-bit（默认）
ARCH=riscv64   # RISC-V 64-bit
ARCH=x86_64    # AMD64/Intel 64
```

#### 日志级别

```bash
LOG=none       # 关闭所有日志
LOG=error      # 只显示错误
LOG=warn       # 警告和错误
LOG=info       # 信息及以上（默认）
LOG=debug      # 调试及以上
LOG=trace      # 所有日志
```

#### 断言控制

```bash
ASSERT=panic   # 启用断言，失败时 panic（默认）
ASSERT=off     # 禁用断言（零开销）
```

### 编译流程

```
Makefile
    ↓
1. 解析 ARCH/LOG/ASSERT 参数
    ↓
2. 选择工具链和编译标志
    ↓
3. 编译源文件（tests/*.c, lib/*.c）
    ↓
4. 生成静态库（.a 文件）
    ↓
5. 链接（如果需要）
```

### 工具链配置

| 架构 | CC 编译器 | AR 归档器 | 特殊标志 |
|------|----------|----------|---------|
| AArch64 | `aarch64-linux-musl-gcc` | `aarch64-linux-musl-ar` | `-D__aarch64__` |
| RISC-V | `riscv64-linux-musl-gcc` | `riscv64-linux-musl-ar` | `-march=rv64gc -mabi=lp64 -D__riscv -D__riscv_xlen=64` |
| x86_64 | `gcc` | `ar` | `-D__x86_64__` |

### 编译标志详解

```bash
-Wall -Wextra -O2 -g          # 基本标志：警告、优化、调试信息
-nostdinc                     # 不使用标准库头文件
-Iinclude                     # 主头文件路径
-Iinclude/<arch>              # 架构特定头文件路径
-D<arch_macro>                # 架构宏定义
-DLOG_LEVEL=<level>           # 日志级别定义
-DASSERT_OFF (可选)           # 禁用断言
-MMD -MP                      # 生成依赖文件
```

### 输出文件

```
build/
├── spinlock_<arch>.a         # 主库（包含测试）
│   ├── klog_test.o
│   ├── list_test.o
│   ├── spinlock_test.o
│   ├── string_test.o
│   └── assert_test.o
└── libklog_<arch>.a          # 日志库
    └── klog.o
```

### 常用构建命令

```bash
# 完整重建
make ARCH=aarch64 clean && make ARCH=aarch64

# 调试版本
make ARCH=aarch64 LOG=debug ASSERT=panic

# 发布版本
make ARCH=aarch64 LOG=none ASSERT=off

# 测试所有架构
for arch in aarch64 riscv64 x86_64; do
    make ARCH=$arch clean && make ARCH=$arch
done

# 查看帮助
make help
```

---

## 核心模块详解

### 1. 类型系统 (types.h)

#### 基础整数类型

```c
// 无符号类型
typedef unsigned char        uint8_t;
typedef unsigned short       uint16_t;
typedef unsigned int         uint32_t;
typedef unsigned long long   uint64_t;

// 有符号类型
typedef char        int8_t;
typedef short       int16_t;
typedef int         int32_t;
typedef long long   int64_t;
```

#### 指针相关类型

```c
typedef unsigned long size_t;        // 64位系统正确类型
typedef long          ssize_t;       // 有符号 size
typedef unsigned long uintptr_t;     // 指针到整数
typedef long          intptr_t;      // 有符号指针
typedef long          ptrdiff_t;     // 指针差值
```

**注意**：`size_t` 在 64 位系统上是 `unsigned long`，不是 `unsigned long long`，这符合 System V AMD64 ABI 和 AArch64 AAPCS64 规范。

#### 地址类型

```c
typedef uint64_t vaddr_t;  // 虚拟地址
typedef uint64_t paddr_t;  // 物理地址
```

#### 类型限制

```c
#define INT8_MAX   (127)
#define INT8_MIN   (-128)
#define UINT8_MAX  (255U)

#define INT64_MAX  (9223372036854775807LL)
#define INT64_MIN  (-9223372036854775808LL)
#define UINT64_MAX (18446744073709551615ULL)

#define SIZE_MAX   (18446744073709551615UL)
```

#### 实用宏

##### 数组和结构体

```c
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define offsetof(type, member) __builtin_offsetof(type, member)

#define container_of(ptr, type, member) __extension__ ({              \
    const __typeof__(((type *)0)->member) *__mptr = (ptr);           \
    (type *)((char *)__mptr - offsetof(type, member));               \
})
```

**使用示例**：

```c
struct task {
    int id;
    struct list_head list;
};

struct list_head *ptr = &task->list;
struct task *t = container_of(ptr, struct task, list);
```

##### 类型安全的 MIN/MAX

```c
#define MIN(a, b) __extension__ ({                                    \
    __typeof__(a) _a = (a);                                           \
    __typeof__(b) _b = (b);                                           \
    _a < _b ? _a : _b;                                                \
})

#define MAX(a, b) __extension__ ({                                    \
    __typeof__(a) _a = (a);                                           \
    __typeof__(b) _b = (b);                                           \
    _a > _b ? _a : _b;                                                \
})
```

**好处**：避免多次求值问题

```c
int x = 1, y = 2;
int z = MIN(x++, y);  // x 只递增一次（正确的）
// 不安全的版本：((x++) < (y) ? (x++) : (y))  // x 递增两次！
```

##### 位操作

```c
#define BIT(n)  (1UL << (n))
#define BIT64(n) (1ULL << (n))

#define SET_BIT(mask, bit)   ((mask) |= BIT(bit))
#define CLEAR_BIT(mask, bit) ((mask) &= ~BIT(bit))
#define TEST_BIT(mask, bit)  (!!((mask) & BIT(bit)))
```

##### 对齐操作

```c
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)
```

**使用示例**：

```c
size_t addr = 0x1234;
size_t aligned = ALIGN_UP(addr, 4096);  // 页对齐
bool is_page_aligned = IS_ALIGNED(addr, 4096);
```

##### 其他工具

```c
#define IS_POWER_OF_TWO(x) (((x) != 0) && (((x) & ((x) - 1)) == 0))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)
```

##### 编译器属性

```c
#define PACKED __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))
#define NORETURN __attribute__((noreturn))
#define UNUSED __attribute__((unused))
#define WEAK __attribute__((weak))
#define ALIAS(alias) __attribute__((alias(#alias)))
```

### 2. 内存屏障 (barrier.h)

#### 为什么需要内存屏障？

**问题**：CPU 和编译器会重排指令以优化性能，但这可能导致多核同步问题。

```c
// 程序员写的代码
data = 42;
ready = 1;

// CPU 可能重排为（有问题！）
ready = 1;
data = 42;
```

**解决**：使用内存屏障强制执行顺序。

#### 屏障类型

##### 1. 编译器屏障

```c
#define barrier_compiler() asm volatile("" ::: "memory")
```

**作用**：告诉编译器不要优化跨越此点的内存访问，但不生成 CPU 指令。

##### 2. 数据内存屏障 (mb)

```c
static inline void barrier_data(void);
```

**作用**：确保之前的所有内存访问完成后，才执行后续的内存访问。

##### 3. 读屏障 (rmb)

```c
static inline void barrier_data_read(void);
```

**作用**：确保之前的读操作完成后，才执行后续的读操作。

##### 4. 写屏障 (wmb)

```c
static inline void barrier_data_write(void);
```

**作用**：确保之前的写操作完成后，才执行后续的写操作。

##### 5. 指令同步屏障

```c
static inline void barrier_instr_full(void);
```

**作用**：刷新流水线，确保之前的指令都执行完成。

##### 6. 获取/释放屏障

```c
static inline void barrier_acquire(void);  // 获取语义
static inline void barrier_release(void);  // 释放语义
```

**使用场景**：锁实现中的释放-获取模式。

#### 架构实现对比

| 操作 | AArch64 | RISC-V | x86_64 |
|------|---------|--------|--------|
| 编译器屏障 | `"" ::: "memory"` | `"" ::: "memory"` | `"" ::: "memory"` |
| 数据屏障 | `dmb ish` | `fence rw, rw` | `mfence` |
| 读屏障 | `dmb ishld` | `fence r, r` | `lfence` |
| 写屏障 | `dmb ishst` | `fence w, w` | `sfence` |
| 指令屏障 | `isb` | `fence.i` | - (不常用) |

#### 使用示例

```c
// 生产者-消费者模式
int data = 0;
bool ready = false;

// 生产者
data = 42;
barrier_data_write();  // 确保数据先写入
ready = true;

// 消费者
if (ready) {
    barrier_data_read();  // 确保读取 ready 后再读取 data
    int value = data;     // value 一定是 42
}
```

### 3. 自旋锁 (spinlock.h)

#### 数据结构

```c
typedef struct {
    volatile uint32_t lock;
} spinlock_t;

typedef struct {
    volatile uint32_t lock;
} spinlock_noirq_t;
```

**区别**：
- `spinlock_t`：普通自旋锁
- `spinlock_noirq_t`：带中断禁用的自旋锁（用于中断处理程序）

#### 基本操作

```c
spinlock_t my_lock = SPINLOCK_INIT;

spin_lock(&my_lock);
    // 临界区
spin_unlock(&my_lock);
```

#### 中断上下文安全

```c
unsigned long flags;

spin_lock_irqsave(&lock, flags);
    // 临界区（中断已禁用）
spin_unlock_irqrestore(&lock, flags);
```

**为什么需要 `irqsave`？**

```c
// 不安全的例子（可能死锁）
void interrupt_handler(void) {
    spin_lock(&lock);  // 如果中断发生在持有 lock 的内核代码中 → 死锁！
}

// 安全的例子
void interrupt_handler(void) {
    unsigned long flags;
    spin_lock_irqsave(&lock, flags);  // 禁用中断，避免死锁
    // ...
    spin_unlock_irqrestore(&lock, flags);
}
```

#### 架构实现

##### AArch64: LDAXR/STLXR

```c
static inline void spin_lock(spinlock_t *lock)
{
    uint32_t tmp;
    __asm__ volatile(
        "   sevl                   \n"
        "1: wfe                    \n"
        "2: ldaxr %w0, [%1]       \n"  // Load-Acquire Exclusive
        "   cbnz %w0, 1b          \n"
        "   stxr %w0, %w2, [%1]   \n"  // Store-Release Exclusive
        "   cbnz %w0, 2b          \n"
        : "=&r"(tmp)
        : "r"(&lock->lock), "r"(1)
        : "memory"
    );
}
```

**关键字**：
- `ldaxr`：Load-Acquire Exclusive，确保读取操作不会被重排到其后
- `stxr`：Store-Release Exclusive，确保写入操作不会被重排到其前
- `wfe`：Wait For Event，降低功耗

##### RISC-V: LR/SC

```c
static inline void spin_lock(spinlock_t *lock)
{
    uint32_t tmp;
    __asm__ volatile(
        "1: lr.w %0, (%1)        \n"  // Load-Reserved
        "   bnez %0, 1b          \n"
        "   li %0, 1             \n"
        "   sc.w %0, %0, (%1)    \n"  // Store-Conditional
        "   bnez %0, 1b          \n"
        : "=&r"(tmp)
        : "r"(&lock->lock)
        : "memory"
    );
}
```

**关键字**：
- `lr.w`：Load-Reserved，标记内存位置
- `sc.w`：Store-Conditional，只有在该位置未被修改时才成功

##### x86_64: xchg

```c
static inline void spin_lock(spinlock_t *lock)
{
    __asm__ volatile(
        "   lock xchg %0, %1    \n"
        : "+r"(1), "+m"(lock->lock)
        :: "memory"
    );
}
```

**关键字**：
- `lock xchg`：原子交换，x86_64 提供强内存排序保证

### 4. 内核日志 (klog.h)

#### 日志级别

| 级别 | 值 | 显示内容 | 颜色 | 使用场景 |
|------|---|---------|------|---------|
| NONE | 0 | 关闭所有日志 | - | 发布版本 |
| ERROR | 1 | 只显示错误 | 红色 | 严重错误 |
| WARN | 2 | 警告和错误 | 黄色 | 潜在问题 |
| INFO | 3 | 信息及以上 | 绿色 | 重要事件（默认） |
| DEBUG | 4 | 调试及以上 | 蓝色 | 调试信息 |
| TRACE | 5 | 所有日志 | 无色 | 详细跟踪 |

#### 基础日志宏

```c
KLOG_ERROR("Critical error: %s", msg);     // 总是显示
KLOG_WARN("Warning: value=%d", value);     // WARN 级别及以上
KLOG_INFO("System initialized");            // INFO 级别及以上
KLOG_DEBUG("Buffer at %p, size=%d", ptr);  // DEBUG 级别及以上
KLOG_TRACE("Function: %s", __func__);       // TRACE 级别
```

#### 模块级日志

##### 启用模块

```c
// 启用 UART 和 TIMER 模块
log_set_modules(LOG_MODULE_UART | LOG_MODULE_TIMER);

// 添加单个模块
log_add_module(LOG_MODULE_DRIVER);

// 移除模块
log_remove_module(LOG_MODULE_FS);
```

##### 使用模块日志

```c
// 方式 1：使用预定义的模块宏
KLOG_UART("UART initialized at %d baud", baud_rate);
KLOG_TIMER("Timer tick: %d", tick);
KLOG_DRIVER("Device %s registered", dev_name);

// 方式 2：自定义模块日志
KLOG_MODULE_DEBUG(LOG_MODULE_INIT, "CPU %d initialized", cpu_id);
KLOG_MODULE_TRACE(LOG_MODULE_TASK, "Task switch: %d -> %d", from, to);
```

##### 内置模块

```c
#define LOG_MODULE_INIT    (1ULL << 0)  // 初始化模块
#define LOG_MODULE_TASK    (1ULL << 1)  // 任务调度模块
#define LOG_MODULE_DRIVER  (1ULL << 2)  // 驱动模块
#define LOG_MODULE_UART    (1ULL << 3)  // UART 驱动
#define LOG_MODULE_TIMER   (1ULL << 4)  // 定时器模块
#define LOG_MODULE_MM      (1ULL << 5)  // 内存管理模块
#define LOG_MODULE_FS      (1ULL << 6)  // 文件系统模块
#define LOG_MODULE_NET     (1ULL << 7)  // 网络模块
#define LOG_MODULE_SMP     (1ULL << 8)  // 多核模块
```

**注意**：支持最多 64 个模块（位 0-63）。

#### 兼容性宏

```c
// Linux 内核风格
pr_err(fmt, ...)    // 等同于 KLOG_ERROR
pr_warn(fmt, ...)   // 等同于 KLOG_WARN
pr_info(fmt, ...)   // 等同于 KLOG_INFO
pr_debug(fmt, ...)  // 等同于 KLOG_DEBUG

// printk 别名
printk(fmt, ...)    // 等同于 kprintf
```

#### 平台集成

需要实现以下函数：

```c
// 在 platform.c 中
void uart_putchar(char c) {
    // 向 UART 写入一个字符
}

void uart_putstr(const char *str) {
    while (*str) {
        uart_putchar(*str++);
    }
}
```

#### 日志格式

```
[INFO ] file.c:42: This is an info message
[ERROR] file.c:123: Critical error: test error
[WARN ] file.c:456: Warning: value out of range
[MOD  ] file.c:789: UART initialized at 115200 baud
```

### 5. 双向链表 (list.h)

#### 数据结构

```c
struct list_head {
    struct list_head *next, *prev;
};
```

#### 初始化

```c
// 静态初始化
#define LIST_HEAD(name) \
    struct list_head name = { &(name), &(name) }

// 动态初始化
struct list_head my_list;
INIT_LIST_HEAD(&my_list);
```

#### 基本操作

```c
// 添加到链表头部（栈行为）
list_add(&node->list, &my_list);

// 添加到链表尾部（队列行为）
list_add_tail(&node->list, &my_list);

// 删除节点
list_del(&node->list);

// 检查是否为空
if (list_empty(&my_list)) {
    // 链表为空
}
```

#### 迭代

```c
struct list_head *pos;

// 正向迭代
list_for_each(pos, &my_list) {
    // pos 指向当前节点的 list 成员
}

// 安全迭代（允许删除）
list_for_each_safe(pos, n, &my_list) {
    // 可以安全地删除 pos
    list_del(pos);
}
```

#### 容器获取

```c
struct task {
    int id;
    struct list_head list;
};

struct list_head *ptr = &task->list;
struct task *t = list_entry(ptr, struct task, list);
```

#### 完整示例

```c
struct task {
    int id;
    const char *name;
    struct list_head list;
};

LIST_HEAD(task_list);

// 添加任务
struct task *t1 = malloc(sizeof(struct task));
t1->id = 1;
t1->name = "Task 1";
INIT_LIST_HEAD(&t1->list);
list_add(&t1->list, &task_list);

// 迭代任务
struct task *pos;
list_for_each_entry(pos, &task_list, list) {
    KLOG_INFO("Task: %d - %s", pos->id, pos->name);
}

// 删除任务
list_del(&t1->list);
free(t1);
```

### 6. 内存映射 I/O (mmio.h)

#### 为什么需要 MMIO 包装？

**问题**：直接读写内存映射的设备寄存器可能导致：
- 编译器优化掉必要的读写
- CPU 重排内存访问
- 缓存一致性问题

**解决**：使用带内存屏障的包装函数。

#### 基本读写

```c
// 8 位读写
uint8_t val8 = mmio_read8(addr);
mmio_write8(addr, val8);

// 16 位读写
uint16_t val16 = mmio_read16(addr);
mmio_write16(addr, val16);

// 32 位读写
uint32_t val32 = mmio_read32(addr);
mmio_write32(addr, val32);

// 64 位读写
uint64_t val64 = mmio_read64(addr);
mmio_write64(addr, val64);
```

#### 位操作

```c
// 设置位
mmio_setbits32(addr, BIT(0) | BIT(5));

// 清除位
mmio_clrbits32(addr, BIT(3));

// 修改位（先清后设）
mmio_clearbits32(addr, 0xFF);
mmio_setbits32(addr, 0x42);
```

#### 类型化读写

```c
// 读取特定类型
uint32_t status = mmio_readtype(addr, uint32_t);

// 写入特定类型
mmio_writetype(addr, 0xDEADBEEF, uint32_t);
```

#### 内存屏障集成

```c
// 读操作包含获取屏障（acquire）
static inline uint32_t mmio_read32(volatile void *addr) {
    uint32_t val = *(__volatile uint32_t *)addr;
    barrier_acquire();  // 防止后续操作被重排到读取之前
    return val;
}

// 写操作包含释放屏障（release）
static inline void mmio_write32(volatile void *addr, uint32_t val) {
    barrier_release();  // 确保之前的操作完成
    *(__volatile uint32_t *)addr = val;
}
```

#### 使用示例

```c
// UART 寄存器
#define UART_BASE 0x09000000
#define UART_FR    0x18   // Flag register
#define UART_DR    0x00   // Data register

// 检查发送 FIFO 是否为空
if (!(mmio_read32(UART_BASE + UART_FR) & BIT(5))) {
    // FIFO 不满，可以写入
    mmio_write32(UART_BASE + UART_DR, 'A');
}

// 设置位
mmio_setbits32(UART_BASE + UART_CR, BIT(0));  // 使能 UART
```

### 7. 断言系统 (assert.h)

#### 断言类型

##### 1. 可禁用断言

```c
assert(ptr != NULL);
assert(x > 0);
```

**行为**：
- `ASSERT=panic`：启用，失败时 panic
- `ASSERT=off`：完全禁用，零开销

##### 2. 总是启用的断言

```c
assert_always(ptr != NULL);
assert_always(critical_flag);
```

**行为**：即使 `ASSERT=off` 仍然检查。

**使用场景**：关键检查，如安全验证、空指针检查。

##### 3. 编译时断言

```c
static_assert(sizeof(int) == 4, "int must be 4 bytes");
static_assert(sizeof(void *) == 8, "must be 64-bit");
```

**行为**：编译时检查，如果失败则编译报错。

##### 4. 辅助宏

```c
// 声明代码不应到达这里
assert_not_reached();

// 声明表达式不可能为真
assert_unreachable(error_code);
```

#### Panic 行为

```
断言失败
    ↓
KLOG_ERROR 输出错误信息
    ↓
调用 platform_panic()
    ↓
调用 arch_halt()
    ↓
CPU 停止（WFI/HLT）
```

#### 平台集成

```c
// platform.c
#if defined(__aarch64__)
    #include "aarch64/halt_arch.h"
#elif defined(__x86_64__)
    #include "x86_64/halt_arch.h"
#elif defined(__riscv)
    #include "riscv64/halt_arch.h"
#endif

void platform_panic(void) {
    // 平台特定的清理
    klog_flush();

    // 调用架构特定的 halt
    arch_halt();
}
```

#### 使用示例

```c
// 指针检查
void init_device(struct device *dev) {
    assert(dev != NULL);
    assert(dev->base_addr != 0);
    assert(IS_ALIGNED(dev->base_addr, PAGE_SIZE));
}

// 数组边界
void buffer_write(int *buf, size_t index, int value) {
    assert(index < BUFFER_SIZE);
    buf[index] = value;
}

// 状态机
int parse_state(int state) {
    switch (state) {
        case STATE_INIT:
            return 0;
        case STATE_RUNNING:
            return 1;
        default:
            assert_unreachable(state);
    }
}
```

---

## 架构抽象层

### 架构检测

```c
#if defined(__aarch64__)
    #define ARCH_AARCH64 1
    #define ARCH_NAME "aarch64"
#elif defined(__x86_64__)
    #define ARCH_X86_64 1
    #define ARCH_NAME "x86_64"
#elif defined(__riscv)
    #define ARCH_RISCV64 1
    #define ARCH_NAME "riscv64"
#else
    #error "Unsupported architecture"
#endif
```

### 架构特定文件

```
include/
├── aarch64/
│   ├── barrier_impl.h       # DMB/DSB/ISB 实现
│   ├── cache_impl.h         # DC CVAC/IVAC 实现
│   ├── cpu.h                # CPU 定义
│   ├── halt_arch.h          # WFI halt
│   ├── spin_lock_impl.h     # LDAXR/STLXR 实现
│   └── string_impl.h        # NEON memcpy
├── riscv64/
│   ├── barrier_impl.h       # fence 实现
│   ├── cache_impl.h         # CBO 实现
│   ├── halt_arch.h          # WFI halt
│   ├── spin_lock_impl.h     # LR/SC 实现
│   └── string_impl.h        # 标准 memcpy
└── x86_64/
    ├── barrier_impl.h       # mfence 实现
    ├── cache_impl.h         # clflush 实现
    ├── halt_arch.h          # HLT halt
    ├── spin_lock_impl.h     # xchg 实现
    └── string_impl.h        # rep movs memcpy
```

### 添加新架构支持

#### 1. 添加架构检测

在 `include/arch.h` 中添加：

```c
#elif defined(__new_arch__)
    #define ARCH_NEW_ARCH 1
    #define ARCH_NAME "new_arch"
```

#### 2. 创建目录

```bash
mkdir -p include/new_arch
```

#### 3. 实现架构特定文件

创建 `include/new_arch/barrier_impl.h`：

```c
static inline void barrier_data(void) {
    __asm__ volatile("/* 架构特定指令 */" ::: "memory");
}
```

#### 4. 更新 Makefile

添加工具链配置：

```makefile
else ifeq ($(ARCH),new_arch)
    CC      := new_arch-linux-musl-gcc
    AR      := new_arch-linux-musl-ar
    CFLAGS  += -D__new_arch__
    CFLAGS  += -I$(INCLUDE_DIR)/new_arch
    TARGET  := $(BUILD_DIR)/spinlock_new_arch.a
```

---

## 开发环境配置

### VSCode 配置

#### c_cpp_properties.json

```json
{
  "configurations": [
    {
      "name": "AArch64",
      "includePath": [
        "${workspaceFolder}/include",
        "${workspaceFolder}/include/aarch64"
      ],
      "defines": ["__aarch64__"],
      "compilerPath": "/usr/bin/aarch64-linux-musl-gcc"
    },
    {
      "name": "RISC-V",
      "includePath": [
        "${workspaceFolder}/include",
        "${workspaceFolder}/include/riscv64"
      ],
      "defines": ["__riscv", "__riscv_xlen=64"],
      "compilerPath": "/usr/bin/riscv64-linux-musl-gcc"
    },
    {
      "name": "x86_64",
      "includePath": [
        "${workspaceFolder}/include",
        "${workspaceFolder}/include/x86_64"
      ],
      "defines": ["__x86_64__"],
      "compilerPath": "/usr/bin/gcc"
    }
  ],
  "default": {
    "name": "AArch64"
  }
}
```

#### 切换架构

方法 1：使用状态栏
1. 点击状态栏的架构名称
2. 选择目标架构

方法 2：使用命令面板
1. `Ctrl+Shift+P`
2. 输入 "C/C++: Select a Configuration"
3. 选择目标架构

### 工具链安装

#### AArch64

```bash
# Ubuntu/Debian
sudo apt-get install gcc-aarch64-linux-gnu

# musl 工具链（推荐）
wget https://musl.cc/aarch64-linux-musl-cross.tgz
tar xf aarch64-linux-musl-cross.tgz
export PATH=$PATH:$(pwd)/aarch64-linux-musl-cross/bin
```

#### RISC-V

```bash
# Ubuntu/Debian
sudo apt-get install gcc-riscv64-linux-gnu

# musl 工具链
wget https://musl.cc/riscv64-linux-musl-cross.tgz
tar xf riscv64-linux-musl-cross.tgz
export PATH=$PATH:$(pwd)/riscv64-linux-musl-cross/bin
```

#### x86_64

```bash
# 系统自带
sudo apt-get install gcc make
```

---

## 编程规范

### 命名约定

#### 函数

```c
module_operation()          // 模块_操作
spin_lock()                 // 使用下划线
klog_putchar()              // 模块名_操作名
```

#### 宏

```c
#define MODULE_DEFINE        // 全大写
#define MAX(a, b)            // 全大写
#define LOG_MODULE_UART      // 模块_类型_名称
```

#### 类型

```c
typedef struct {
    // ...
} module_t;                 // 小写_下划线 _t 后缀
```

#### 变量

```c
int local_var;              // 小写下划线
static uint64_t g_counter;  // 全局变量加 g_ 前缀
```

### 代码风格

#### 缩进

```c
// 使用 4 空格缩进
if (condition) {
    do_something();
}
```

#### 大括号

```c
// 左大括号不换行
if (condition) {
    // ...
}

// 函数同样
void function(void)
{
    // ...
}
```

#### 注释

```c
/**
 * function_name - 简短描述
 * @param param1: 参数1描述
 * @return 返回值描述
 *
 * 详细说明（可选）
 */
static inline int function_name(int param1)
{
    // 实现
}
```

### 头文件组织

```c
#ifndef MODULE_H
#define MODULE_H

/*
 * 模块描述
 * 详细说明
 */

// 1. 包含其他头文件
#include "types.h"
#include "arch.h"

// 2. 定义常量
#define MODULE_CONST  100

// 3. 类型定义
typedef struct {
    // ...
} module_t;

// 4. 函数声明
static inline void module_operation(void);

// 5. 包含架构实现
#if defined(__aarch64__)
    #include "aarch64/module_impl.h"
// ... 其他架构
#endif

#endif  // MODULE_H
```

---

## 测试与调试

### 测试文件结构

```
tests/
├── spinlock_test.c    // 自旋锁测试
├── string_test.c      // 字符串测试
├── list_test.c        // 链表测试
├── klog_test.c        // 日志测试
└── assert_test.c      // 断言测试
```

### 编写测试

```c
#include "module.h"

void test_basic_operation(void) {
    // 准备
    int result = module_init();

    // 断言
    assert(result == 0);

    // 清理
    module_cleanup();
}

void run_all_tests(void) {
    KLOG_INFO("=== Module Tests ===");

    test_basic_operation();

    KLOG_INFO("=== Tests Complete ===");
}
```

### 运行测试

```bash
# 编译测试
make ARCH=aarch64 LOG=debug

# 查看生成的库
ls -lh build/*.a
```

### 调试技巧

#### 1. 使用日志

```c
KLOG_DEBUG("Entering function %s", __func__);
KLOG_DEBUG("Value: %d", value);
```

#### 2. 使用断言

```c
assert(condition);  // 失败时显示文件和行号
```

#### 3. 使用模块日志

```c
log_set_modules(LOG_MODULE_UART);
KLOG_UART("Debug message");
```

---

## 性能优化

### 编译时优化

```bash
# 发布版本
make LOG=none ASSERT=off  # 移除所有日志和断言
```

### 代码优化

#### 1. 使用内联函数

```c
// ✅ 好（内联）
static inline int fast_operation(void) {
    return x + y;
}

// ❌ 差（函数调用开销）
int fast_operation(void);
```

#### 2. 避免不必要的屏障

```c
// ✅ 好（只在需要时使用屏障）
spin_lock(&lock);
data = 42;
spin_unlock(&lock);

// ❌ 差（过度使用屏障）
barrier_data();
spin_lock(&lock);
barrier_data();
data = 42;
barrier_data();
spin_unlock(&lock);
barrier_data();
```

#### 3. 使用编译器内置函数

```c
// ✅ 好（使用内置函数）
static inline int abs(int x) {
    return __builtin_abs(x);
}

// ❌ 差（手动实现）
static inline int abs(int x) {
    return x < 0 ? -x : x;
}
```

### 内存优化

```bash
# 查看生成的库大小
ls -lh build/*.a

# 不同配置的对比
# LOG=debug ASSERT=panic:  ~50KB
# LOG=none  ASSERT=off:    ~30KB (减少 40%)
```

---

## 常见问题

### 1. 编译错误：multiple definition

**问题**：
```
multiple definition of `function'
```

**原因**：在头文件中使用了非内联函数。

**解决**：
```c
// ❌ 错误
// header.h
void function(void) { }

// ✅ 正确
// header.h
static inline void function(void) { }
```

### 2. 未定义的引用

**问题**：
```
undefined reference to `uart_putchar'
```

**原因**：缺少平台层实现。

**解决**：
```c
// platform.c
void uart_putchar(char c) {
    // 实现
}
```

### 3. IntelliSense 错误

**问题**：VSCode 显示 "未定义标识符"

**原因**：架构选择错误。

**解决**：
1. 打开命令面板 (`Ctrl+Shift+P`)
2. 选择 "C/C++: Select a Configuration"
3. 选择正确的架构

### 4. 架构宏未定义

**问题**：
```
#error "Unsupported architecture"
```

**原因**：Makefile 没有正确传递架构宏。

**解决**：检查 Makefile 中的 `CFLAGS`：
```makefile
CFLAGS += -D__aarch64__  # 确保这一行存在
```

### 5. 链接错误

**问题**：
```
cannot find -laarch64-linux-musl-gcc
```

**原因**：工具链未安装或不在 PATH 中。

**解决**：
```bash
# 检查工具链
which aarch64-linux-musl-gcc

# 添加到 PATH
export PATH=$PATH:/path/to/toolchain/bin
```

---

## 附录

### A. 快速参考卡

#### 编译命令

```bash
make ARCH=aarch64 LOG=debug ASSERT=panic
make ARCH=riscv64 LOG=info ASSERT=panic
make ARCH=x86_64 LOG=none ASSERT=off
```

#### 常用宏

```c
MIN(a, b)                  // 最小值
MAX(a, b)                  // 最大值
CLAMP(x, lo, hi)           // 限制范围
ALIGN_UP(x, a)             // 向上对齐
ARRAY_SIZE(arr)            // 数组大小
container_of(ptr, type, member)  // 获取结构体
BIT(n)                     // 位操作
```

#### 日志宏

```c
KLOG_ERROR(fmt, ...)       // 错误
KLOG_WARN(fmt, ...)        // 警告
KLOG_INFO(fmt, ...)        // 信息
KLOG_DEBUG(fmt, ...)       // 调试
KLOG_TRACE(fmt, ...)       // 跟踪
```

#### 断言宏

```c
assert(cond)               // 可禁用
assert_always(cond)        // 总是启用
static_assert(cond, msg)   // 编译时
```

### B. 架构指令速查

| 操作 | AArch64 | RISC-V | x86_64 |
|------|---------|--------|--------|
| 数据屏障 | `dmb ish` | `fence rw, rw` | `mfence` |
| 读屏障 | `dmb ishld` | `fence r, r` | `lfence` |
| 写屏障 | `dmb ishst` | `fence w, w` | `sfence` |
| 指令屏障 | `isb` | `fence.i` | - |
| 等待事件 | `wfi` | `wfi` | `hlt` |
| 禁用中断 | `msr daifset, #0xF` | `csrci mstatus, 8` | `cli` |
| 原子交换 | `swp` / `ldxr/stxr` | `lr/sc` | `lock xchg` |

### C. 项目文件清单

#### 核心头文件

```
include/
├── types.h              # 基础类型
├── arg.h                # 可变参数
├── arch.h               # 架构检测
├── barrier.h            # 内存屏障
├── cache.h              # 缓存操作
├── spinlock.h           # 自旋锁
├── string.h             # 字符串操作
├── klog.h               # 内核日志
├── list.h               # 双向链表
├── mmio.h               # 内存映射I/O
└── assert.h             # 断言系统
```

#### 架构实现

```
include/aarch64/
├── barrier_impl.h
├── cache_impl.h
├── cpu.h
├── halt_arch.h
├── spin_lock_impl.h
└── string_impl.h

include/riscv64/
├── barrier_impl.h
├── cache_impl.h
├── halt_arch.h
├── spin_lock_impl.h
└── string_impl.h

include/x86_64/
├── barrier_impl.h
├── cache_impl.h
├── halt_arch.h
├── spin_lock_impl.h
└── string_impl.h
```

### D. 相关资源

- [ARM Architecture Reference Manual](https://developer.arm.com/documentation/)
- [RISC-V Instruction Set Manual](https://riscv.org/technical/specifications/)
- [Intel SDM (x86_64)](https://software.intel.com/content/www/us/en/develop/articles/intel-sdm.html)
- [Linux Kernel Documentation](https://www.kernel.org/doc/html/latest/)
- [Musl Libc](https://musl.libc.org/)

---

**文档版本**: 1.0
**最后更新**: 2025-01-02
**作者**: Ajax
**项目**: Avatar OS
