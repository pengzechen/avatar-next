# AArch64 NEON 启用指南

## 启用 NEON 工具函数

在 [include/aarch64/cpu.h](include/aarch64/cpu.h) 中提供了完整的 NEON 控制接口。

### API 说明

#### 1. `aarch64_enable_neon()` - 启用 NEON

```c
static inline void aarch64_enable_neon(void);
```

**功能**：启用 NEON/浮点指令，允许在 EL0 使用

**原理**：
```assembly
mrs x0, cpacr_el1          # 读取 CPACR_EL1
orr x0, x0, #(3 << 20)     # 设置 FPEN[21:20] = 0b11
msr cpacr_el1, x0          # 写回 CPACR_EL1
isb                        # 指令同步屏障
```

**CPACR_EL1.FPEN 字段**：
- `0b00`: 根据 CPACR_EL1.TTA 决定
- `0b01`: 执行 NEON/FP 时 Trap 到 EL1
- `0b10`: 执行 NEON/FP 时 Trap 到 EL2
- `0b11`: ✅ 允许在 EL0 使用 NEON/FP

#### 2. `aarch64_disable_neon()` - 禁用 NEON

```c
static inline void aarch64_disable_neon(void);
```

**功能**：禁用 NEON/浮点指令，执行时会触发异常

#### 3. `aarch64_is_neon_enabled()` - 检查状态

```c
static inline int aarch64_is_neon_enabled(void);
```

**返回值**：1 表示已启用，0 表示未启用

#### 4. `aarch64_get_cpacr()` - 读取寄存器

```c
static inline uint64_t aarch64_get_cpacr(void);
```

**返回值**：CPACR_EL1 寄存器的当前值

## 使用方法

### 在内核初始化时启用

```c
#include "aarch64/cpu.h"

void kernel_main(void)
{
    /* 早期初始化 */
    aarch64_enable_neon();  /* 启用 NEON */

    /* 现在可以安全使用 NEON 优化的函数 */
    memcpy(dest, src, size);  /* 会使用 NEON 优化版本 */
}
```

### 在 string_impl.h 中的使用

[include/aarch64/string_impl.h](include/aarch64/string_impl.h) 中的 `memcpy_neon()` 依赖 NEON：

```c
static inline void *memcpy_neon(void *dest, const void *src, size_t n)
{
    /* 使用 NEON 指令 */
    asm volatile(
        "ld1 {v0.16b}, [%[src]]\n"
        "st1 {v0.16b}, [%[dest]]\n"
        ...
    );
}
```

**重要**：调用前必须先执行 `aarch64_enable_neon()`

## 注意事项

1. **必须早期初始化**：在内核启动早期调用，避免任何 NEON 指令执行前
2. **异常处理**：如果未启用就执行 NEON 指令，会触发异常
3. **EL1 权限**：这些函数需要在 EL1（内核态）执行
4. **性能影响**：NEON 可以显著提升大块内存拷贝性能

## 自动启用策略

当前实现中：
- **小块数据** (≤ 128 字节)：使用通用实现
- **大块数据** (> 128 字节)：使用 NEON 优化

这样可以确保在启用 NEON 后自动获得最佳性能。
