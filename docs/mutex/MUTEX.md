# Mutex 睡眠锁实现

## 概述

`mutex.h`/`mutex.c` 实现了基于任务阻塞的睡眠锁（Mutex），用于内核任务间的互斥同步。

## 设计特点

### 1. 睡眠锁 vs 自旋锁

| 特性 | 自旋锁 (spinlock) | 睡眠锁 (mutex) |
|------|------------------|---------------|
| **等待方式** | 忙等待（CPU 空转） | 任务阻塞（让出 CPU） |
| **适用场景** | 短时间锁定、多核 | 长时间锁定、单核 |
| **开销** | 高 CPU 占用 | 上下文切换开销 |
| **中断安全** | 必须关中断 | 不能在中断处理中使用 |
| **实现** | `include/spinlock.h` | `kernel/task/mutex.h` |

### 2. FIFO 等待队列

- 先等待的任务先被唤醒（公平性）
- 使用 `list_t` 双向链表实现
- 每个任务使用 `wait_node` 字段加入等待队列

### 3. 不可重入

- 同一任务重复获取锁会检测死锁
- 不支持递归锁（递归锁需要计数器）

## API 文档

### 初始化

```c
void mutex_init(mutex_t *mutex);
```

**使用前必须调用**，初始化锁的内部状态。

### 获取锁

```c
void mutex_lock(mutex_t *mutex);
```

- 如果锁可用，立即获取并返回
- 如果锁已被持有，当前任务阻塞并进入等待队列
- 当锁被释放时，任务被唤醒并重新尝试获取

**注意**：会阻塞当前任务，不能在中断处理程序中使用。

### 释放锁

```c
void mutex_unlock(mutex_t *mutex);
```

- 释放锁并唤醒等待队列中的第一个任务
- 当前任务必须是锁的持有者（否则记录错误）
- 如果没有等待者，仅释放锁

### 尝试获取

```c
bool mutex_trylock(mutex_t *mutex);
```

- 如果锁可用，获取并返回 `true`
- 如果锁已被持有，立即返回 `false`，不阻塞

### 查询状态

```c
bool mutex_is_locked(mutex_t *mutex);
task_t *mutex_holder(mutex_t *mutex);
```

用于调试和监控。

## 实现细节

### 数据结构

```c
typedef struct mutex {
    volatile bool    locked;      /* 锁状态 */
    list_t           wait_queue;  /* 等待队列 */
    task_t          *holder;      /* 持有者（调试用） */
} mutex_t;
```

### 获取锁流程

```
mutex_lock()
  ├─ 关中断保护临界区
  ├─ while (locked) {
  │   ├─ 死锁检测
  │   ├─ task_block(&wait_queue)
  │   │   ├─ state = TASK_BLOCKED
  │   │   ├─ 加入 wait_queue
  │   │   └─ sched_schedule()  ← 阻塞，让出 CPU
  │   └─ [被唤醒后重新检查]
  │   }
  ├─ locked = true
  ├─ holder = current
  └─ 开中断
```

### 释放锁流程

```
mutex_unlock()
  ├─ 关中断保护临界区
  ├─ 检查持有者
  ├─ locked = false
  ├─ holder = NULL
  ├─ if (!wait_queue.empty) {
  │   ├─ 从队列取第一个任务
  │   └─ task_unblock(task)
  │       ├─ state = TASK_READY
  │       └─ sched_enqueue(task)  ← 加入就绪队列
  │   }
  └─ 开中断
```

### 死锁检测

```c
if (mutex->holder == cur) {
    KLOG_ERROR("[mutex] DEADLOCK: task '%s' trying to re-acquire!\n",
              cur->name);
    /* 死循环 */
    while (1) {
        task_yield();
    }
}
```

检测到同一任务重复获取锁时，记录错误并进入死循环。

## 使用示例

### 基本用法

