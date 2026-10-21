# Task 基础设施的用户态 A/B 验收

> 目标：证明跨核唤醒/kick 基础设施**确实**改变了行为，而不是「编译过了、
> 看起来没问题」。这是多 vCPU 之前必须过的一道门。

## 1. 测什么

测试程序：`apps/c/task_ipc_test.c`（rootfs 里的 `/bin/task-ipc`），
入口脚本：`tools/task_infra_test.sh`。

| 模式 | 做什么 | 判据 |
|---|---|---|
| `dist <n>` | fork N 个子进程，各自用 `getcpu()` 报告所在 CPU | SMP 下应两边都有，例如 `cpu0=4 cpu1=4` |
| `ping <iters> <attempts>` | 父/子用 `MAP_SHARED` 内存 + futex 乒乓 | 跨核往返延迟；有 IPI 应几十~几百 us，没有则是一个 tick（本内核 100Hz = 10ms） |

`getcpu` 是新加的观测 syscall（x86_64 309 → 通用 168），只返回当前逻辑
CPU。它让测试不依赖内核日志。实现见 `kernel/syscall/syscall.c`。

**为什么用 futex 而不是 pipe**：pipe 的 `blocked_reader/blocked_writer`
是无锁检查，存在已知的 lost-wakeup（`racetest pipe` 就是打它）。用它测
task 基础设施，失败时无法区分是 IPI 坏了还是 pipe 丢了唤醒 —— aarch64
实测确实会间歇卡死。futex 的 wait/wake 有全局锁，且新内核里唤醒统一走
`sched_unblock()`，是干净的判据。

**为什么没有 fanout 用例**：用 N 个 pipe 一次唤醒 N 个进程的写法同样被
pipe 的引用计数/EOF 语义污染，测出来的不是唤醒延迟。留 `dist` + `ping`。

## 2. 怎么跑

```bash
# 当前内核
make PLATFORM=qemu-virt-x86_64 SMP=2 clean
make PLATFORM=qemu-virt-x86_64 SMP=2 kernel rootfs
tools/task_infra_test.sh x86_64 2

# 指定内核/rootfs（A/B 用）
KERNEL_BIN=/path/kernel_x86_64.bin \
ROOTFS_IMG=/path/rootfs-x86_64.img \
OUT=/tmp/ab.log \
tools/task_infra_test.sh x86_64 2
```

脚本会启动宿主 shell，逐条执行（**不能一次灌入**：模拟 16550 FIFO 很小，
guest 忙时会丢字符），最后打印所有 `TASKIPC` 行。

基线对照：从 `7ef6b2c`（task 基础设施提交之前）建 worktree，只补
`getcpu` 观测与同一个测试程序，其余保持旧实现：

```bash
git worktree add --detach /tmp/avatar-baseline 7ef6b2c
# 把 getcpu 的 syscall.c/syscall_internal.h 改动、apps/c/task_ipc_test.c
# 和 Makefile 的 task-ipc 规则复制进去，然后：
cd /tmp/avatar-baseline
make PLATFORM=qemu-virt-x86_64 SMP=2 kernel rootfs
```

## 3. 实测结果（2026-10-22）

环境：x86_64 用 KVM；aarch64/riscv64 用 QEMU TCG（绝对数偏高，看数量级）。

### x86_64 SMP=2（KVM）

| 内核 | dist | ping 跨核 | ping 同核 |
|---|---|---|---|
| 新 | cpu0=4 cpu1=4 | **16.0 / 16.6 us** | 13.2 us |
| 基线 7ef6b2c | cpu0=4 cpu1=4 | **9956.6 / 9956.0 us** | 16.1 us |

跨核唤醒从「每轮等一个 10ms tick」变成 ~16us，约 **600 倍**；
同核路径没有退化。

### aarch64 SMP=2（TCG）

