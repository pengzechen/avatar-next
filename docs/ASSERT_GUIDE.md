# 断言系统 (ASSERT)

## 概述

内核断言系统，提供运行时检查与编译时检查。断言**始终启用**，没有编译期开关。

## 编译

不需要任何额外参数：

```bash
make PLATFORM=qemu-virt-aarch64 kernel
```

`ASSERT=` 选项已于 2026-09 移除，显式传入会直接报错（原因见文末）。

## 断言类型

### 1. 运行时断言 - `assert()`

用于校验函数入参、内部不变量等。

```c
#include "assert.h"

void example(void *ptr)
{
    assert(ptr != NULL);  // 失败时打印信息并 panic
}
```

**行为**：条件为 false 时，通过 `KLOG_ERROR` 打印失败表达式、文件与行号，
然后调用 `platform_panic()` 停机。

### 2. 关键路径断言 - `assert_always()`

与 `assert()` 行为完全一致，仅错误信息措辞不同（`Critical assertion failed`
vs `Assertion failed`），用于在日志里标注"这里失败说明内核核心状态已被破坏"。

```c
void critical_check(void)
{
    assert_always(!in_irq_context());
}
```

当前调用点：`kernel/task/task.c:708`、`kernel/task/sched.c:183`。

> 历史上 `assert_always` 的意义是"即使 `ASSERT=off` 也生效"。该开关移除后，
> 它与 `assert` 已无功能差异，保留只是为了在调用点表达严重性。

### 3. 编译时断言 - `static_assert()`

C11 `_Static_assert` 的薄封装，条件为常量 false 时编译报错。

```c
static_assert(sizeof(int) == 4, "int must be 4 bytes");
static_assert(sizeof(void *) == 8, "must be 64-bit");
```

**使用场景**：类型大小验证、常量范围检查、编译时不变量。

## Panic 行为

断言失败时的完整调用链：

1. **打印错误信息**（`KLOG_ERROR`）
   ```
   [ERROR] Assertion failed: ptr != NULL, file kernel/mm/pmm.c, line 35
   ```

2. **调用 `platform_panic()`** —— 定义在 `platforms/qemu/qemu_platform.c`，
   实现为 `qemu_panic()`

3. **`qemu_panic()` 关中断后死循环停机**

   | 架构 | 关中断 | 停机指令 |
   |---|---|---|
   | AArch64 | `msr daifset, #0xF` | `wfe` |
   | RISC-V | `csrw sie, zero` / `csrw sip, zero` | `wfi` |
   | x86_64 | `cli` | `hlt` |

## 平台集成

平台层只需提供 `platform_panic()`：

```c
/* platforms/qemu/qemu_platform.c */
void platform_panic(void)
{
    qemu_panic();   /* 关中断 + 死循环 */
}
```

> 注意：`include/{aarch64,riscv64,x86_64}/halt_arch.h` 里的 `arch_halt()`
> 目前**没有任何调用点**，`qemu_panic()` 用的是自己的内联汇编。这些头文件
> 是历史遗留，实际不参与 panic 路径。

## 使用示例

### 指针与范围检查

```c
void init_device(struct device *dev)
{
    assert(dev != NULL);
    assert(dev->base_addr != 0);
    assert(IS_ALIGNED(dev->base_addr, PAGE_SIZE));
}
```

### 内部不变量

```c
uint64_t pmm_alloc_pages(pmm_t *pmm, uint32_t page_count)
{
    assert(pmm != NULL);
    assert(page_count > 0);
    /* ... */
}
```

当前内核里使用 `assert()` 的地方只有 `kernel/mm/pmm.c`（10 处），
集中在 PMM 的入参与不变量校验。

### 编译时验证

```c
struct task_control_block {
    uint64_t sp;
    uint64_t pc;
    /* ... */
};

static_assert(sizeof(struct task_control_block) == 256,
              "TCB size must be 256 bytes");
```

## 性能

断言确实有开销，但实测远小于直觉。x86_64 qemu-virt 平台完整构建对比：

| 配置 | `.text` | ELF 总大小 |
|------|---------|-----------|
| 断言启用（当前唯一模式） | 397,312 | 2,271,872 |
| 断言全部禁用（对照，已不可达） | 393,216 | 2,264,272 |
| **差值** | **-4,096 (-1.03%)** | **-7,600 (-0.335%)** |

开销量级是 **1%**，不是数量级上的差异。

## 为什么没有 ASSERT=off

曾经存在 `ASSERT=panic|off` 编译开关（`-DASSERT_OFF` 控制 `ASSERT_ENABLED`），
2026-09 移除，理由：

1. **收益极小**：全内核禁用只省 1% 的 `.text`，大头集中在
   `kernel/mm/pmm.c`。旧文档声称的 `-40%` 与实测相差约 120 倍。
2. **语义割裂**：关键路径用的是 `assert_always`，它被刻意设计成不受该开关
   影响——「关闭断言」关掉的只是最不重要的那批检查。
3. **风险不对称**：省 1% 体积换来一整类 bug 静默通过。

需要去掉某处检查时，直接删除那一行断言即可，不要重新引入整包开关。

## 注意事项

1. **副作用**：避免在断言条件里写有副作用的表达式
   ```c
   /* 错误 */
   assert(x++ > 0);

   /* 正确 */
   assert(x > 0);
   x++;
   ```

2. **函数调用**：不要把有副作用的函数调用放进断言条件

3. **panic 不可恢复**：断言失败会停机，确保这是期望行为

4. **断言不是错误处理**：可恢复的错误（如用户态传入非法指针）应当返回错误码，
   断言只用于"内核自己绝不该走到这里"的情况

## 测试

测试代码在 [`tests/assert_test.c`](../tests/assert_test.c)。

`tests/*.c` 会由 Makefile 自动编译链接进内核（见 `TESTS_SOURCES`），
但**不会自动运行**——需要在 `kernel/main.c` 的 `task_init()` 之后手动调用入口：

```c
run_assert_tests();
```

覆盖内容：
- ✅ 编译时断言
- ✅ 数组边界检查
- ✅ 指针有效性验证
- ✅ 算术假设

## 相关文档

- 快速参考：[`ASSERT_QUICKREF.md`](ASSERT_QUICKREF.md)
- 测试体系：[`tests.md`](tests.md)
