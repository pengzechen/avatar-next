# SMP：每核 `preempt_schedule_depth` 被已退出任务永久占住，整机失去 trap 抢占

**日期**: 2026-11-24
**影响架构**: x86_64 / aarch64 / riscv64（SMP>1；SMP=1 不复现）
**严重性**: 高（随机整机停摆，`rt-trace` 固定 240s 超时）
**触发条件**: 多核 + 有任务在 `task_exit()` 里恰好被 timer/IPI 抢占，例如
`make -C tests rt-trace ARCHES=x86_64 SMP=4`（KVM 下约 10%~20% 命中）

---

## 一、症状

`rt-trace` 自测跑到 `RT EVENT TEST: PASS` 后串口再无输出，测试框架等
`PREEMPTOFF TEST: PASS` 一直等到 240s 超时：

```
[INFO][C0] tests/unit-test/rt_event_test.c:485: === RT EVENT TEST: PASS ...
  [TIMEOUT  ] x86_64   rt-trace/trace (240.1s)
```

用 gdb 接上挂死的 QEMU 看 CPU0：

```
Thread 1.1 (CPU#0 [running]) cyclictest_hog at tests/unit-test/cyclictest.c:195
```

* CPU0 正在跑 `cyclictest` 的 NORMAL 忙循环 `cycli-hog`（prio 10），
  它不会主动让出 CPU；
* FIFO 80 的 `cyclictest` 任务 `state=READY`，安静地躺在 CPU0 的
  `rt_queue[80]` 里（`rt_bitmap[1]=0x10000`），`sleep_expire_ns=0`
  （定时器已经把它唤醒并入队了）；
* `g_cpus[0].need_resched == 1`、**`g_cpus[0].preempt_schedule_depth == 1`**；
* CPU0 的 timer 仍在正常走（`local_ticks` 每 1.5s +150），
  但 `trap_preempt_kernel_side()` 每次都在 `preempt_schedule_depth != 0`
  处返回 `false`。

即：RT 任务已经就绪，定时器也把它唤醒了，但这颗核**拒绝从 trap 返回路径
抢占**，于是高优先级任务永远等不到 CPU，boot 上下文（idle）也永远回不来。

## 二、根因：重入深度记在了 cpu_t 上

`preempt_schedule_depth` 原本是 `cpu_t` 里的 per-CPU 计数，用来防止
`preempt_enable()` / trap 返回路径把自己递归进 `sched_schedule()`：

```c
c->preempt_schedule_depth++;
sched_schedule();     /* ← 这里可能把当前任务切走 */
c->preempt_schedule_depth--;   /* 等本任务再次被调度回来才执行 */
```

问题在于 `sched_schedule()` 会切换上下文：一旦切走，这个 `--` 就挂在
**被切走那个任务的栈帧**里，要等它再次被调度回来才执行。正常情况下没问题。
但 `task_exit()` 有一条特殊窗口：

```c
void task_exit(void)
{
    ...
    cur->state = TASK_DEAD;       /* 1. 先发布 DEAD */
    task_exit_notify(cur);
    sched_dequeue(cur);
    sched_schedule();             /* 2. 真正切走（不会回来）*/
    while (1)
        ;
}
```

第 1 步之后、第 2 步之前，中断是开着的。若这时来一次 timer tick 或
resched IPI，trap 返回路径会执行 `trap_preempt_kernel_side()`：

1. 看到 `need_resched`、`preemptible()`、`preempt_schedule_depth == 0`，
   于是 `preempt_schedule_depth++`，调用 `sched_schedule()`；
2. `sched_schedule()` 发现当前任务 `state == TASK_DEAD`，**不会把它重新
   入队**，直接切到下一个任务；
3. 死任务从此不会被调度回来，`preempt_schedule_depth--` 永远不会执行；
4. 死任务的栈随后按 `switch_done` 被回收/复用，这个计数再也无人清理。

于是**这颗 CPU 的 `preempt_schedule_depth` 永远是 1**，之后所有
trap 返回路径的抢占都被拒绝。`cyclictest` 阶段正好有一个不会 yield 的
NORMAL 忙循环（`cycli-hog`）等着被 FIFO 80 抢占，于是直接死锁。

实测抓到的调度记录（临时跟踪环，`state=3` 即 `TASK_DEAD`）：

```
tp 8315 mtx-str(state=3) -> mtx-str psd=1
```

即：`mutex` 压力测试里某个任务在 `TASK_DEAD` 状态被 trap 抢占切走，
`psd` 从 0 变成 1 之后再无 1→0。之后的 IPC/event 自测之所以还能过，
是因为它们全靠 `task_block()`/`task_yield()` 主动切换，不需要 trap 抢占；
到了 `cyclictest` 才暴露。

## 三、修复：把重入深度挪进 task_t

重入深度本质上是"**当前任务**正在执行 `sched_schedule()`"的标记，
属于任务上下文，不属于 CPU —— Linux 的 `preempt_count` 也放在每线程的
`thread_info` 里。修复只改三处：

```c
/* include/task/task.h */
uint32_t preempt_count;
uint32_t preempt_schedule_depth;   /* 新增：随任务切换 */

/* kernel/task/preempt.c + kernel/task/sched.c */
cur->preempt_schedule_depth++;
sched_schedule();
cur->preempt_schedule_depth--;
```

`cpu_t` 里删掉该字段。新任务由 `task_alloc_slot()` 的 `memset(task, 0,
sizeof(*task))` 清零（fork 路径也走这里），所以首次运行天然是 0；死任务
即使把自己的 `preempt_schedule_depth` 留在 1，也只影响它自己（而且它已经
DEAD），不会再污染整颗核。

保留的语义：

* **同一任务**在 `sched_schedule()` 返回后、`--` 之前的窗口里，仍然不会
  被递归调度（`cur->preempt_schedule_depth != 0`）；
* 一条任务被切走后，另一条任务在这颗核上照常可以被 trap 抢占 ——
  它有自己的、通常为 0 的计数。

`cpu_t` 布局不受影响：删掉 4 字节后 `local_ticks` 仍 8 字节对齐，
`scratch_rsp`/`kernel_rsp0` 仍在 96/104，`include/task/cpu.h` 里的
`static_assert` 会继续把关。

## 四、回归

修复前 `x86_64 SMP=4 rt-trace` 自测约 10%~20% 概率卡死（同一条命令连续跑
十几次必中一次）。修复后用同一内核连续启动 **80 次**，80/80 都跑到
`PREEMPTOFF TEST: PASS`，`rt-trace` suite 也恢复正常。

## 五、备注：为什么不在 task_exit 里关中断

在 `task_exit()` 里把 `state = TASK_DEAD` 到 `sched_schedule()` 之间用
`cli` 包起来也能堵住这个特定窗口，但同类问题不止一处：任何"任务被切走且
不再回来"的路径（`sched_kill_task()`、异常路径置 DEAD 等）都会重演。
把重入深度变成任务私有状态，才是与上下文切换语义一致的做法。
