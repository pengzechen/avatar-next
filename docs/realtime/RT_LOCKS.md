# RT 锁：mutex 修正与 rt_mutex（优先级继承）

> 一句话：**RT 数据面的短临界区用自旋锁；需要睡眠的共享状态用
> `rt_mutex`；普通文件系统/内核路径用修好的 `mutex`。** 目前还没有
> 用户态 PI futex，也没有完整的多锁 PI 链。

## 一、普通 mutex（`kernel/task/mutex.h`）

用途：普通内核路径，当前唯一使用者是 lwext4/FatFs 的全局串行化
（`fs/lwext4_port/fs_lock.c`）。

语义：

- 睡眠锁，可重入（同一个任务重复 `mutex_lock` 只增加计数）；
- 等待队列 FIFO，但解锁后允许抢跑；
- 没有优先级继承 —— **不要用它做 RT 任务之间的同步**。

2026-11 修正了 SMP 正确性：

- `mutex_t` 内部增加 `spinlock_t lock`，状态、owner 与等待队列全部在
  这把锁下修改；
- 等待者在**同一把锁下**完成「检查 → 入队 → 置 BLOCKED」，然后才释放
  锁并 `sched_schedule()`。旧实现先恢复中断再 `task_block()`，解锁若落在
  中间会丢唤醒；
- `mutex_is_locked()/mutex_holder()` 也加锁读取，避免 SMP 下读到撕裂值。

锁序：`mutex->lock → 目标核 rq_lock`（`task_unblock` 内部）。没有反向
路径，和 `completion.c` 一致。

## 二、rt_mutex（`kernel/task/rt_mutex.h`）

```c
#include "task/rt_mutex.h"

static rt_mutex_t m = ...; /* 必须 rt_mutex_init() */

rt_mutex_lock(&m);
/* 共享状态 */
rt_mutex_unlock(&m);
```

语义：

- **非递归**：同一任务重复加锁返回 `-RTM_EDEADLK`；
- 等待者按有效 RT 优先级降序排队（同优先级 FIFO）；
- 等待者入队时把 owner 的继承优先级提升到不低于自己的优先级；
- 解锁时把锁交给队首（最高优先级）等待者，并把新 owner 的继承优先级
  设为剩余等待者的最高值；
- 线程上下文才可调用（会睡眠）；ISR / RT step / `rtdev` 的 RT 面不能用。

### 调度器支持

`task_t` 增加 `pi_priority`（0 = 无继承）。有效优先级为：

```text
effective = max(基础 rt_priority（NORMAL 为 0）, pi_priority)
```

`sched_set_pi_boost()` 负责：

- 更新 `pi_priority`；
- 如果任务当前 READY 且不在 CPU 上，在同一把 `rq_lock` 下先出队、再按
  新的有效优先级入队（否则按新优先级找不到旧队列节点）；
- 被提升的任务在 tick 上按 FIFO 对待 —— 否则中等优先级任务会在
  tick 点抢走持锁者，PI 就失效了。

### 观察到的行为

`RT_TEST=1` 的场景 6（三架构，分别跑 RT owner 和 NORMAL owner）：

- 低优先级 owner（RT 20，或 NORMAL 10）持锁；
- 高优先级等待者（80）入队 → owner 的 `pi_priority` 变为 80；
- 第二个等待者（60）入队 → 仍为 80；
- 中等优先级任务（50）就绪后，owner 以 80 先释放锁；
- 等待者按 80 → 60 的顺序拿锁，中等任务最后完成；
- owner 释放后 `pi_priority` 恢复为 0。

### 已知限制（V1）

1. **只做直接继承**。owner 自己又阻塞在另一把 `rt_mutex` 上时，不会沿
   等待链继续提升（没有嵌套 PI 链）。
2. **不跟踪任务持有的全部 rt_mutex**。同一任务持有多把有等待者的
   rt_mutex 时，释放其中一把会把继承优先级清零，可能丢失另一把锁带来
   的提升。
3. 不做死锁检测（A→B→A 会真的睡死）。
4. 没有超时/可中断等待；持有者退出时也不会自动释放。

需要完整语义时，按 Linux `rt_mutex` 的 waiter/owner 链 + PI chain 重做。

## 三、选择建议

| 场景 | 用什么 |
|---|---|
| ISR 与线程共享的短临界区 | `spin_lock_irqsave` |
| RT 任务之间的共享状态 | `rt_mutex` |
| RT 数据面与 supervisor | 共享页 + seqlock/原子变量（不睡眠） |
| 普通内核/文件系统长操作 | `mutex` |
| 用户态 pthread | futex（当前只有 WAIT/WAKE，无 PI） |

## 四、回归门禁

```bash
# 三架构：mutex 并发压力 + rt_mutex PI 顺序/恢复
make -C tests rt-test

# 真并发：SMP=4（mutex 的 SMP 竞争检查需要 >1 核）
make -C tests rt-test SMP=4 ARCHES=x86_64
```

串口标记：

```text
=== RT MUTEX TEST: PASS mtx=18000 err=0 boost=80 after=0 order=1/2/3 ===
```

`mtx` 是 6 个任务 × 3000 次临界区；`err` 统计同时进入临界区的次数；
`order` 是 high(80)/low(60)/medium(50) 的完成顺序。