```c
static mutex_t g_mutex;
static int g_shared_counter = 0;

void task_a(void *arg) {
    for (int i = 0; i < 100; i++) {
        mutex_lock(&g_mutex);

        /* 临界区：访问共享资源 */
        g_shared_counter++;
        KLOG_INFO("counter = %d\n", g_shared_counter);

        mutex_unlock(&g_mutex);

        task_yield();  /* 让出 CPU */
    }
}

void task_b(void *arg) {
    for (int i = 0; i < 100; i++) {
        mutex_lock(&g_mutex);

        /* 临界区 */
        g_shared_counter++;

        mutex_unlock(&g_mutex);

        task_yield();
    }
}

int main() {
    mutex_init(&g_mutex);

    task_create("task_a", task_a, NULL, 1);
    task_create("task_b", task_b, NULL, 1);

    /* idle 循环 */
    while (1) {
        task_yield();
    }
}
```

### 使用 trylock

```c
void task(void *arg) {
    mutex_t *mutex = (mutex_t *)arg;

    /* 尝试获取锁，不阻塞 */
    if (mutex_trylock(mutex)) {
        /* 成功获取锁 */
        /* ... 临界区 ... */
        mutex_unlock(mutex);
    } else {
        /* 锁已被持有，做其他事情 */
        KLOG_INFO("Lock busy, will try later\n");
    }
}
```

### 保护共享数据结构

```c
typedef struct {
    mutex_t lock;
    int     value;
} protected_int_t;

void protected_increment(protected_int_t *p) {
    mutex_lock(&p->lock);
    p->value++;
    mutex_unlock(&p->lock);
}

int protected_get(protected_int_t *p) {
    mutex_lock(&p->lock);
    int value = p->value;
    mutex_unlock(&p->lock);
    return value;
}
```

## 任务状态转换

```
任务 A 持有锁，任务 B 尝试获取：

task_b: mutex_lock()
  ├─ locked = true (由 task_a 持有)
  ├─ task_block(&wait_queue)
  │   └─ state: READY → BLOCKED
  │   └─ 加入 wait_queue
  └─ sched_schedule()
      └─ 切换到其他任务

task_a: mutex_unlock()
  ├─ locked = false
  ├─ 从 wait_queue 取出 task_b
  ├─ task_unblock(task_b)
  │   ├─ state: BLOCKED → READY
  │   └─ 加入就绪队列
  └─ sched_schedule()
      └─ 可能切换到 task_b

task_b: 被调度
  └─ 重新检查 locked，成功获取
```

## 调试支持

### 日志输出

```c
KLOG_DEBUG("[mutex] task 'task_a' (id=1) waiting for mutex\n");
KLOG_DEBUG("[mutex] task 'task_a' (id=1) acquired mutex\n");
KLOG_DEBUG("[mutex] task 'task_a' (id=1) releasing mutex\n");
KLOG_DEBUG("[mutex] waking task 'task_b' (id=2)\n");
```

### 错误检测

1. **死锁检测**：同一任务重复获取
2. **非法释放**：非持有者尝试释放
3. **状态检查**：查询锁是否被持有

### 持有者信息

```c
task_t *holder = mutex_holder(&g_mutex);
if (holder) {
    KLOG_INFO("Lock held by '%s' (id=%u)\n", holder->name, holder->id);
}
```

## 限制和注意事项

### 1. 不能在中断处理中使用

```c
/* ❌ 错误：在 IRQ handler 中使用 */
void timer_handler() {
    mutex_lock(&g_mutex);  /* 会死锁！中断中不能阻塞 */
}
```

**原因**：
- 中断处理程序没有对应的 `task_t`
- `task_block()` 会尝试访问 `g_current_task`，可能指向任意任务
- 中断上下文中阻塞会导致系统挂起

**正确做法**：使用自旋锁 (`spinlock.h`)

```c
/* ✅ 正确：中断中使用 spinlock */
void timer_handler() {
    spin_lock(&g_lock);
    /* ... 临界区 ... */
    spin_unlock(&g_lock);
}
```

