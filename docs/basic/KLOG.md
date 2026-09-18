# 内核日志系统（klog）

## 快速上手

```bash
# 默认：级别 info，模块全开
make PLATFORM=qemu-virt-aarch64 run

# 定位中断问题：只要 GIC 的调试日志，别的模块闭嘴
make PLATFORM=qemu-virt-aarch64 run LOG=debug LOG_MODULES=gic

# 发布版：所有日志宏在编译期被抹除
make PLATFORM=qemu-virt-aarch64 kernel LOG=none
```

启动时会打印一行生效配置，排查"为什么我的日志不见了"先看它：

```
[INFO][C0] lib/klog.c:225: [klog] level=3 modules=init,task,driver,uart,timer,mm,fs,net,smp,gic,generic
```

## 两种控制手段

| | 编译期 | 运行期 | 作用 |
|---|---|---|---|
| **级别** | `make LOG=<level>` | `set_log_level()` | 决定 DEBUG/TRACE 的门槛 |
| **模块** | `make LOG_MODULES=<list>` | `log_set_modules_by_name()` | 在 DEBUG/TRACE 上再筛一层 |

### 级别：`LOG=`（编译期）

| 级别 | 值 | 颜色 |
|------|---|------|
| `none` | 0 | — |
| `error` | 1 | 红 |
| `warn` | 2 | 黄 |
| `info` | 3（默认） | 绿 |
| `debug` | 4 | 蓝 |
| `trace` | 5 | 灰 |

**只有 `LOG=none` 是编译期抹除**：宏体整个变成 `do {} while (0)`，参数不求值、格式串也不进 `.rodata`。

其余级别（error/warn/info/debug/trace）只是把 `g_log_level` 的**初值**设成该级别，调用点仍然带着"读 `g_log_level` + 比较 + 分支"——实测默认 info 构建下每个 `KLOG_DEBUG` 调用点都是：

```
ldr w0, [g_log_level]; cmp w0, #0x3; b.hi <skip>
```

两个直接推论：

- 运行期 `set_log_level(LOG_LEVEL_TRACE)` 能把 info 构建里的 TRACE 日志**打开**（代码一直都在）；
- `KLOG_ERROR` 没有任何级别检查，运行期**关不掉**（要关只能 `LOG=none`）。

> 早先文档写的"`LOG=error` 时其他日志会被编译器优化掉"是错的 —— 那是把 `LOG=none` 的行为安到了别的级别上。

### 模块：`LOG_MODULES=`（编译期白名单）

```bash
make ... LOG=debug LOG_MODULES=uart,gic      # 只放行这两个模块的 DEBUG/TRACE
make ... LOG=debug LOG_MODULES=all           # 全部（等于不传）
make ... LOG=debug LOG_MODULES=none          # 全不放行
```

模块掩码只对 `KLOG_DEBUG` / `KLOG_TRACE` / `KLOG_MODULE_*` 起作用（ERROR/WARN/INFO 不受模块限制）。

**白名单语义**：`LOG_MODULES=` 一经传入，掩码就是白名单——
- 打了模块标签的日志（`KLOG_MODULE_DEBUG(LOG_MODULE_GIC, ...)` 及 `KLOG_UART()` 这类简写）按位放行；
- **未打标签**的 `KLOG_DEBUG` / `KLOG_TRACE` 归入 `LOG_MODULE_GENERIC`，不在名单里就不输出；
- 不传 `LOG_MODULES=` 时掩码是全 1，因此旧行为完全不变。

实测（`LOG=debug LOG_MODULES=gic`）：8 条 GIC 模块日志输出、普通 DEBUG 一条不出；把 `generic` 加进名单后普通 DEBUG 恢复 11 条。

模块名拼错会在启动时报错并点名（同时掩码为 0，即什么都不输出）：

```
[ERROR][C0] lib/klog.c:219: [klog] LOG_MODULES 有 1 个无法识别的模块名："gci"
```

### 运行期等价 API

```c
#include "klog.h"

/* 按名字设置（与编译期 LOG_MODULES= 同一套名字） */
log_set_modules_by_name("uart,gic");     /* 返回无法识别的名字个数 */

/* 按位操作 */
log_set_modules(LOG_MODULE_UART | LOG_MODULE_GIC);
log_add_module(LOG_MODULE_TIMER);
log_remove_module(LOG_MODULE_FS);

/* 查询：把当前启用的模块名写进缓冲区 */
char names[128];
log_modules_to_string(names, sizeof(names));

/* 级别 */
set_log_level(LOG_LEVEL_DEBUG);
get_log_level();
```

## 日志宏

