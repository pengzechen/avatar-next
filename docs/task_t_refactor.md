# task_t 结构重构方案

**状态**：P0~P3 已完成（2026-11-10）；P4/P5 待做
**对象**：`kernel/task/task.h` 的 `task_t`（任务控制块）

**结果**：`sizeof(task_t)` **3624 → 1600（P1）→ 768 B（P3）**。
fd 表/cwd/pgd 等进程级状态移到独立的 `files_struct` / `fs_struct` /
`mm_struct` 静态池对象；调度热区仍在首条 cacheline。

## 一、现状

`offsetof` 实测（x86_64，三架构字段布局相同）：

```
sizeof(task_t) = 3624 B
```

64 个静态槽位 `g_task_pool[TASK_MAX]` 约 **226.5 KiB** BSS；另有
`64 × 16 KiB = 1 MiB` 内核栈。

| 区间 | 内容 | 大小 | 占比 |
|---|---|---:|---:|
| 0–99 | sp/state/switch_done/id/name/节点/cpu_affinity | 100 B | 2.8% |
| 100–199 | 用户进程标志、pgd、user_*、heap/mmap | 100 B | 2.8% |
| 200–455 | `cwd[128]` + `exe_path[128]` | 256 B | 7.1% |
| 456–999 | `fd_table[256]` + `fd_cloexec[32]` | 544 B | 15.0% |
| 1000–1415 | uid/gid、`groups[32]`、`rlimit_cur/max[16]` | 416 B | 11.5% |
| 1416–1503 | prctl/退出/等待/CPU 时间 | 88 B | 2.4% |
| 1504–3599 | 信号系统（`sig_actions[64]` 占 2048 B） | 2096 B | 57.8% |
| 3600–3623 | itimer、preempt_count、on_cpu、wake_pending | 24 B | 0.7% |

字段访问次数（全 `kernel/` 粗计，非热路径加权）前列：`id` 118、`state` 61、
`pgd` 57、`is_user_process` 44、`name` 39、`fd_table` 39、`fd_cloexec` 27、
`cwd` 26、`tgid` 25、`sig_actions` 24。

## 二、问题

### 1. 冷热混排 + 相邻 TCB 伪共享

调度/切换/唤醒真正碰的是：

```
sp(0) state(8) switch_done(12) id(16) run_node(64)
cpu_affinity(96) ... preempt_count(3616) on_cpu(3620) wake_pending(3621)
```

一次切换跨约 3.6 KiB、3 条 cacheline；`on_cpu/wake_pending` 在结构体最尾部。

`sizeof(task_t) % 64 == 40`，且 `g_task_pool` 没有 64 字节对齐。相邻两个
TCB 共用一条 cacheline：

- 任务 N 尾部：`itimer_expire_ns`、`itimer_interval_ns`、`preempt_count`、
  `on_cpu`、`wake_pending`
- 任务 N+1 头部：`sp`、`state`、`switch_done`、`id`、`priority`、`name`

CPU0 切任务 N、CPU1 跑任务 N+1 时会互相弹这条 cacheline。

### 2. 共享对象没有建模

`clone_handler` 手工逐字段复制 pgd/用户布局/fd/cwd/信号 action/rlimit/组，
约 40 行。Linux 中这些属于可引用计数的进程级对象：

- `pgd` + `heap_end` + `mmap_*` + `interp_end` → `mm_struct`
- `fd_table` + `fd_cloexec`（+ `cwd` 的 fs 部分）→ `files_struct` / `fs_struct`
- `sig_actions` + `pgid/sid/ctty` → `signal_struct`（线程共享信号处置）
- `uid/gid/groups/umask` + rlimits → `cred` / `limits`

当前用 `is_thread` / `shares_pgd` 两个 bool 手工模拟共享关系，
`task_group_has_other_threads()` 扫线程组判断页表能否释放；
`exit_group` 的兄弟线程竞态同源。

### 3. 大数组占了九成

`sig_actions`（2048 B，58%）+ `fd_table`（544 B，15%）+ `cwd/exe`（256 B）
+ rlimits（256 B）+ groups（128 B）≈ 3.2 KiB，占 89%。典型进程只装
2~5 个信号 handler、开不到 20 个 fd。

### 4. 两个待确认的不一致

- `TASK_STACK_SIZE` 注释写“16 KiB 不够、32 KiB 下实测高水位”，但定义仍是
  `16384u`；`git log -S 32768` 显示 32 KiB 从未提交过。
- `TASK_MAX_FD = 256`，但 `RLIMIT_NOFILE` 默认 1024 且允许到 1M；
  `setrlimit(NOFILE, 1024)` 成功但 fd 分配仍被 256 卡住。

## 三、分阶段方案

### P0：只调布局，不改语义（低风险）

目标：调度热字段压进前 1~2 条 cacheline，消除伪共享。

1. 把 `preempt_count`、`on_cpu`、`wake_pending`、`switch_done`、
   `cpu_affinity` 移到结构体最前面，与 `sp/state/id/run_node` 同区；
   `itimer_expire_ns/itimer_interval_ns/sleep_expire_ns` 紧随其后（tick
   每 10 ms 扫全池会读它们）。
