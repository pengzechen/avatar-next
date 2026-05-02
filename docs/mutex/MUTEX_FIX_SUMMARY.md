# Mutex 竞态条件修复总结

## 问题背景

在实现 mutex（睡眠锁）时，发现了多个严重的竞态条件bug，导致：
- 数据损坏（balance不是50的倍数）
- 丢失操作（withdrawals计数不对）
- Lost updates（balance与预期严重不符）

## 关键Bug修复

### 1. Mutex Unlock 的竞态条件 ⚠️ **最严重**

**问题代码**：
```c
void mutex_unlock(mutex_t *mutex)
{
    ...
    /* 释放锁 */
    mutex->locked = false;      // ❌ 锁已释放！
    mutex->holder = NULL;

    /* 唤醒等待队列中的第一个任务 */
    if (!list_is_empty(&mutex->wait_queue)) {
        list_node_t *node = list_delete_first(&mutex->wait_queue);  // ❌ 还在操作wait_queue！
        ...
    }
    ...
}
```

**问题**：在释放锁（`locked = false`）之后，还在操作 `wait_queue`。其他任务可以立即获取锁并访问 `wait_queue`，导致多个任务同时修改链表，造成数据损坏。

**修复**：
```c
void mutex_unlock(mutex_t *mutex)
{
    ...
    /* 先从等待队列中取出第一个任务（在锁释放前操作 wait_queue） */
    task_t *waiter = NULL;
    if (!list_is_empty(&mutex->wait_queue)) {
        list_node_t *node = list_delete_first(&mutex->wait_queue);
        waiter = container_of(node, task_t, wait_node);
    }

    /* 释放锁（现在可以安全释放了，wait_queue 已操作完毕） */
    mutex->holder = NULL;
    barrier_compiler();  // 编译器屏障，确保操作顺序
    mutex->locked = false;

    /* 恢复中断 */
    arch_irq_restore(flags);

    /* 最后唤醒等待的任务（锁已释放，任务可以立即竞争） */
    if (waiter != NULL) {
        task_unblock(waiter);
    }
}
```

**修复要点**：
1. 先操作 `wait_queue`（此时锁还被持有）
2. 再释放锁（`locked = false`）
3. 最后唤醒任务

---

### 2. Mutex Lock 的中断状态管理

**问题代码**：
```c
void mutex_lock(mutex_t *mutex)
{
    uint64_t flags = arch_irq_save();

    while (mutex->locked) {
        ...
        task_block(&mutex->wait_queue);

        /* 被唤醒后重新检查 */
        flags = arch_irq_save();  // ❌ 覆盖了原来的flags！
    }
    ...
    arch_irq_restore(flags);  // ❌ 恢复错误的中断状态
}
```

**问题**：当任务被唤醒后，`flags = arch_irq_save()` 覆盖了最初保存的中断状态。最终恢复的是错误的状态，可能导致中断被错误地禁用或启用。

**修复**：
```c
void mutex_lock(mutex_t *mutex)
{
    uint64_t flags = arch_irq_save();

    while (mutex->locked) {
        ...
        /* 恢复中断，允许调度器工作 */
        arch_irq_restore(flags);
        task_block(&mutex->wait_queue);
        /* task_block 会切换到其他任务，被唤醒后重新获取锁 */
        flags = arch_irq_save();  // ✓ 重新保存当前中断状态

        /* 被唤醒后重新检查锁是否可用 */
    }
    ...
    arch_irq_restore(flags);  // ✓ 恢复正确的状态
}
```

**修复要点**：
- 在 `task_block()` 前恢复中断
- 被唤醒后重新保存中断状态
- 这样最终恢复的状态是正确的

---

### 3. 内存屏障防止编译器重排

**添加位置**：

**a) Mutex 获取/释放**：
```c
// mutex_lock
mutex->locked = true;
barrier_compiler();  // 确保 locked 在 holder 之前写入
mutex->holder = cur;

// mutex_unlock
mutex->holder = NULL;
barrier_compiler();  // 确保 holder 在 locked 之前清空
mutex->locked = false;
```

**b) 调度器**：
```c
next->state = TASK_RUNNING;
barrier_compiler();  // 确保 state 在 g_current_task 之前完成
g_current_task = next;
barrier_compiler();  // 确保 g_current_task 在 arch_task_switch 之前完成
```

**c) task_current()**：
```c
task_t *task_current(void)
{
    barrier_compiler();  // 编译器屏障，确保每次都重新读取
    return g_current_task;
}
```

**作用**：
- 防止编译器优化导致指令重排
- 确保关键操作按正确顺序执行
- 保证多任务环境下的一致性

---

### 4. 测试逻辑修正

#### a) 验证逻辑错误

