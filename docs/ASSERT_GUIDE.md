# 断言系统 (ASSERT)

## 概述

完整的内核断言系统，支持运行时检查和编译时检查。

## 编译

```bash
# 基本编译（断言启用）
make ARCH=aarch64

# 禁用断言（发布模式）
make ARCH=aarch64 ASSERT=off

# 启用断言（调试模式，默认）
make ARCH=aarch64 ASSERT=panic
```

## 断言类型

### 1. 运行时断言 - `assert()`

可被编译禁用的断言，用于开发和调试。

```c
#include "assert.h"

void example(void)
{
    int *ptr = NULL;
    assert(ptr != NULL);  // 失败时 panic
}
```

**行为**：
- `ASSERT=panic`（默认）：启用断言，失败时调用 `platform_panic()`
- `ASSERT=off`：完全禁用断言，零运行时开销

### 2. 总是启用的断言 - `assert_always()`

即使 `ASSERT=off` 仍然有效的断言，用于关键检查。

```c
void critical_check(void *ptr)
{
    assert_always(ptr != NULL);  // 总是检查
}
```

**使用场景**：
- 关键数据结构检查
- 安全相关验证
- 不应该被禁用的检查

### 3. 编译时断言 - `static_assert()`

编译时检查，如果条件为常量 false，编译时报错。

```c
static_assert(sizeof(int) == 4, "int must be 4 bytes");
static_assert(sizeof(void *) == 8, "must be 64-bit");
```

**使用场景**：
- 类型大小验证
- 常量范围检查
- 编译时不变量验证

## 辅助宏

### `assert_not_reached()`

声明代码不应该执行到这里。

```c
int parse_value(int value)
{
    switch (value) {
        case 0: return 100;
        case 1: return 200;
        default:
            assert_not_reached();  // 不应该执行到这里
    }
}
```

### `assert_unreachable(expr)`

声明表达式永远不会为真。

```c
void process_state(int state)
{
    if (state == STATE_INIT) {
        /* 初始化逻辑 */
    } else if (state == STATE_RUNNING) {
        /* 运行逻辑 */
    } else {
        assert_unreachable(state);  // 不可能的状态
    }
}
```

## Panic 行为

断言失败时的行为：

1. **输出错误信息**（通过 KLOG_ERROR）
   ```
   [ERROR] file.c:42: Assertion failed: ptr != NULL, file test.c, line 123
   ```

2. **调用 `platform_panic()`**
   - 平台层实现 `platform_panic()`
   - 该函数调用架构特定的 `arch_halt()`

3. **架构特定的 halt**
   - **AArch64**: WFI (Wait For Interrupt) + 禁用中断
   - **RISC-V**: WFI + 禁用中断
   - **x86_64**: HLT + 禁用中断

## 平台集成

### 实现 platform_panic()

在平台层实现：

```c
/* platform.c */
#if defined(__aarch64__)
    #include "aarch64/halt_arch.h"
#elif defined(__x86_64__)
    #include "x86_64/halt_arch.h"
#elif defined(__riscv)
    #include "riscv64/halt_arch.h"
#endif

void platform_panic(void)
{
    /* 可以在这里添加平台特定的清理逻辑 */
    /* 例如：刷新日志、关闭设备等 */

    /* 最后调用架构特定的 halt */
    arch_halt();
}
```

## 使用示例

### 示例 1：指针检查

```c
void init_device(struct device *dev)
{
    assert(dev != NULL);
    assert(dev->base_addr != 0);
    assert(IS_ALIGNED(dev->base_addr, PAGE_SIZE));
}
```

### 示例 2：数组边界

```c
void buffer_write(int *buf, size_t index, int value)
{
    assert(index < BUFFER_SIZE);
    buf[index] = value;
}
```

### 示例 3：状态机检查

```c
void task_switch(task_t *from, task_t *to)
{
    assert_always(from != NULL);
    assert_always(to != NULL);
    assert(to->state == TASK_READY);
    /* 切换任务 */
}
```

### 示例 4：编译时验证

```c
/* 确保结构体大小符合预期 */
struct task_control_block {
    uint64_t sp;
    uint64_t pc;
    /* ... */
};

static_assert(sizeof(struct task_control_block) == 256,
              "TCB size must be 256 bytes");
```

## 性能考虑

### 编译时开销

| 配置 | 代码体积 | 运行时开销 |
|------|---------|-----------|
| `ASSERT=panic` | 基准 | 条件检查开销 |
| `ASSERT=off` | -40% | 零开销 |

### 建议

- **开发阶段**：使用 `ASSERT=panic`
- **性能测试**：使用 `ASSERT=panic`
- **生产发布**：使用 `ASSERT=off`

## 注意事项

1. **副作用**：避免在断言条件中使用有副作用的表达式
   ```c
   /* 错误：有副作用 */
   assert(x++ > 0);

   /* 正确 */
   assert(x > 0);
   x++;
   ```

2. **函数调用**：复杂的函数调用不应放在断言中（禁用断言时不执行）

3. **assert_always**：谨慎使用，避免在关键路径上使用

4. **panic 不可恢复**：断言失败会停止系统，确保这是期望的行为

## 架构特定实现

### AArch64

```c
/* include/aarch64/halt_arch.h */
static inline void arch_halt(void)
{
    dmb();
    dsb();
    __asm__ volatile("msr daifset, #0xF" ::: "memory");
    __asm__ volatile("wfi" ::: "memory");
    while (1) {
        __asm__ volatile("wfi" ::: "memory");
    }
}
```

### RISC-V

```c
/* include/riscv64/halt_arch.h */
static inline void arch_halt(void)
{
    fence(i, w);
    __asm__ volatile("csrci mstatus, 0x8" ::: "memory");
    __asm__ volatile("wfi" ::: "memory");
    while (1) {
        __asm__ volatile("wfi" ::: "memory");
    }
}
```

### x86_64

```c
/* include/x86_64/halt_arch.h */
static inline void arch_halt(void)
{
    mfence();
    __asm__ volatile("cli" ::: "memory");
    __asm__ volatile("hlt" ::: "memory");
    while (1) {
        __asm__ volatile("hlt" ::: "memory");
    }
}
```

## 测试

运行 [examples/assert_test.c](../examples/assert_test.c) 中的测试：

```bash
make ARCH=aarch64 ASSERT=panic
```

测试覆盖：
- ✅ 基本断言
- ✅ 总是启用的断言
- ✅ 编译时断言
- ✅ 辅助宏
- ✅ 数组边界检查
- ✅ 指针验证
