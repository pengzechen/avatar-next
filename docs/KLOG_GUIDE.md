# 内核日志系统 (KLOG)

## 概述

完整的内核日志系统，支持全局日志级别和模块级别的细粒度控制。

## 编译

```bash
# 基本编译
make ARCH=aarch64

# 指定日志级别
make ARCH=aarch64 LOG=none     # 关闭所有日志
make ARCH=aarch64 LOG=error    # 只显示错误
make ARCH=aarch64 LOG=warn     # 警告和错误
make ARCH=aarch64 LOG=info     # 信息及以上（默认）
make ARCH=aarch64 LOG=debug    # 调试及以上
make ARCH=aarch64 LOG=trace    # 所有日志（包括跟踪）
```

## 日志级别

| 级别 | 值 | 显示内容 | 颜色 |
|------|---|---------|------|
| `NONE` | 0 | 关闭所有日志 | - |
| `ERROR` | 1 | 只显示错误 | 红色 |
| `WARN` | 2 | 警告和错误 | 黄色 |
| `INFO` | 3 | 信息、警告和错误 | 绿色 |
| `DEBUG` | 4 | 调试信息及以上 | 蓝色 |
| `TRACE` | 5 | 所有日志（包括跟踪） | 无色 |

## API 使用

### 基本日志宏

```c
#include "klog.h"

/* 错误日志（总是显示） */
KLOG_ERROR("Critical error: %s", error_msg);

/* 警告日志 */
KLOG_WARN("Warning: value out of range: %d", value);

/* 信息日志 */
KLOG_INFO("System initialized, memory: %d MB", mem_size);

/* 调试日志 */
KLOG_DEBUG("Allocated buffer at %p, size: %d", ptr, size);

/* 跟踪日志 */
KLOG_TRACE("Function entry: %s", __func__);
```

### 模块级调试日志

#### 启用模块日志

```c
/* 启用 UART 和 TIMER 模块 */
log_set_modules(LOG_MODULE_UART | LOG_MODULE_TIMER);

/* 添加单个模块 */
log_add_module(LOG_MODULE_DRIVER);

/* 移除模块 */
log_remove_module(LOG_MODULE_FS);
```

#### 使用模块日志

```c
/* 方式 1: 使用预定义的模块宏 */
KLOG_UART("UART initialized at %d baud", baud_rate);
KLOG_TIMER("Timer tick: %d", tick);
KLOG_DRIVER("Device %s registered", dev_name);

/* 方式 2: 自定义模块日志 */
KLOG_MODULE_DEBUG(LOG_MODULE_INIT, "CPU %d initialized", cpu_id);
KLOG_MODULE_TRACE(LOG_MODULE_TASK, "Task switch: %d -> %d", from, to);
```

### 动态日志级别控制

```c
/* 运行时切换日志级别 */
set_log_level(LOG_LEVEL_DEBUG);
KLOG_INFO("Log level set to DEBUG");

/* 查询当前日志级别 */
log_level_t level = get_log_level();
```

## 模块定义

### 内置模块

```c
#define LOG_MODULE_INIT    (1ULL << 0)  /* 初始化模块 */
#define LOG_MODULE_TASK    (1ULL << 1)  /* 任务调度模块 */
#define LOG_MODULE_DRIVER  (1ULL << 2)  /* 驱动模块 */
#define LOG_MODULE_UART    (1ULL << 3)  /* UART 驱动 */
#define LOG_MODULE_TIMER   (1ULL << 4)  /* 定时器模块 */
#define LOG_MODULE_MM      (1ULL << 5)  /* 内存管理模块 */
#define LOG_MODULE_FS      (1ULL << 6)  /* 文件系统模块 */
#define LOG_MODULE_NET     (1ULL << 7)  /* 网络模块 */
#define LOG_MODULE_SMP     (1ULL << 8)  /* 多核模块 */
```

### 添加新模块

```c
/* 在 klog.h 中添加（使用 1ULL << N） */
#define LOG_MODULE_BLOCK  (1ULL << 9)  /* 块设备模块 */
#define LOG_MODULE_CHAR   (1ULL << 10) /* 字符设备模块 */
```

