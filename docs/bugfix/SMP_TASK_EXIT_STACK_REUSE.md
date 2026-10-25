# SMP：任务退出栈被父进程提前回收，运行中的内核栈被刷成 0xA5

**日期**: 2026-10-25
**影响架构**: x86_64 / aarch64 / riscv64（SMP>1；SMP=1 不复现）
**严重性**: 高（随机整机挂死，LTP `getpid01` 几乎必中）
**触发条件**: 多核 + 连续 `fork`/`wait4`，例如
`make -C tests ltp-full ARCHES=x86_64 SMP=4`

---

## 一、症状

`getpid01` 连续 fork 100 次、每次子进程 `getpid()` 后退出。跑到第 10~20 个
子进程时：

```
getpid01.c:32: TPASS: getpid() returns 132
[ERROR][C3] CPU exception #13 at RIP=0xffff800000234186 EC=0x0
[ERROR][C3]   RAX=0xa5a5a5a5a5a5a5a5 RBX=0x0 ...
```

之后串口停在半行 `[ERROR]`，整机挂死（异常打印本身需要 UART 锁，另一个核
也已经在异常路径上）。`SMP=1` 下 100 次全过。

反汇编定位：

```
ffff800000234182: mov -0x38(%rbp),%rax    /* preempt_enable 的函数地址 */
ffff800000234186: call *%rax              /* RAX = 0xa5a5... → #GP */
```

这个 `call` 在 `sched_dequeue()` 内、`spin_unlock_irqrestore()` 的
`preempt_enable()` 处——**栈上保存的代码地址被改成了 0xA5**。

## 二、根因：DEAD ≠ 已经离开内核栈

`0xA5` 是 `TASK_STACK_MAGIC`，只在 `task_alloc_slot()` 认领槽位时出现：

```c
memset(task->stack_base, TASK_STACK_MAGIC, TASK_STACK_SIZE);
```

而旧的退出顺序是：

```c
void task_exit(void)
{
    cur->state = TASK_DEAD;            /* 1. 发布 DEAD */
    notify_parent_wait_from_task(cur); /* 2. 唤醒父进程 */
    sched_dequeue(cur);                /* 3. 仍在本任务内核栈上 */
    sched_schedule();                  /* 4. 这里才 arch_task_switch 切走 */
}
```

SMP>1 时的竞态：

```
CPU A (子进程)                         CPU B (父进程)
state = DEAD
notify_parent()  ───────────────────►  wait4 看到 DEAD
                                        task_reap_dead() → g_stack_used=0
                                        fork 下一个子进程
                                        task_alloc_slot()
                                        memset(旧子进程的栈, 0xA5, 16KB)
sched_dequeue() / sched_schedule()
  ← 正在用这块栈保存上下文，栈已被刷成 0xA5
```

旧代码的注释认为“DEAD 的槽位由父进程 wait 后再回收，所以切换期间安全”，
漏掉的是：**父进程正是在切换完成之前被唤醒的**。SMP=1 时父进程不可能与
退出中的子进程并发，所以只有多核会中。

## 三、修复：把“逻辑死亡”和“资源可回收”分成两个时刻

> 第一版修复曾用“每 CPU 退出专用栈”先把栈弃用。它确实挡住了 `0xA5`
> 刷栈，但挡不住 **TCB 和页表**：父进程仍可能在子进程执行
> `arch_task_switch` 之前 `vm_destroy_user_process()` 并复用 TCB。
> 最终改成下面的生命周期门控，不再换栈。

新增 `task_t::switch_done`：

1. 调度器每次把任务**切入** CPU 时清零；
2. 三个架构的 `arch_task_switch` 在**真正换到 next 的栈、且地址空间已经
   切换之后**，用 release 语义置 1（x86 `mov`、AArch64 `stlr`、RISC-V
   `fence w,w` + `sw`）；
3. `task_reap_dead()` 在销毁 pgd / 回收槽位之前，用 acquire load 等待
   `switch_done`；
4. `task_alloc_slot()` 的孤儿回收扫描要求 `switch_done` 已置位，否则跳过
   本轮（以后再看）。

这样 `state = TASK_DEAD` 只表示“不在调度队列里”，父进程可以拿到退出码；
但 pgd、内核栈、TCB 要等到任务真正离开 CPU 之后才允许回收。

这与 Linux 的 `do_exit()` → `schedule()` → `finish_task_switch()` 是同一个
思路：Linux 用 `task_struct` 引用计数 + RCU 异步保证“栈/TCB 只在调度器
不再引用后释放”；本内核规模小，用一次同步等待实现同样的顺序。

`switch_done` 的置位点在三个 `switch.S` 里，改上下文切换/伪帧布局时必须
一起看 `kernel/task/task.h` 和 `kernel/task/switch.h` 的注释。

## 四、验证

| 配置 | 修复前 | 修复后 |
|------|--------|--------|
| x86_64 SMP=4 全量 LTP | 首次运行即卡死在 getpid01 | **6/6 全过** |
| x86_64 SMP=4，连续 20 次 getpid01（2000 次 fork/wait） | 会挂 | **0 TFAIL / 0 内核异常** |
| x86_64 SMP=4 smoke（含 getpid01） | 偶发 | **10/10 全过** |
| x86_64 SMP=1 / SMP=8 全量 | 79/79 | 79/79 / 79/79 |
| aarch64 SMP=1 / SMP=4 全量 | — | 79/79 / 79/79 |
| riscv64 SMP=1 / SMP=4 全量 | — | 79/79 / 79/79 |

## 五、教训

1. **“任务已 DEAD”不等于“它的内核栈/页表/TCB 已经没人用”。** 任何释放
   资源的判据都必须落到“已经切出 CPU”这个事件上，而不是状态位。
2. **只保护栈是不够的。** 父进程的 `task_reap_dead()` 还会销毁 pgd、
   `task_alloc_slot()` 会 `memset` 整个 TCB。三者和栈一样，都必须等
   `arch_task_switch` 真正换到 next 之后才能回收。
3. **不要在切换完成之前唤醒会回收该任务资源的其他核。**
   `notify_parent()` 放在 `sched_schedule()` 之前，等于把回收窗口提前打开了；
   现在用 `switch_done` 把这个窗口显式关掉。
4. 看到内核寄存器/栈里出现 `0xa5a5a5a5a5a5a5a5`，先查 `task_alloc_slot()`
   的栈复用，而不是当成普通野指针。
5. 这类竞态用 SMP=1 怎么跑都测不出来；`getpid01` 的连续 fork/wait 是很好的
   回归网，但必须在 SMP>1 下跑。
