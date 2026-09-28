# SMP>1 下 helper 模式（`/bin/vmm-run`）的概率性起不来 / Ctrl+] 后卡死

**日期**: 2026-09-27
**影响架构**: 三架构通用（本文只列与架构无关的部分；x86/riscv 各自的
「每核状态」问题见 `docs/vmm/X86_GUEST_LINUX.md` §9.10 与
`docs/vmm/RISCV64_GUEST_LINUX.md`）
**严重性**: 高（SMP=2 下 helper 模式约 50% 起不来；重启 guest 必失败）
**触发条件**: `SMP>=2` + `run-net`/`run-fs` 起宿主 shell，再敲 `/bin/vmm-run`

---

## 一、症状

三条表现，看起来像三个不相关的 bug，实际根因互相咬合：

1. **概率性起不来**（约 1/2）：`/bin/vmm-run` 发 `VMM_IOC_BOOT` 之后 guest 一个
   字节都不输出。SMP=1 永远正常，直启模式（`test-guest-linux`）也正常。
2. **Ctrl+] 之后重启失败**：宿主里所有 `open()` 都报
   `[fd] pool_alloc: no free slots (FD_POOL_SIZE=128)`。
3. **Ctrl+] 之后整机卡死**，或概率性崩在调度器里：
   `sched_schedule+0xf2`（`next->state = TASK_RUNNING`），`CR2=0xffffffffffffffc8`。

## 二、根因（三个，都在「任务/每核状态」这条线上）

### 2.1 `task_create()` 之后再改核有窗口 ⇒ 同一任务被两个核同时执行

```c
/* kernel/vmm/vmm.c —— 原来 */
struct task *t = task_create(name, vcpu_task_fn, vcpu, priority);
if (t) task_set_cpu_affinity(t, 0);      /* ← 已经晚了 */
```

`task_create()` 内部**已经** `sched_enqueue()`（`CPU_AFFINITY_ANY` → round-robin
可能挑中 CPU1）。若 CPU1 在那两步之间把新任务挑走开始执行：

* `sched_dequeue(t)` 找不到它 —— RUNNING 的任务**不在任何队列里**（安全检查
  直接跳过）；
* 紧接着 `sched_enqueue(t)` 又把它挂到 CPU0 的队列上。

于是同一个 vCPU 任务**既在 CPU1 上运行、又躺在 CPU0 的运行队列里**，被两个核
同时执行：任务状态、内核栈、运行队列一起被写坏。SMP=2 下命中率约 1/2
（取决于 round-robin 先把它分给哪颗核），正好解释「有概率」。

**修法**：新增 `task_create_affinity(name, fn, arg, prio, cpu)` —— 在
`sched_enqueue()` **之前**把 `cpu_affinity` 定好；vcpu 任务改用它。

### 2.2 运行队列被写坏之后，调度器把野指针当任务用

`sched_enqueue()` 里那句无条件 `list_node_init(&task->run_node)` 会在**没持锁**
的情况下把已经在队列里的节点链接清零 —— 重复入队 = 静默写坏队列。
`pick_next()` 只是 `list_delete_first()` + `container_of()`，于是拿到野指针，
`next->state = ...` 直接 #PF/#GP。

**修法**（两道防御，都不改变正常路径行为）：

* `sched_enqueue()`：拒绝入队 `state == TASK_RUNNING` 的任务（并打 `KLOG_ERROR`）；
* `pick_next()`：取出的指针必须落在 `g_task_pool[]` 内，越界就跳过并打印现场，
  让本核跑 idle —— 坏的是一个节点，整机还能继续。

### 2.3 次级核 idle 任务的 `fd_table` 是 0（不是 -1）⇒ fd 池被一次吃光

`setup_secondary_idle_task()` 里 `memset(idle, 0, sizeof(*idle))` 之后只填了
少数几个字段 —— **`fd_table[]` 留成了 0**。而 `fd_pool_alloc()` 返回的槽位号
`0` 是**合法值**，于是这张表在 `fd_table_inherit()` 眼里就是「256 个都指向
槽位 0 的 fd」：一次继承 `fd_pool_alloc()` 循环 256 次，`FD_POOL_SIZE(128)` 个槽
瞬间占满，此后宿主里任何 `open()` 都失败。

（为什么会拿 idle 当父进程？见 `docs/vmm/X86_GUEST_LINUX.md` §9.10：宿主 MSR
被装错核之后 `task_current()` 会返回 idle。两者叠加才炸得这么彻底。）

**修法**：

* `setup_secondary_idle_task()` 显式 `fd_table[j] = -1`；
* `fd_table_inherit()` 遇到**非用户进程**的父进程直接按「无可继承」处理 ——
  内核任务本来就没有 fd 表可继承，从根上杜绝这类破坏。

## 三、验证