2. 大数组和冷字段移到尾部：`sig_actions`、`fd_table`、`cwd/exe`、
   `rlimits/groups`。
3. `task_t` 加 `__attribute__((aligned(64)))`，并加编译期断言：
   ```c
   _Static_assert(sizeof(task_t) % 64 == 0, "...");
   _Static_assert(offsetof(task_t, on_cpu) < 64, "...");
   _Static_assert(offsetof(task_t, wake_pending) < 64, "...");
   _Static_assert(offsetof(task_t, switch_done) < 64, "...");
   ```
4. bool 合并成 flags 留到后续阶段做（7 个 bool 只占 7 字节，收益小、
   调用点分散，避免 P0 扩大风险）。
5. 三个架构的 `switch.S` 都没有硬编码偏移——sp/switch_done 都是从 C
   传入指针，字段重排安全。

预期：结构体仍是 ~3.6 KiB，但热区 <128 B，伪共享消失。

**实施结果**：`sizeof(task_t) = 3648`（aligned(64)，57 条 cacheline）；
`on_cpu` offset 32、`wake_pending` 33、`switch_done` 12、`run_node` 48，
全部落在首条 cacheline。bool 合并成 flags 因调用点分散（~80 处）留到 P5，
P0 只做布局，零语义改动。

### P1：信号 action 外置（收益最大）

```
struct signal_struct {           /* 引用计数，CLONE_THREAD 共享 */
    volatile int refcount;
    sig_action_t actions[NSIG];
};
```

- `task_t` 只留 `struct signal_struct *sig`；`blocked_sigs/pending_sigs/
  sig_saved_blocked/sig_frame_sp` 仍是每线程字段。`pgid/sid/ctty` 暂留
  task_t（移入 signal_struct 需要一起处理 setsid/setpgid/tty 和懒分配
  的 NULL 语义，留到后续阶段）。
- `sig == NULL` 表示全部 `SIG_DFL`，第一次 `rt_sigaction` 时从静态池
  `g_signal_pool[TASK_MAX]` 取一份，避免动态分配和失败路径。
- 线程（`CLONE_THREAD` + `CLONE_VM`）在 clone 时共享同一份：
  refcount++；fork 复制一份。为了保持“线程共享信号处置”的语义，
  clone 时必须保证父任务已有 signal_struct（NULL 则先分配一份默认的）。
- 访问统一走 helper：

  ```c
  bool task_sigaction_get(task_t *t, int sig, sig_action_t *out);
  int  task_sigaction_set(task_t *t, int sig, const sig_action_t *act);
  bool task_signal_is_ignored(task_t *t, int sig);
  void task_signal_exec_reset(task_t *t);
  ```

- 更新 `signal.c`（投递 + rt_sigaction）、`exec.c`（exec 重置，保留
  SIG_IGN）、`proc_lifecycle.c`（clone 共享/复制）、`pipe.c` 与
  `task.c::task_signal_pending_effective` 的「是否忽略」判断。
- `slot_take_and_free()` 释放 TCB 时 put 引用计数；exec 若共享则
  先 unshare 再重置。

预期：TCB 体积减少约 2 KiB（3624 → ~1.6 KiB）；线程信号处置语义
顺带修正。

**实施结果**：`sizeof(task_t) = 1600`（25 条 cacheline，3624 → 1600，-56%）。
信号常量/`sig_action_t`/`signal_struct_t` 及处置 API 放在公共头
`include/signal.h`；`sig_action_t[64]` 的静态池实现在
`kernel/syscall/core/signal_struct.c`（与 `signal.c` 放在一起，不再放在
task 目录）；
`sig == NULL` 表示全 SIG_DFL，`rt_sigaction` 首次写入或创建线程组时才分配。
`CLONE_THREAD + CLONE_VM` 共享，fork 复制，exec 先 unshare 再保留 SIG_IGN。
失败路径（fork 的 pgd 分配/地址空间复制失败）会 release 子任务的引用。
`pgid/sid/ctty` 暂留 task_t（见正文说明）。

### P2：files/fs 外置

```
struct files_struct {
    volatile int refcount;
    int16_t fd_table[TASK_MAX_FD];
    uint8_t fd_cloexec[TASK_MAX_FD / 8];
};
struct fs_struct {
    volatile int refcount;
    char cwd[TASK_CWD_LEN];
};
```

- fork 默认复制；未来支持 `CLONE_FILES` 时线程可直接共享。
- `TASK_MAX_FD` 与 `RLIMIT_NOFILE` 的不一致本轮未改：实际可达上限仍由
  `TASK_MAX_FD`（256）和 `FD_POOL_SIZE`（128）共同决定；彻底对齐需要
  动态/可增长的 fd 池，留作后续项。
- `exe_path` 随 P3 放到 mm。

