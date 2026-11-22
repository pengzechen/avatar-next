# C API 文档（类 rustdoc）

本目录的文档由 `tools/gen_apidoc.py` 从 C 源码**紧挨函数定义上方的注释**生成，
入口是 [`index.md`](index.md)。浏览器里看用
`docs/api/html/index.html`（单页、可搜索、明暗主题，交互接近 rustdoc）。

```bash
make apidoc          # 重新生成 docs/api/ 下的全部产物
make apidoc-check    # 只检查：有提取警告时非 0 退出（CI 用）
```

生成器只用 Python 标准库，**不要求安装 Doxygen**。注释本身采用
**Doxygen 兼容格式**，所以将来接 Doxygen / Sphinx-Breathe 也不需要改源码。

## 产物

| 文件 | 内容 |
| --- | --- |
| `index.md` | 总览：模块/函数/覆盖率统计、子系统入口 |
| `kernel.md` 等 | 每个子系统一份，按源文件列出函数签名与说明 |
| `api.json` | 机器可读的完整数据（CI、编辑器插件、二次渲染） |
| `html/index.html` | 单文件可搜索 HTML，rustdoc 风格 |
| `auto_summaries.json` | 可选的人工/AI 自动摘要覆盖层（见下文） |

## 注释怎么写

### 1. 推荐格式（Doxygen 兼容）

```c
/**
 * @brief 从等待队列里摘掉一个任务。
 * @param queue 目标等待队列。
 * @return 被摘下的任务；队列为空时返回 NULL。
 * @note 调用者必须持有 queue->lock。
 * @warning 不能在中断上下文调用。
 */
task_t *waitq_pop(wait_queue_t *queue);
```

支持的标签：`@brief`、`@details`、`@param` / `@arg`、`@return` / `@returns` /
`@retval`、`@note`、`@warning`、`@see`、`@deprecated`、`@since`、`@todo`。
`@param` 写成 `@param[in] queue 说明` 也可以。

### 2. 项目现有的普通块注释也会被收录

```c
/*
   条件检查、入等待队列与置 BLOCKED 全在 mutex->lock 下完成。
   这是修掉旧实现丢唤醒的关键。
*/
void mutex_lock(mutex_t *mutex)
```

生成器会把它当作函数说明，并把第一行作为摘要。**空行分段**，段内换行保留。

### 3. 位置规则

- 注释必须紧挨函数定义上方，中间可以隔空行；
- 形如 `/* ── mutex_lock ────── */` 的装饰性分节标题会被跳过；
  如果它上方还有正文注释，生成器会继续向上找到正文；
- 行内注释（`#include "x.h" /* ... */`）不会被当成函数说明；
- 文件头注释会被当作模块说明。推荐写成：

```c
/*
   kernel/task/mutex.c — 睡眠锁(Mutex)实现

   这里写模块级设计、锁顺序、限制条件……
*/
```

如果在第一个 `#include` 之后写模块说明，第一行必须带 `文件名 — 标题`
前缀，否则生成器不会把它误认成模块说明。

### 4. 函数体内的备注

函数体内部的注释也会被收集，作为**函数内备注**，在 HTML 和 Markdown 里
默认折叠、点击展开。每条独立注释块是一条备注；同一函数有多条注释就分成
多个部分，并标出源码行号。行尾注释（`foo(); /* ... */`）会额外标“行尾”。

```c
int klog_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    /* 先估算长度，避免在栈上开大缓冲 */
    ...
    if (rc < 0)
        return -1; /* 格式化失败，保持 buf 原样 */
    ...
}
```

上面会产生两条备注：正文块注释和行尾注释。纯装饰线（`/* ────── */`）
会被跳过；函数体里的多段注释不会合并。

### 5. 没有注释的函数

文档里会显示“⚠️ 尚无文档注释”。两种补齐方式：

1. **改源码**（推荐）：在函数上方按上面的格式补注释，`make apidoc` 重新生成；
2. **自动摘要覆盖层**：把摘要写进 `docs/api/auto_summaries.json`，不改源码：

```json
{
  "kernel/rt/rt_worker.c::rtw_clamp_i64": "把 64 位值限制在 [lo, hi] 区间。",
  "kernel/rt/rt_worker.c::rtw_sat_add": {
    "brief": "饱和加法。",
    "description": "溢出时钳到 INT64_MIN/INT64_MAX，供 RT 模板的定点运算使用。"
  }
}
```

覆盖层只补**没有人工注释**的函数，展示时带“自动摘要”标记，不会冒充人工注释。
它适合批量生成初稿，长期仍应把内容迁回源码。

> 仓库里已经附带了一份自动摘要初稿（`auto_summaries.json`），它由一轮
> “阅读函数体写一句话”的批处理生成，覆盖当前所有没有人工注释的函数。
> 这些摘要是**未经人工逐条复核**的初稿；公共 API 应优先迁回源码注释，
> 迁回后覆盖层里对应的条目会自动失效（源码注释优先）。

## 覆盖率与 CI

```bash
python3 tools/gen_apidoc.py --check                  # 看统计与提取警告
python3 tools/gen_apidoc.py --check --strict         # 有提取警告就失败
python3 tools/gen_apidoc.py --check --min-coverage 50  # 覆盖率低于 50% 失败
```

`make apidoc-check` 等价于 `--check --strict`，适合放进 CI。覆盖率按
“有说明的函数 / 函数定义总数”计算；自动摘要单独计数。

建议优先级：先补 `include/` 的公共 API 和 `kernel/` 的核心子系统，
再补驱动和 VMM 的内部辅助函数。

## 扫描范围与限制

扫描目录：`kernel driver fs lib boot platforms apps include tools`。

排除：`third_party/`、`build/`、`imgs/`、`tests/ltp/`、`tests/.artifacts/`、
`apps/rknn-aarch64/`（厂商 SDK 头文件）。默认不扫描测试；需要时加
`--include-tests`（只包含 `tests/unit-test`）。

解析限制（有意保守）：

- 以**函数定义**为单位（含头文件里的 `static inline`），不重复收录声明；
- 同一函数在多个 `#if ARCH_*` 分支里各有一份定义时，会按源文件分别列出；
- 复杂宏生成的函数、K&R 风格定义不会被识别；
- 类型（struct/enum/typedef）暂未展开成独立条目，只在源码可见。

## 为什么不用 Doxygen

- 仓库里大部分函数只有普通 `/* */` 注释，Doxygen 直接忽略，覆盖率会接近 0；
- 生成器要能在任何机器上 `make apidoc`，不引入 apt/pip 依赖；
- HTML 是单文件，便于作为构建产物或 Artifact 分享。

注释保持 Doxygen 兼容，将来要切换工具链时不用重写。