```bash
make PLATFORM=qemu-virt-x86_64 SMP=2 kernel rootfs
tools/vmm_helper_regress.sh x86_64 10 2            # 启动
tools/vmm_helper_regress.sh x86_64 6  2 --restart  # 启动 → Ctrl+] → 再启动
```

修复前后（`run-net` helper 模式，判据 = guest 打出自己那行 uname）：

| 架构 | SMP | 启动 | Ctrl+] 后再启动 |
|---|---|---|---|
| x86_64  | 2 | 10/10 | 10/10 |
| x86_64  | 4 | 5/5 | 20/20 |
| riscv64 | 2 | 6/6 | 6/6 |
| riscv64 | 4 | 5/5 | 5/5 |
| aarch64 | 2 | 6/6 | 6/6 |
| aarch64 | 4 | 5/5 | 5/5 |

（以上为**同一把尺子**：最终版工具 + 每组先 clean 重建非 `GUEST_LINUX` 变体 +
独占串行。修复前基线：x86_64 SMP=2 启动 6/6 **失败**、重启 0/6。）
直启模式门禁 `tools/boot_regress.sh 20`：**20/20**（41s）。
x86 侧另有一条同族根因（VMCS 初始化必须在 vCPU 核上）见
`docs/vmm/X86_GUEST_LINUX.md` §9.13 —— 它表现为「Ctrl+] 后再启动失败」，
本文的调度器/每核状态修复**不能**覆盖它。

> ⚠️ **这套回归必须独占机器、串行跑。** vCPU 任务钉在单核上，机器被压满时它抢不到
> CPU —— guest 自己的时钟在 90 秒墙上时间里只走 0.5 秒，看起来像"卡死/起不来"，
> 其实空载 **2 秒**就起来了。工具现在带 `flock` 自锁，多架构请排队串行。
>
> ⚠️ **跑之前先确认内核变体**：本回归必须在**非 `GUEST_LINUX`** 的内核上跑。
> 直启变体会自己引导 guest，而 **guest 的提示符也是 `~ #`** —— 脚本会把它当成
> 宿主就绪、把 `/bin/vmm-run` 喂进 guest（guest 回显 `not found`），于是把
> 「直启成功」误判成「helper 成功」；Ctrl+] 那轮则必然失败（实测 3/3 失败、
> 每轮白等满 216 秒）。触发方式极隐蔽：`make ... GUEST_LINUX=1 rootfs`
> **会覆盖同一 build 目录里的内核**（见 CLAUDE.md「变体标志」那一条），
> 跑完直启门禁紧接着跑本脚本就会中招。脚本现在启动时 `strings` 自检，
> 是直启变体就直接拒绝运行并打印重建命令。
>
> ⚠️ **Ctrl+] 之后要等对事件**：不能只等 `stop requested` 就敲第二次 ——
> 那个标记出现时 helper 还没退出、宿主 shell 也还没回到读 stdin，命令会掉在
> 缝里（riscv64 SMP=2 实测 2/4 假失败，每轮白等满 180 秒，而 stop 流程本身
> 完全正常：`stop requested` → `guest stopping` → `vcpu0 exited normally`）。
> 脚本现在按日志**字节偏移**判定「stop 之后**新出现**的宿主提示符」再喂命令。
>
> ⚠️ **判据里带括号就别用 `grep -E`**：guest 的 uname 是
> `Linux (none) 6.2.15`，`grep -acE` 会把 `(none)` 当**分组**，于是永远匹配不上 ——
> 实测把所有**成功**的轮次都判成了"guest 启动 0 次"，白查了半天（日志里
> marker 明明在）。工具现在用 `grep -acF` 固定串匹配判据。

## 四、教训

**「每核一份」和「每任务一份」的状态，初始化点必须和使用点同核。** 这一轮
一共抓到四处同一形态的问题（x86 三处：`IA32_FEATURE_CONTROL`、VMXON 区域、
宿主 MSR 载入表；riscv64 一处：`hgatp` 等 HS CSR），外加这里的一处。它们的
共同外壳都是「SMP=1 永远正常、直启模式也正常、只有 helper 模式 + 多核才犯」——
因为**只有 helper 模式才会让 setup 核 ≠ vCPU 核**。

**同一家族后来还有第五处，但形态反过来**：VMCS 宿主区的 `HOST_SEL_TR` /
`HOST_BASE_TR`（TSS 也是每核一份，却写死了 cpu0 那份）。它的特殊之处在于
**VMCS 宿主区在 VM-exit 时只会被"装回"、不会被保存** —— 所以连"运行中被
纠正"的机会都没有，那个错值会一直留在里面。判据因此不是"setup 核 ≠ vCPU 核"，
而是"**有没有 VM-exit 落在非 BSP 核上**"。根因、自检与实测见
`docs/vmm/X86_GUEST_LINUX.md` §11.5。