### 2. 不可重入

```c
void recursive_function() {
    mutex_lock(&g_mutex);

    /* ... 做一些事情 ... */

    recursive_function();  /* ❌ 死锁！ */

    mutex_unlock(&g_mutex);
}
```

**解决方案**：
- 避免递归调用
- 或实现可重入锁（需要递归计数）

### 3. 忘记释放锁

```c
void bad_function() {
    mutex_lock(&g_mutex);

    if (error_condition) {
        return;  /* ❌ 忘记释放锁！ */
    }

    mutex_unlock(&g_mutex);
}
```

**正确做法**：

```c
void good_function() {
    mutex_lock(&g_mutex);

    if (error_condition) {
        mutex_unlock(&g_mutex);  /* 确保释放 */
        return;
    }

    mutex_unlock(&g_mutex);
}
```

## 测试

运行测试：

```c
run_mutex_tests();
```

测试内容：
1. 多个任务竞争同一把锁
2. 共享计数器的互斥访问
3. `trylock` 功能测试
4. 等待队列的 FIFO 顺序

预期输出：
```
[INFO] === Mutex Test ===
[INFO] [mutex_test] mutex initial state: unlocked ✓
[DEBUG] [mutex] task 'task_a' (id=1) acquired mutex
[INFO] [task_a] counter = 1
[DEBUG] [mutex] task 'task_a' (id=1) releasing mutex
[DEBUG] [mutex] task 'task_b' (id=2) waiting for mutex
[DEBUG] [mutex] task 'task_b' (id=2) acquired mutex
[INFO] [task_b] counter = 2
...
```

## 性能考虑

### 1. 上下文切换开销

- 每次阻塞/唤醒涉及 `sched_schedule()`
- 包括保存/恢复寄存器、调度决策等
- **适合长时间锁**，不适合短时间锁

### 2. 等待队列开销

- 每个等待任务占用 `wait_node`（两个指针）
- FIFO 公平性可能影响性能（优先级低的任务先等待会先执行）

### 3. 中断开关开销

```c
uint64_t flags = arch_irq_save();  /* 关中断 */
/* ... 临界区 ... */
arch_irq_restore(flags);            /* 开中断 */
```

每次 `lock`/`unlock` 都会关/开中断，增加延迟。

## 与自旋锁的选择

| 场景 | 推荐锁 | 原因 |
|------|--------|------|
| 保护短临界区（< 微秒级） | Spinlock | 上下文切换开销 > 忙等待开销 |
| 保护长临界区（> 毫秒级） | Mutex | 避免 CPU 空转 |
| 单核系统 | Mutex | 忙等待浪费整个 CPU |
| 多核系统 | Spinlock (短) / Mutex (长) | 根据临界区长度选择 |
| 可能睡眠的操作 | Mutex | 任务必须阻塞 |
| 中断处理程序 | Spinlock | 不能阻塞 |
| 任务间同步 | Mutex | 可以阻塞等待 |

## 扩展可能

### 1. 可重入锁（递归锁）

```c
typedef struct {
    mutex_t base;
    int     recursion_count;
} recursive_mutex_t;
```

### 2. 读写锁

```c
typedef struct {
    mutex_t mutex;
    int     readers;
    int     writers;
} rwlock_t;
```

### 3. 优先级继承

防止优先级反转问题：
- 低优先级任务持有锁
- 高优先级任务等待锁
- 临时提升低优先级任务的优先级

### 4. 超时机制

```c
bool mutex_lock_timeout(mutex_t *mutex, uint64_t timeout_ms);
```

## 参考资料

- 《操作系统导论》(OSTEP) - 锁章节
- Linux Kernel: `mutex` 实现
- 《并发编程的艺术》

---

**文档版本**: 1.0
**创建日期**: 2026-05-02
**作者**: Avatar OS Team