**实施结果**：`include/process_fs.h` 定义两个对象；
`kernel/syscall/fs/process_fs.c` 实现静态池 + 引用计数（池大小
`TASK_MAX + AVATAR_MAX_CPUS`，idle 任务也有一份）。TCB 从 1600 → 约
1000 B。当前 clone 仍按旧行为复制（fork/线程各一份），为后续共享留好模型。

### P3：抽出 `mm_struct`（生命周期模型）

```
struct mm_struct {
    volatile int refcount;
    uint64_t *pgd;
    uint64_t heap_end;
    uint64_t mmap_next, mmap_base;
    uint64_t interp_end;
    char exe_path[TASK_EXE_LEN];
};
```

- `user_entry/user_sp/user_stack_top/user_stack_size` 是每线程的，留 task。
- `is_thread` / `shares_pgd` 可由 `mm->refcount` 和 `tgid != id` 推导，
  删除两个 bool。
- fork：共享或复制 mm；exec：替换 mm；最后一个引用释放 pgd。
- `task_group_has_other_threads()` 可退化成 `mm->refcount > 1`。

**实施结果**：`include/mm_struct.h` + `kernel/mm/mm_struct.c`。所有任务
（含 idle）创建时分配一份空 mm；`CLONE_VM + CLONE_THREAD` 共享父 mm，
fork 使用自己的新 mm 并设置 `owns_pgd`；exec 在旧 mm 仅剩当前引用时
复用，否则分配新 mm、旧 mm 留给线程组。`shares_pgd` 字段删除，
`is_thread` 改为 `task_is_thread(t)`（`tgid != id`）；
`task_group_has_other_threads()` 删除（exec 不再需要手工判断）。
`pgid/sid/ctty` 仍留在 task_t（属于进程级但不在本轮范围）。
最终 `sizeof(task_t)=768 B`。

`uid/euid/gid/egid/groups/umask` + `rlimit_cur/max` 抽出引用计数对象，
fork 复制、线程共享，集中化继承逻辑。

### P5：冷字段收尾

- `entry/arg` 只在启动时用一次，可移入 ext。
- `exit_status/exit_signal/is_waiting/wait_pid` 可与睡眠/itimer 状态做
  生命周期 union。
- 剩余 bool 合并成 flags（`on_cpu/wake_pending` 保持独立字节或原子
  操作，不要塞进普通 bitfield）。
- 独立 `struct task_ext *ext` 承载 rlimit/groups/timer/退出状态等冷数据。

## 四、实施注意事项

1. **静态池 + 引用计数，不引入 kmalloc**：任务创建可能在早期启动或
   关中断路径。`g_signal_pool[TASK_MAX]` 等与 TCB 池同寿命，分配不会
   失败。
2. **释放路径**：`task_reap_dead()` / `slot_take_and_free()` 现在只负责
   pgd；对象化后要保证“最后一个线程退出才减引用”，且不重复释放。
   与 `docs/bugfix/SMP_TASK_EXIT_STACK_REUSE.md` 的 `switch_done` 门控
   在同一个区域，改完必须跑 SMP>1。
3. **`on_cpu/wake_pending` 的并发语义**不要被 bitfield 破坏；
   `switch_done` 是 release store，不能和普通标志混在一个非原子字里。
4. **头文件改动后三架构全量 clean**（CLAUDE.md 的已知陷阱），并跑：
   - `make -C tests ltp-full ARCHES='aarch64 riscv64 x86_64'`
   - x86_64 SMP=4 / aarch64 SMP=2 的 LTP
   - `tools/vmm_helper_regress.sh`、`tools/vmm_multivm_regress.sh`
5. **迁移期用访问宏过渡**（如 `TASK_FD(t, fd)`），但引用计数的增减不要
   藏在宏里。

## 五、落地顺序

1. ✅ P0 布局调整（零语义风险，先拿 cache 局部性）
2. ✅ P1 `signal_struct`（去掉 58% 体积 + 修正线程信号语义）
3. ✅ P2 `files/fs_struct`
4. ✅ P3 `mm_struct`
5. ⬜ P4/P5 cred/limits 与冷字段收尾

## 六、验证记录

2026-11-10：

- 三架构 `make PLATFORM=<p> clean && make PLATFORM=<p> kernel` 全部通过
  （x86_64 / aarch64 / riscv64）。
- LTP 全量（103 例），P1 后与 P2/P3 后各跑过一遍：
  - x86_64 SMP=1：103/103 PASS（P1、P2/P3 各一次）
  - aarch64 SMP=1：103/103 PASS（P1、P2/P3 各一次）
  - riscv64 SMP=1：103/103 PASS（P1、P2/P3 各一次）
  - x86_64 SMP=4：103/103 PASS
  - aarch64 SMP=2：103/103 PASS
- VMM helper/多 VM 回归：本轮未跑（构建目录里没有对应 rootfs 镜像；
  P0/P1 不改变 VMM 路径，vCPU 任务只受 TCB 字段重排影响，三架构编译
  已覆盖布局变化）。需要时可先 `make PLATFORM=<p> rootfs` 再跑
  `tools/vmm_helper_regress.sh` / `tools/vmm_multivm_regress.sh`。
