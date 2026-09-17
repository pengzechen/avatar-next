# 断言系统快速参考

## 编译使用

断言**始终启用**，没有编译期开关：

```bash
make PLATFORM=qemu-virt-aarch64 kernel
```

传 `ASSERT=` 会直接报错（该选项已移除，见文末「为什么没有 ASSERT=off」）。

## API 快速参考

| 宏 | 说明 | 失败时 |
|-----|------|--------|
| `assert(cond)` | 运行时断言 | 打印信息 + `platform_panic()` |
| `assert_always(cond)` | 关键路径断言，与 `assert` 行为一致 | 打印信息 + `platform_panic()` |
| `static_assert(cond, msg)` | 编译时断言（C11 `_Static_assert`） | 编译报错 |

## 使用示例

```c
#include "assert.h"

/* 基本断言 */
void example(void *ptr)
{
    assert(ptr != NULL);        // 失败时 panic
}

/* 关键路径断言：失败说明内核核心状态已被破坏 */
void critical(void)
{
    assert_always(!in_irq_context());
}

/* 编译时断言 */
static_assert(sizeof(int) == 4, "int must be 4 bytes");
```

`assert_always` 与 `assert` 的唯一区别是错误信息措辞
（`Critical assertion failed` vs `Assertion failed`），用于在日志中标注严重程度。

## 平台集成

`platform_panic()` 由平台层实现，声明在 `include/assert.h`：

```c
void platform_panic(void)
{
    /* 平台特定的清理，然后停机 */
    arch_halt();
}
```

当前实现在 `platforms/qemu/qemu_platform.c`。

## 文件结构

```
include/
  assert.h                      # 断言宏 + platform_panic 声明

platforms/qemu/
  qemu_platform.c               # platform_panic() 实现

tests/
  assert_test.c                 # 断言系统测试

docs/
  ASSERT_GUIDE.md               # 完整文档
  ASSERT_QUICKREF.md            # 本文件
```

## 性能

断言确实有运行时开销，但实测远小于直觉：

| 配置 | `.text` | ELF 总大小 |
|---|---|---|
| 断言启用 | 397,312 | 2,271,872 |
| 断言全部禁用 | 393,216 | 2,264,272 |
| **差值** | **-4,096 (-1.03%)** | **-7,600 (-0.335%)** |

（x86_64 qemu-virt 平台实测。禁用状态是临时构造出来做对照的，现已不提供该选项。）

## 为什么没有 ASSERT=off

曾经存在 `ASSERT=panic|off` 编译开关，2026-09 移除，原因：

1. **收益极小**：全内核禁用只省 1% 的 `.text`，因为真正大量使用断言的
   只有 `kernel/mm/pmm.c`（10 处）。
2. **文档数据是错的**：旧文档声称能省 `-40%`，与实测差约 120 倍。
3. **语义割裂**：内核关键路径用的是 `assert_always`，它被**刻意设计成
   不受该开关影响**——于是「关闭断言」关掉的只是最不重要的那批检查。
4. **风险不对称**：省下的 1% 换来的是一整类 bug 静默通过。

需要去掉某处检查时，直接删除那一行断言即可，不要重新引入整包开关。

## 架构支持

✅ AArch64 (ARM 64-bit)
✅ RISC-V 64-bit
✅ x86_64 (AMD64/Intel 64)
