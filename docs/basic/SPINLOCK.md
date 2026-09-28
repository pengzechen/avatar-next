# 自旋锁（spinlock）

> **一句话**：加锁只用 `include/spinlock.h`；**中断状态存在调用点的局部变量里**，
> 不要存进锁对象。中断屏蔽本身见 [INTERRUPT_MASKING.md](INTERRUPT_MASKING.md)。

## 锁类型：只有一种

```c
typedef struct { volatile uint64_t lock; } spinlock_t;

/* 兼容别名：类型与 spinlock_t 完全相同，名字用来表达"这把锁配 irqsave 用" */
typedef spinlock_t spinlock_noirq_t;

spinlock_t l1 = SPINLOCK_INIT;
spinlock_noirq_t l2 = SPINLOCK_NOIRQ_INIT;   /* == SPINLOCK_INIT */
spinlock_init(&l1);
spinlock_irq_init(&l2);
```

**为什么只有一个结构**：以前 `spinlock_noirq_t` 里多一个 `irq_flags` 字段，
`spin_lock_irqsave()` 把调用者的中断状态写进**锁对象**、解锁时读回来。那把锁被
两颗 CPU 争抢时，后写者的 flags 会覆盖先写者的 —— 解锁时恢复的是**别人的**中断
状态（该开的没开、该关的关了）。现在按 Linux 的做法由调用点保存：

```c
uint64_t flags;
spin_lock_irqsave(&lock, &flags);
...
spin_unlock_irqrestore(&lock, flags);
```

## 接口

| 接口 | 说明 |
|---|---|
| `spin_lock(l)` / `spin_unlock(l)` | 基本自旋锁，**不动中断状态** |
| `spin_lock_irqsave(l, &flags)` | 关中断 + 加锁；旧中断状态写进 `flags` |
| `spin_unlock_irqrestore(l, flags)` | 解锁 + 恢复中断状态 |
| `spin_trylock(l)` | 非阻塞，返回 0 = 成功 |
| `spin_trylock_irqsave(l, &flags)` | 同上带中断保护（**目前无调用者**） |
| `spinlock_init` / `spinlock_irq_init` | 初始化（静态初始化用 `SPINLOCK_INIT`） |

## 各架构实现

| | 加锁 | 解锁 |
|---|---|---|
| aarch64 | `ldaxr` / `cbnz` / `stlxr` 自旋（先 `preempt_disable()`） | `stlr wzr`（stlr 自带 release 语义，无需额外 dmb） |
| riscv64 | `lr.w` / `sc.w` 自旋 + `fence rw, rw` | `fence rw, rw` + `sw zero` |
| x86_64 | `lock xchg` 自旋（本身就是全屏障）+ `mfence` | `mfence` + `movl $0` |

`spin_lock`/`spin_unlock` 内含 `preempt_disable()` / `preempt_enable()`（后者只在
"中断已开、且需要重调度"时才真正切换 —— 见 `kernel/task/preempt.c`）。

## 调用点分布（约 60 处）

| 位置 | 用途 |
|---|---|
| `lib/klog.c` | `g_klog_lock`：整条日志持锁输出（格式化在锁外做） |
| `kernel/mm/pmm.c` | `pmm->lock`：最热的锁；**日志一律在解锁之后打** |
| `kernel/task/sched.c` | `rq_lock`：run_queue；跨核用 irqsave，本核临界区用 `spin_lock`（外层已关中断） |
| `kernel/syscall/fs/tty.c` | 环形缓冲 + 硬件 FIFO 两把锁，从不嵌套持有 |
| `driver/uart/uart_dw.c` | TX/RX 缓冲，ISR 与线程都取 → 全部 irqsave |
| `driver/uart/uart_pl011.c` | 同上（**同一子系统两个驱动两套做法的问题已修**） |
| `driver/ion/ion.c` | `g_ion_lock`，日志同样放在解锁后 |
| `kernel/syscall/core/futex.c` | `g_futex_lock` 保护全局等待表（原来只用本核关中断，SMP 下会写坏） |

## 必须遵守的几条

1. **ISR 里取的锁必须是 irqsave 变体**。线程持普通 `spin_lock` 时被同核中断打断，
   ISR 再去取同一把锁 → 自旋到死。`uart_pl011.c` 曾经就是这样（ISR 注册被注释掉才
   没炸）；对照 `uart_dw.c` 的正确写法。
2. **锁对象里不放任何"调用者状态"**（flags、任务指针……）。这类状态一律放调用点
   的局部变量或 per-CPU 结构。上面的 `irq_flags` 就是反例。
3. **不要在持锁（尤其关中断）期间打日志**。klog 会持自己的锁把整条消息逐字符轮询
   写 UART，把这段延迟塞进 `pmm->lock` 这种热点锁里代价很大；更糟的是如果打印路径
   又回头调用了你手里的锁（UART 驱动的"TX 满"分支曾经 `logger_warn` → klog 重入
   同一把锁）就是同核自死锁。PMM 与 ion 的日志都已挪到解锁之后。
4. **不要在持锁期间让出 CPU**（`sched_schedule()`、睡眠、等设备）。需要睡眠的用
   `mutex_t`（`kernel/task/mutex.h`，另一套词汇，语义是睡眠锁）。
5. **锁顺序要一致**。目前没有发现嵌套持有多把锁的地方（`tty.c` 两把锁是先后取、
   不重叠），新增嵌套时请保持全局一致的顺序，并在注释里写明。
6. **"关中断 + 加锁"用 `spin_lock_irqsave`**，不要手写成 `arch_irq_save()` +
   `spin_lock()`（除非像 `sched.c` 那样，临界区跨越多个加锁点、必须由外层统一
   管理中断状态 —— 那里有注释说明）。

## 未来可能需要的

- 排队/票据锁（当前实现无公平性，极端争抢下可能饿死某颗 CPU）；
- 读写锁（现在没有 `rwlock`，也没有需求）。
