# 实时、Linux syscall 兼容与虚拟化的统一任务/内存架构

**状态**：架构定稿，R1/R2 已完成（2026-11-10）；R0/R3+ 待做
**目标分支**：`realtime`（从 `dev` 拉出，只承载实时化改造）

## 一、目标与非目标

**目标**

- 一个通用的 task 模型，同时承载：
  - 内核线程（net-poll、vmm-loader、idle）
  - 内核 RT 线程（实时 worker、中断下半部）
  - 用户线程（busybox、pthread、LTP）
  - vCPU 线程（宿主内核态执行 guest）
- 一个通用的 mm 模块，同时服务用户地址空间和 guest stage-2 地址空间。
- 保留“兼容一部分 Linux syscall”的定位。
- 以 Linux PREEMPT_RT 思路做**软实时/有界延迟**，不是硬实时认证。

**非目标（至少第一阶段）**

- 不做用户态 RT 线程，不给 RT 线程提供 Linux syscall/POSIX 实时 ABI。
- 不做硬实时 WCET 认证、完全无动态分配的内核、全 syscall 路径可抢占。

## 二、统一任务模型

RT 是**调度类**，不是 task 类型；vCPU 是**执行域 + 扩展指针**。

```c
enum task_domain {
    TASK_DOMAIN_KERNEL = 0,  /* 内核线程 */
    TASK_DOMAIN_USER,        /* 用户进程/线程，有 mm/files/fs/sig */
    TASK_DOMAIN_VCPU,        /* 宿主内核线程 + struct vcpu * 扩展 */
};

enum task_sched_policy {
    TASK_SCHED_NORMAL = 0,
    TASK_SCHED_FIFO,         /* 1..99，数值越大优先级越高 */
    TASK_SCHED_RR,
};
```

| 线程 | domain | mm | policy | 说明 |
|---|---|---|---|---|
| 内核线程 | KERNEL | NULL | NORMAL | vmm-loader、net-poll、idle |
| 内核 RT 线程 | KERNEL | NULL | FIFO/RR | 实时 worker |
| 用户线程 | USER | 有 | NORMAL | Linux ABI、LTP |
| vCPU 线程 | VCPU | NULL（宿主侧） | NORMAL/FIFO | guest stage-2 独立 |

**关键不变量**

1. RT 线程只存在于 KERNEL 域（第一阶段）；不复用 Linux syscall 层。
2. vCPU 的宿主侧 `task->mm == NULL`；guest 的 G-stage/EPT 属于
   `vcpu->stage2`，不属于 task mm。
3. RT 是调度策略，未来若增加用户 RT，只需 `USER + FIFO`，不改任务生命周期。

## 三、调度器

- 每 CPU：
  - `rt_queue[100]` + `rt_bitmap[2]`：RT 优先级队列，最高优先级 O(1)；
  - `run_queue`：NORMAL 轮转队列；
  - `idle`。
- `pick_next()`：最高 RT 队列 → NORMAL → idle。
- `sched_enqueue/sched_unblock`：按 policy 入对应队列；RT 任务唤醒时若
  优先级高于当前任务，立即置 `need_resched` + resched IPI。
- tick 语义：
  - NORMAL：tick 触发轮转；
  - FIFO：tick 不抢占；
  - RR：按时间片计数，片用完才置 `need_resched`；
  - idle：tick 时允许被任意就绪任务抢占。
- 后续：oneshot 定时器/时间轮、优先级继承 mutex、RT throttling、CPU 隔离。

## 四、mm 模块

- `mm_struct` 是可选的进程地址空间对象（引用计数），只挂在 USER 任务上。
- `mm == NULL` 的 KERNEL/VCPU 任务沿用内核页表，调度器不切用户 CR3/satp/TTBR0。
- stage-2 地址空间独立建模（后续）：与用户 mm 共用“地址空间操作接口”，
  但生命周期属于 `vcpu/vm`。
- 后续为 RT 增加 VMA 元数据、mlock/mlockall、预缺页；RT 线程禁止在
  实时循环中触发 demand paging / COW / 动态分配。

## 五、RT 线程的约束

RT 内核线程可以用内核 API，但必须遵守：

- 不在 RT 循环里调用 VFS/ext4、控制台/KLOG、动态分配、可能长时间持锁的路径；
- 数据交互走预分配共享内存、无锁队列、RT mutex、timer、中断/门铃；
- 驱动/设备路径要 RT-safe：短临界区、无全局锁；
- 用户面只保留**非 RT 控制面**（/dev ioctl、共享内存、event），RT 线程本身
  不执行 syscall。

## 六、vCPU 线程

- vCPU 任务 = `TASK_DOMAIN_VCPU` + `struct vcpu *` 扩展；
- 复用 `task_create_*`、`task_block/unblock`、`task_kick`、调度器 RT 类；
- vCPU 空闲时 block（WFI/halt/event wait），由中断/`task_kick()` 唤醒；
- 后续暴露 vCPU 调度策略/亲和性接口；RT VM 的 guest 内存预填充/钉住。

## 七、路线图

| 阶段 | 内容 | 状态 |
|---|---|---|
| R0 | 延迟测量：IRQ-off/preempt-off/唤醒延迟/VM-exit 延迟 | 待做 |
| R1 | task domain / sched policy / 创建接口（本文件） | ✅ 已完成 |
| R2 | per-CPU RT 队列 + 优先级抢占 + FIFO/RR tick | ✅ 已完成 |
| R3 | 内核可抢占：有界 IRQ-off、长锁拆分、ISR 减负 | 待做 |
| R4 | mm VMA/mlock + stage-2 预填充 | 待做 |
| R5 | vCPU RT 调度接口与 guest 内存钉住 | 待做 |
| R6 | 仅在内核域增加 RT worker API；不做用户 RT syscall ABI | 约束 |

## 八、实现与验证（R1/R2）

- `task_t` 新增 `domain` / `sched_policy` / `rt_priority` / `rt_slice`。
- `TASK_DOMAIN_KERNEL/USER/VCPU`；`TASK_SCHED_NORMAL/FIFO/RR`。
- 新增接口：
  - `task_create_kernel_rt(name, fn, arg, rt_prio, affinity)`
  - `task_create_vcpu(name, fn, arg, priority, affinity)`
  - `task_create_affinity()` 保持原语义（KERNEL + NORMAL）。
- 调度器：每 CPU `rt_queue[100] + rt_bitmap`；`pick_next` 先 RT 后
  NORMAL 再 idle；高优先级 RT 入队/唤醒时立即置 `need_resched` 并
  resched IPI；FIFO 不被 tick 抢占，RR 按 10 tick 时间片轮转。
- vCPU 创建改走 `task_create_vcpu()`（domain=VCPU）。
- 新增 `RT_TEST=1` 自测（`tests/unit-test/rt_sched_test.c`）：
  三个任务钉 CPU0，验证高优先级 RT > 低优先级 RT > NORMAL 的执行顺序。

**验证结果**：

- 三架构 clean build 通过。
- LTP 全量 103/103：x86_64 / aarch64 / riscv64 SMP=1 全绿；
  x86_64 SMP=4、aarch64 SMP=2 也全绿。
- `RT_TEST=1` 在 x86_64 / aarch64 / riscv64 上均 PASS：
  `make PLATFORM=<p> RT_TEST=1 LOG=info run`，串口应出现
  `RT SCHED TEST: PASS`。

**后续验收门禁**：R0 延迟指标；VMM helper/多 VM 回归；R3 的有界
IRQ-off 后才对外宣称实时能力。
