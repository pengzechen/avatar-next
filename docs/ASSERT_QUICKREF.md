# 断言系统快速参考

## 编译使用

```bash
# 启用断言（调试）
make ARCH=aarch64 ASSERT=panic

# 禁用断言（发布）
make ARCH=aarch64 ASSERT=off
```

## API 快速参考

| 宏 | 说明 | 可禁用 |
|-----|------|--------|
| `assert(cond)` | 运行时断言 | ✅ |
| `assert_always(cond)` | 总是启用的断言 | ❌ |
| `static_assert(cond, msg)` | 编译时断言 | N/A |
| `assert_not_reached()` | 声明代码不应到达 | ❌ |
| `assert_unreachable(expr)` | 声明表达式不可能为真 | ❌ |

## 使用示例

```c
#include "assert.h"

/* 基本断言 */
void example(void)
{
    int *ptr = NULL;
    assert(ptr != NULL);  // 失败时 panic
}

/* 总是启用的断言 */
void critical(void *ptr)
{
    assert_always(ptr != NULL);  // 即使 ASSERT=off 仍然检查
}

/* 编译时断言 */
static_assert(sizeof(int) == 4, "int must be 4 bytes");

/* 辅助宏 */
int parse(int value)
{
    switch (value) {
        case 0: return 100;
        default:
            assert_not_reached();  // 不应该到达这里
    }
}
```

## 平台集成

在平台层实现 `platform_panic()`：

```c
void platform_panic(void)
{
    /* 平台特定的清理 */
    klog_flush();

    /* 调用架构特定的 halt */
    arch_halt();
}
```

## 文件结构

```
include/
  assert.h              # 断言系统主头文件
  aarch64/
    halt_arch.h         # AArch64 halt 实现
  riscv64/
    halt_arch.h         # RISC-V halt 实现
  x86_64/
    halt_arch.h         # x86_64 halt 实现

examples/
  assert_test.c         # 测试示例

docs/
  ASSERT_GUIDE.md       # 完整文档
```

## 性能

- **ASSERT=panic**: 有条件检查开销
- **ASSERT=off**: 零运行时开销，代码体积减少 ~40%

## 架构支持

✅ AArch64 (ARM 64-bit)
✅ RISC-V 64-bit
✅ x86_64 (AMD64/Intel 64)
