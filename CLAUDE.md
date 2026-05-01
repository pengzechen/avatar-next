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

### 编译命令

```bash
# 基本编译
make ARCH=aarch64
make ARCH=riscv64
make ARCH=x86_64

# 调试版本（日志+断言）
make ARCH=aarch64 LOG=debug ASSERT=panic

# 发布版本（零开销）
make ARCH=aarch64 LOG=none ASSERT=off

# 清理
make ARCH=aarch64 clean

# 帮助
make help
```

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

- **内存屏障**: `docs/BARRIER.md` - barrier.h API 和实现
- **缓存操作**: `docs/CACHE.md` - cache.h DMA 和 MMIO 缓存管理
- **自旋锁**: `docs/SPINLOCK.md` - spinlock.h 同步原语

### 调试和日志

- **内核日志**: `docs/KLOG_GUIDE.md` - klog.h 日志系统和模块控制
- **断言系统**: `docs/ASSERT_GUIDE.md` - assert.h 运行时和编译时断言

### 数据结构和工具

- **双向链表**: `docs/LIST_API.md` - list.h Linux 风格链表
- **字符串操作**: `docs/STRING.md` - string.h 字符串和内存操作
- **MMIO**: `docs/MMIO.md` - mmio.h 内存映射 I/O

### 架构特定

- **AArch64**: `docs/arch/aarch64/NEON_USAGE.md` - NEON 优化

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
使用 klog 进行错误和调试输出：

```c
KLOG_ERROR("Critical error: %s", msg);
KLOG_DEBUG("Value: %d", value);
KLOG_MODULE_DEBUG(LOG_MODULE_UART, "UART init");
```

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
