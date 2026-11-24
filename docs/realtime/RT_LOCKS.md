# RT 锁：mutex 修正与 rt_mutex（优先级继承）

> 一句话：**RT 数据面的短临界区用自旋锁；需要睡眠的共享状态用
> `rt_mutex`；普通文件系统/内核路径用修好的 `mutex`。** 目前还没有
> 用户态 PI futex；内核 `rt_mutex` 已支持嵌套 PI 链。

## 一、普通 mutex（`include/task/mutex.h`）

用途：普通内核路径，当前唯一使用者是 lwext4/FatFs 的全局串行化
（`fs/ext4/port/fs_lock.c`）。

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

## 二、rt_mutex（`include/task/rt_mutex.h`）

```c
#include "task/rt_mutex.h"

static rt_mutex_t m = ...; /* 必须 rt_mutex_init() */

rt_mutex_lock(&m);
/* 共享状态 */
rt_mutex_unlock(&m);
```

语义：

- **非递归**：同一任务重复加锁返回 `-RTM_EDEADLK`；
- 非 owner 调用 `rt_mutex_unlock()` 返回 `false`（API 契约）；内核只打
  `KLOG_DEBUG`，避免负向测试/误用刷出假的 ERROR。调用者必须检查返回值；
- 等待者按等待优先级降序排队（同优先级 FIFO）；
- 等待者入队时把 owner 的继承优先级提升到不低于自己的优先级；
- **支持嵌套 PI 链**：owner 自己又阻塞在另一把 `rt_mutex` 上时，提升
  会沿 owner 链逐级向上传递，直到最底层的持有者（见下节）；
- 解锁时把锁交给队首（最高优先级）等待者，并把新 owner 的继承优先级
  设为剩余等待者的最高值；释放者自己的提升按它仍持有的其余锁重算；
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

### PI 链（嵌套优先级继承）

实现：`kernel/rt/rt_mutex.c`。每个任务记两个东西：

- `held_rt_mutexes`：当前持有的锁（`rt_mutex_t.held_node` 链）；
- `pi_blocked_on`：正在等哪把锁（NULL = 没在等）。

一个任务的提升 = 它持有的**所有**锁里最高等待者的 `pi_waiter_prio`。
等待者阻塞时，从该锁的 owner 开始沿 `pi_blocked_on` 向上走：重算 owner
的 `pi_priority`；若它自己也在等锁，就把它在那个等待队列里的优先级更新
并重排，再继续处理上一层的 owner。

经典三层例子（`RT_TEST=1` 的 `rt-mutex-pi-chain` 场景）：

```text
H(80) --等--> M2(owner = M)
M(50) --等--> M1(owner = L)
L(20) --持--> M1，并在事件上睡眠

没有链:  H 只把 M 直接提到 80；L 停在 50
有链:    M = 80，且 L 沿 M 的 pi_blocked_on 被传递提到 80
```

测试用一个 W(60) 做“墙”：同时唤醒 W 和 L，链生效时 L(80) 先运行并
释放 M1，H 的拿锁延迟是微秒级；链失效时 L 只有 50，W(60) 会先跑满
20ms，H 的延迟被拖到 20ms 以上。回归标记：

```text
=== RT PI CHAIN: PASS prio_low=80 prio_mid=80 high_lat=8630 ns ===
```

实现取舍：用**一把全局 PI 锁**保护整张“任务-锁-等待者”图，而不是像
Linux 那样逐锁做 `rt_mutex_adjust_prio_chain`。链式调整需要同时改多个
owner 和等待队列，逐锁获取会引入环和竞态；本内核的 RT 锁规模很小，
全局锁换来的是可证明的简单性。锁序仍是 `g_rtm_pi_lock → rq_lock`，
调度器不会反向获取 PI 锁。

### 已知限制

1. 不做死锁检测（A→B→A 会真的睡死）；链深度上限 32，超限只停止传播。
2. 没有超时/可中断等待；持有者退出时也不会自动释放锁。
3. 全局 PI 锁把所有 `rt_mutex` 操作串行化（当前规模下可接受；锁数量
   上来后再考虑按锁分级/逐锁链式调整）。

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
=== RT MUTEX TEST: PASS mtx=18000 err=0 boost=80 after=0 order=1/2/3 chain=80/80/8630 ===
```

`mtx` 是 6 个任务 × 3000 次临界区；`err` 统计同时进入临界区的次数；
`order` 是 high(80)/low(60)/medium(50) 的完成顺序；`chain` 是 PI 链场景
的 `low 优先级 / mid 优先级 / 高优先级拿锁延迟`。