**问题代码**：
```c
/* 验证：余额应该是 100 的倍数 */
if (g_account_locked.balance % 100 != 0) {  // ❌ 错误！
    g_account_locked.errors++;
}
```

**问题**：Deposit加100，Withdraw减50。并发执行时，balance可能是150、250等（50的倍数但不是100的倍数）。

**修复**：
```c
/* 验证：余额应该是 50 的倍数（deposit +100, withdraw -50）*/
if (g_account_locked.balance % 50 != 0) {  // ✓ 正确
    g_account_locked.errors++;
}
```

#### b) 预期余额计算错误

**问题代码**：
```c
int32_t expected = (int32_t)account->deposits * 100 -
                   (int32_t)account->withdrawals * 50;  // ❌ 忘记初始余额
```

**修复**：
```c
int32_t expected = 1000 +  // ✓ 初始余额
                   (int32_t)account->deposits * 100 -
                   (int32_t)account->withdrawals * 50;
```

#### c) 无法检测Lost Updates

**问题**：只检查 `balance % 50 != 0`，无法检测到丢失的更新操作。

**修复**：添加三种错误检测：
```c
/* 1. 验证错误（balance不是50的倍数）*/
if (account->errors > 0) { ... }

/* 2. 丢失操作（deposits/withdrawals计数不正确）*/
if (account->deposits != expected_deposits ||
    account->withdrawals != expected_withdrawals) { ... }

/* 3. Balance不正确（lost updates导致）*/
if (difference != 0) { ... }
```

#### d) 竞态窗口太小

**问题**：使用延迟循环模拟竞态，但循环太长，一个任务完成整个critical section才被切换。

**修复**：在 read-modify-write 中间强制 yield：
```c
/* ❌ 无锁版本 */
uint32_t old_balance = g_account_no_lock.balance;
task_yield();  // 🔴 在read和write之间强制切换，制造竞态窗口
g_account_no_lock.balance = old_balance + 100;

/* ✅ 有锁版本 */
mutex_lock(&g_account_mutex);
uint32_t old_balance = g_account_locked.balance;
g_account_locked.balance = old_balance + 100;  // ✓ 原子执行
mutex_unlock(&g_account_mutex);
task_yield();  // 在mutex外yield，让其他任务竞争锁
```

---

## 测试结果对比

### Phase 1 (NO LOCK) - 严重的竞态条件
```
Final balance: 3600
Total deposits: 1000 ✓
Total withdrawals: 990 ❌ (丢失了10次withdraw操作！)
Expected balance: 51500
Difference: -47900 ❌

❌ LOST OPERATIONS: Expected 1000 deposits/1000 withdrawals, got 1000/990
❌ LOST UPDATES: Balance off by -47900
```

### Phase 2 (WITH LOCK) - 完美保护
```
Final balance: 51000 ✓
Total deposits: 1000 ✓
Total withdrawals: 1000 ✓
Expected balance: 51000 ✓
Difference: 0 ✓

✓ All operations completed correctly
```

---

## 核心教训

### 1. 锁的释放顺序至关重要
- **先操作共享数据结构，再释放锁**
- 绝不能在释放锁后还访问受保护的数据

### 2. 中断状态管理要小心
- 保存/恢复中断状态要配对
- 任务切换后会改变中断状态，需要重新保存

### 3. 内存屏障不能少
- 编译器可能重排指令
- 关键操作之间需要内存屏障保证顺序

### 4. 测试要触发真正的竞态
- 延迟循环不够，要在critical section中间切换任务
- 验证逻辑要全面，不能只检查数据一致性

### 5. 错误检测要全面
- 检查数据一致性（balance % 50）
- 检查操作计数（deposits/withdrawals）
- 检查最终结果（balance == expected）

---

## 修改文件清单

1. **kernel/task/mutex.c** - 核心修复
   - mutex_unlock() 操作顺序调整
   - mutex_lock() 中断状态管理
   - 添加内存屏障

2. **kernel/task/task.c**
   - task_current() 添加内存屏障
   - 添加 #include "barrier.h"

3. **kernel/task/sched.c**
   - g_current_task 更新处添加内存屏障
   - 添加 #include "barrier.h"

4. **tests/mutex_comparison_test.c**
   - 修正验证逻辑（% 50 而不是 % 100）
   - 修正预期余额计算
   - 增强错误检测（三种类型）
   - 在critical section中间yield触发竞态
   - 增加任务数和迭代次数

---

## 总结

通过以上修复，mutex 实现现在能够：
- ✅ 正确保护共享数据，防止竞态条件
- ✅ 避免 lost updates 和操作丢失
- ✅ 在高并发压力下保持数据一致性
- ✅ 清晰展示有锁vs无锁的区别

**关键点**：锁的实现细节至关重要，任何一个小的疏忽都可能导致严重的并发bug。