```c
#include "klog.h"

/* 所有宏都**不**自动补换行，调用方自己写 "\n" */
KLOG_ERROR("Critical error: %s\n", msg);      /* 总是显示（LOG=none 除外）*/
KLOG_WARN("value out of range: %d\n", v);
KLOG_INFO("System initialized, %d MB\n", mem_size);
KLOG_DEBUG("Allocated %p size=%d\n", ptr, size);   /* 受 LOG_MODULES 白名单约束 */
KLOG_TRACE("entry: %s\n", __func__);               /* 同上 */

/* 模块日志：只在 DEBUG/TRACE 且该模块位放行时输出 */
KLOG_MODULE_DEBUG(LOG_MODULE_GIC, "GIC: PMR=0x%x\n", pmr);
KLOG_MODULE_TRACE(LOG_MODULE_TASK, "switch %d -> %d\n", from, to);
KLOG_UART("UART initialized at %d baud\n", baud);   /* == KLOG_MODULE_DEBUG(LOG_MODULE_UART, ...) */
```

`KLOG_DEBUG` 与 `KLOG_MODULE_DEBUG` 的区别只有一个：后者额外带模块位，因此能被 `LOG_MODULES=` 单独选中。

### 内置模块

| 名字（`LOG_MODULES=` 里用） | 宏 | 说明 |
|---|---|---|
| `init` | `LOG_MODULE_INIT` | 初始化 |
| `task` | `LOG_MODULE_TASK` | 任务/调度 |
| `driver` | `LOG_MODULE_DRIVER` | 驱动 |
| `uart` | `LOG_MODULE_UART` | UART（`logger_*` 里 `logger_debug` 不带位） |
| `timer` | `LOG_MODULE_TIMER` | 定时器 |
| `mm` | `LOG_MODULE_MM` | 内存管理 |
| `fs` | `LOG_MODULE_FS` | 文件系统 |
| `net` | `LOG_MODULE_NET` | 网络（virtio-net 的每包日志挂在这里） |
| `smp` | `LOG_MODULE_SMP` | 多核 |
| `gic` | `LOG_MODULE_GIC` | GIC（`driver/irq/gicv2.c` 的 `logger_gic_debug`） |
| `generic` | `LOG_MODULE_GENERIC` | 未打标签的 DEBUG/TRACE |

加新模块要同时改两处：`include/klog.h` 的位定义、`lib/klog.c` 的名字表。

## 输出格式

```
[INFO][C0] kernel/main.c:174: === Avatar OS Kernel ===
[ERROR][C0] lib/klog.c:219: [klog] LOG_MODULES 有 1 个无法识别的模块名："gci"
[DEBUG][C0] [MOD] driver/irq/gicv2.c:116: GIC: Detected 288 IRQ lines for virtualization
```

`[级别][C<cpuid>] 文件:行号: 消息`，模块日志多一个 `[MOD]` 标记。级别带 ANSI 颜色。

## 注意事项

1. **单行上限 511 字节**：`kvprintf` 用 512 字节栈缓冲，超长行会被**截断**（`my_vsnprintf` 按 C99 语义返回"本该写多长"，`kvprintf` 会 clamp 到缓冲长度）。
2. **换行**：所有宏都不自动补 `"\n"`，忘记写就会和下一行连在一起。
3. **性能**：`DEBUG`/`TRACE` 调用点在 info 构建下仍在（一次全局读 + 比较 + 分支），只是不执行；要彻底去掉用 `LOG=none`。
4. **线程/中断安全**：klog 自己用 IRQ-safe 自旋锁按**整条消息**加锁（格式化在锁外做），所以 ISR 里打日志是安全的。
5. **断言不受日志级别影响**：`assert()` 失败时用 `kprintf` 直接输出（不是 `KLOG_ERROR`），因此 `LOG=none` 构建下**也会**打印 —— 以前它会被静默掉，只剩一个没有任何输出的 panic。
6. **不要在被 klog 依赖的路径里打日志**：UART 驱动的"发送缓冲满、重试超时"分支里有 `logger_warn`，而它是在 klog 持锁逐字符输出的过程中被调用的 —— 一旦该分支可达就是同核自死锁。目前 UART 的缓冲/中断发送路径整体未启用（`uart_initialized` 永远是 false，初始化赋值被注释掉了），所以不可达；**启用那条路径前必须先解决这个重入**。同理，PL011 的 TX 缓冲锁用的是非 IRQ-safe 的 `spin_lock`。

## 测试

`tests/klog_test.c` 里有级别/模块/动态切换的用例，但 `run_klog_tests()` **目前没有任何调用者**（和之前 `tests/string_test.c` 一样是死代码）。要跑起来需要在 `kernel_main` 里显式调用；字符串那套自检已经用 `make ... test-string` 接通了（见 `docs/basic/STRING.md`），klog 这套还没接。
