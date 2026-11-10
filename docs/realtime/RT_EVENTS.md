# RT 事件：`rt_event` 与 event 模板

> 一句话：**`rt_event` 让 RT 任务在没有事件时真正睡眠；`event` 模板把
> 共享页里的 SPSC 事件环和它连起来，事件到达才执行 step。**

## 一、`rt_event`（`kernel/task/rt_event.h`）

```c
rt_event_t e;
rt_event_init(&e);

/* 线程：等 bit0|bit1 中任意一个，20ms 超时，满足后清位 */
uint32_t bits = rt_event_wait(&e, 0x3, false, 20000000ULL, true);

/* 线程或 ISR：置位并唤醒所有满足 mask 的等待者 */
rt_event_set(&e, 0x1);
```

语义：

- 32 位事件；`wait_all=false` = 任一位，`true` = 全部位；
- `timeout_ns == 0` = 无限等待，否则超时返回 0；
- `clear=true` 时满足后原子清掉请求的 mask；
- `set` 在锁内置位并 `task_unblock()`，所以 ISR 可以调用；
- 等待者对象在 `rt_event_wait()` 的内核栈上，不分配内存；
- 等待者按 `RTW_EVENT_*` mask 过滤，`set` 不会唤醒不相关的任务。

实现要点：

- 检查 bits、入等待队列、置 `TASK_BLOCKED` 都在 `e->lock` 下完成；
  `set` 的置位/出队/唤醒也在同一把锁下，不存在“检查完还没挂上”的
  丢唤醒窗口。
- `set` 唤醒前把 `(bits & mask)` 记到等待者对象里（`matched`）。否则
  另一个 `clear=true` 的等待者可能先清位，被唤醒者会误判为没满足而
  重新睡下去。
- 超时复用 `sleep_expire_ns`：x86_64 由 TSC-deadline hrtimer 唤醒，
  aarch64/riscv64 由 100Hz tick 唤醒。极早期、tick 回调尚未启用的
  环境只能无限等待。

## 二、`event` 模板

### 共享事件环

紧跟在 `struct rtw_shm` 后面，固定偏移 `RTSHM_EVENT_OFFSET == 192`：

```c
struct rtw_event_ring {
    volatile uint32_t head, tail; /* 生产者/消费者自由回绕索引 */
    uint32_t cap;                 /* 64 */
    uint32_t _pad;
    volatile uint64_t posted;     /* 成功入环 */
    volatile uint64_t dropped;    /* 环满丢弃 */
    volatile uint64_t processed;  /* 已处理 */
    volatile uint64_t last_value;
    volatile uint64_t ev[64];
};
```

- SPSC：内核生产者（ioctl / 未来驱动 ISR）由 `w->ev_lock` 串行化，
  RT worker 是唯一消费者；`head` release 发布、`tail` release 回收。
- 环满 **drop-newest**：`RTW_IOC_POST` 返回 `-ENOSPC`，`dropped++`；
  RT 侧不等待、不背压。
- 用户态可以 mmap 共享页读环计数，但**直接写环不会唤醒 worker**；
  投递要走 `rtctl post` / `RTW_IOC_POST`（后续可加 kick 门铃）。

### 数据面

- `period_ns == 0`：worker 阻塞在 `rt_event_wait(DATA|STOP)`，无限等待；
- `period_ns > 0`：作为最长等待/超时（至少 1ms）；
- 每个事件执行一次 `step(event_value)`，参数与 `periodic_io` 相同
  （gain/offset/min/max）；
- 排空后“先清 DATA、再查环”重新武装，避免把刚投递的事件置位清掉；
- `stop` 设置 `RTW_EVENT_BIT_STOP`，阻塞在无限等待里的 worker 也能退出；
- `ticks` = 已处理事件数，`events_posted/processed/dropped` 来自环；
  `budget_ns` 仍按单事件执行时间计 `overruns`。

### 使用

```sh
rtctl create evt event 50 0 0 131072   # period=0, gain=2.0
rtctl start 1
rtctl post 1 21
rtctl post 1 22
rtctl get 1        # value=44 seq=2
rtctl stats 1      # events_posted=2 events_processed=2 events_dropped=0
rtctl stop 1       # 阻塞在事件等待也会被 STOP 唤醒
rtctl destroy 1
```

## 三、测试

```bash
make -C tests rt-test                  # 三架构：rt_event 语义 + event 模板
make -C tests rt-test SMP=4 ARCHES=x86_64
make -C tests apps ARCHES=x86_64      # /bin/rt-event-test 用户态端到端
```

内核场景（`tests/unit-test/rt_event_test.c`）：

- any/all mask、clear、多个等待者的 mask 过滤；
- x86 oneshot 下的 20ms 超时；
- 事件环：先投 100（64 入环/36 drop），start 后处理 64，再投 20，
  stop 阻塞 worker 并确认 STOPPED。

用户态场景（`apps/c/rt_event_test.c`）：

- ioctl create/start/post/stats/mmap/stop/destroy；
- 环容量与 drop 计数、输出值、共享页 magic/version/计数。

## 四、已知限制

- 只有内核侧生产者；用户态直接写环没有唤醒门铃。
- 环容量固定 64，事件无类型/时间戳，只有 u64 payload。
- `clear=true` 是广播语义：同一轮被 `set` 唤醒的多个等待者都会返回并清
  mask，不会只让一个等待者“消费”事件位。
- 不支持多消费者；每个 worker 独占自己的环。
- 没有事件丢失通知之外的重传/确认机制；drop 计数是唯一反馈。
- `rt_event` 超时依赖 tick/hrtimer；早期无 tick 环境只能无限等待。