**注意**：系统支持最多 64 个模块（0-63），定义新模块时使用 `1ULL << N` 格式。

## 使用场景

### 场景 1: 驱动调试

```c
/* 启用驱动模块日志 */
log_set_modules(LOG_MODULE_DRIVER);

/* 驱动代码中 */
int uart_init(void)
{
    KLOG_DRIVER("UART init start");
    /* 初始化代码 */
    KLOG_DRIVER("UART configured: %d baud", 115200);
    KLOG_DRIVER("UART ready");
    return 0;
}
```

### 场景 2: 任务调度跟踪

```c
/* 启用任务模块日志 */
log_set_modules(LOG_MODULE_TASK);

void task_switch(task_t *from, task_t *to)
{
    KLOG_TASK("Context switch: %d -> %d", from->id, to->id);
    KLOG_TASK("Stack pointers: from=%p, to=%p", from->sp, to->sp);
}
```

### 场景 3: 性能分析

```c
/* 编译时指定日志级别 */
make ARCH=aarch64 LOG=trace

/* 代码中 */
void fast_path(void)
{
    KLOG_TRACE("Fast path taken, branch prediction: %s", success ? "hit" : "miss");
    /* ... */
}
```

### 场景 4: 生产环境

```c
/* 编译时关闭日志 */
make ARCH=aarch64 LOG=none

/* 或只保留错误日志 */
make ARCH=aarch64 LOG=error

/* 代码中 */
KLOG_ERROR("Critical failure: system halt");
/* 其他日志会被编译器优化掉 */
```

## 性能优化

### 编译时优化

```bash
# LOG=none 时，所有日志宏被定义为空操作
make LOG=none

# 代码体积：减少约 40%
# 运行时开销：几乎为零
```

### 运行时优化

```c
/* 模块日志只在 DEBUG/TRACE 级别时检查 */
#define KLOG_MODULE_DEBUG(module, fmt, ...) \
    do { \
        if ((g_log_level >= LOG_LEVEL_DEBUG) && log_is_module_enabled(module)) { \
            /* ... */ \
        } \
    } while (0)
```

## 日志格式

### 输出格式

```
[INFO ] file.c:42: This is an info message
[ERROR] file.c:123: Critical error: test error
[WARN ] file.c:456: Warning: value out of range
[MOD  ] file.c:789: UART initialized at 115200 baud
```

### 格式说明

- `[INFO ]` - 日志级别（右对齐，5 字符）
- `file.c:42` - 源文件和行号
- `message` - 日志内容
- `[MOD ]` - 模块日志标识

## 兼容性

### Linux 内核兼容宏

```c
/* pr_* 系列宏 */
pr_err(fmt, ...)    // 等同于 KLOG_ERROR
pr_warn(fmt, ...)   // 等同于 KLOG_WARN
pr_info(fmt, ...)   // 等同于 KLOG_INFO
pr_debug(fmt, ...)  // 等同于 KLOG_DEBUG

/* printk 别名 */
printk(fmt, ...)    // 等同于 kprintf
```

## 测试

运行 [examples/klog_test.c](examples/klog_test.c) 中的测试：

```bash
make ARCH=aarch64 LOG=debug
```

测试覆盖：
- ✅ 不同日志级别
- ✅ 模块日志
- ✅ 动态级别切换
- ✅ 格式化输出

## 注意事项

1. **日志级别设置**：
   - 编译时：`make LOG=debug`
   - 运行时：`set_log_level(LOG_LEVEL_DEBUG)`

2. **模块日志**：
   - 只在 DEBUG/TRACE 级别生效
   - 需要显式启用：`log_set_modules(LOG_MODULE_XXX)`

3. **性能影响**：
   - LOG=none 时开销最小（所有日志被编译掉）
   - LOG=trace 时开销最大

4. **线程安全**：
   - klog 使用 spinlock 保护
   - 可以在中断上下文中使用