| 内核 | ping 跨核 | ping 同核 |
|---|---|---|
| 新 | 109.7 / 101.4 us（100 轮 x3 完整跑完） | 67.9 us |
| 基线 | 6785.5 us（20 轮） | — |

aarch64 的这个测试抓到了两个真问题：

1. `gicv3_ipi_send_single()` 的 `ICC_SGI1R_EL1` 编码错误（INTID 应在
   [27:24]、TargetList 在 [15:0]，旧代码写成了 [3:0] / [16+cpu]）。
   修复前即使发 IPI 也收不到，跨核延迟仍是 8ms；修复后 ~100us。
2. `sched_schedule()` 兑现 `wake_pending` 后没有置本核 `need_resched`，
   切到 idle 后会直接 wfi/hlt，任务要等下一次 tick；tick 异常时表现为
   间歇卡死。修复后 100 轮 x3 稳定跑完。

### riscv64 SMP=2（TCG）

| 内核 | ping 跨核 | ping 同核 |
|---|---|---|
| 新 | 126.1 / 107.5 us（100 轮 x3 完整跑完） | 49.9 us |
| 基线 | 9723.5 us（20 轮） | — |

SBI IPI 路径生效；基线同样是一个 tick。

## 4. completion 模块的 A/B

completion 没有正式的用户态接口，所以加了一组**只在 `TASK_TEST=1` 时编译**的
测试 syscall（`AVATAR_SYS_COMPLETION_WAIT/COMPLETE/RESET`，见
`kernel/syscall/syscall_internal.h`）。用户任务直接阻塞在 `completion_t` 上，
测试程序是 `apps/c/task_completion_test.c`，入口脚本
`tools/task_completion_test.sh`。

```bash
make PLATFORM=qemu-virt-x86_64 SMP=2 TASK_TEST=1 clean
make PLATFORM=qemu-virt-x86_64 SMP=2 TASK_TEST=1 kernel rootfs
tools/task_completion_test.sh x86_64 2
```

`broadcast <n> <attempts>`：每轮 fork n 个等待者，全部 armed 后父进程调用
COMPLETE，子进程记录被唤醒时刻。基线 worktree 使用**同样的 syscall 号**
但内部是裸 `state=READY + task_unblock()`（无 completion、无 IPI）。

| 内核 | x86_64 max_us | aarch64 max_us | riscv64 max_us |
|---|---|---|---|
| 新（completion） | 200 ~ 245 | 582 ~ 871（首轮 3653） | 508 ~ 1333 |
| 基线（裸 wait/wake） | 4980 ~ 4990 | 4154 ~ 5372 | 1982 ~ 4898 |

`ready_cpu0=4 ready_cpu1=4` 在所有轮次都成立：等待者确实分布在不同核上。
新内核的 COMPLETE 对每个远端等待者走 `sched_unblock()` + resched IPI；
基线只入队、不发 IPI，所以跨核等待者要等目标核的下一次 tick。

注意：
- 测试 syscall 只在 `TASK_TEST=1` 变体里存在；正常内核调用会得到 ENOSYS。
- `completion_wait()` 不可被信号打断；测试程序自己控制轮次，不依赖超时。

## 5. 结论与边界

- `dist` 证明 SMP 下任务确实按 round-robin 摊到两颗核。
- `ping` 跨核证明 `sched_unblock() → sched_resched_cpu() → IPI` 真的
  把目标核从 tick 等待中解放出来；同核路径与基线一致，说明没有引入
  额外开销。
- 该测试覆盖的是 **用户态任务** 的唤醒路径。vCPU 任务走同一个
  `task_unblock`/`sched_unblock`，区别只在目标核可能在 guest 里；IPI
  会被硬件转成一次 VM-exit/HS trap（见 `docs/vmm/MULTI_VCPU.md`）。
- `completion_t` 的语义与 pipe 唤醒共用同一条 `sched_unblock` 路径，
  但 completion 本身没有用户态入口，仍需内核侧用例（AP 启动闸门接入时
  一并验证）。
