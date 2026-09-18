# x86_64 SMP：SYSCALL 入口用错内核栈导致系统冻结

**日期**: 2026-09-17
**影响架构**: x86_64（`ARCH_X86_64`）
**严重性**: 高（SMP 下必现整机冻结）
**触发条件**: `SMP>=2`，进入用户态并开始跑多进程负载（如 `./ltp/run_ltp.sh`）

---

## 一、症状

```bash
make PLATFORM=qemu-virt-x86_64 run-net SMP=1 LOG=debug   # 正常，LTP 跑完
make PLATFORM=qemu-virt-x86_64 run-net SMP=4 LOG=debug   # 卡死
```

`SMP=4` 时 LTP 停在第一个测例 `brk01` 的 `fork` 之后，此后串口再无输出，整机冻结。

---

## 二、第一眼看到的假象：自旋锁死锁

用 gdb attach（`-gdb tcp::1234`）采样，四个 vCPU 全在同一处：

```
Thread 1 (CPU#0): #0 spin_lock (lock=...<g_uart_hw_lock>) at include/x86_64/spin_lock_impl.h:23
Thread 2 (CPU#1): #0 spin_lock (lock=...<g_uart_hw_lock>) ...
Thread 3 (CPU#2): #0 spin_lock (lock=...<g_uart_hw_lock>) ...
Thread 4 (CPU#3): #0 spin_lock (lock=...<g_uart_hw_lock>) ...
```

`g_uart_hw_lock.lock == 1`，但**持有者不在任何一个 CPU 上**——四核都在等它。

这就是最误导人的地方：**这不是锁的 bug，锁只是受害者。**

真正的原因藏在日志更早的一行：

```
[ERROR][C0] CPU exception #14 at RIP=0x86 EC=0x10     ← 取指缺页，内核态跳到 0x86
  RDX=0x3fd  RBP=...<uart_x86_rx_available>           ← 正在 signal_check_uart 里
  RSP=0xffff800000318000  CR3=0x9a0000                ← 用户页表
```

CPU0 在 `signal_check_uart()` 里崩了（`ret` 跳到 0x86 → #PF）。而 x86_64 的异常处理尾部是死循环：

```c
/* boot/x86_64/exception.c */
while (1)
    __asm__ volatile("hlt");
```

**崩进去的 CPU 手里还攥着 `g_uart_hw_lock`，于是它永远不释放。** 另外三个核每个 tick 的
timer ISR 都会调 `signal_check_uart()`（`driver/timer/timer_x86_64_impl.h:79`），
自然全堆在这把锁上 → 整机冻死。

> **教训**：看到"锁被持有但持有者不在任何 CPU 上"，先去找**最近发生的那次异常/崩溃**，
> 而不是去读锁的实现。

---

## 三、根因 1（致命）：SYSCALL 入口读的是全局内核栈顶

x86_64 的 `SYSCALL` 指令**不像中断那样由硬件换栈**——它不读 TSS.RSP0。
内核入口必须自己把 RSP 换成内核栈。

原实现（`boot/x86_64/syscall_wrapper.S`）：

```asm
    movq    %rsp, %gs:CPU_SCRATCH_RSP
    /* 使用调度器维护的 TSS.RSP0（每任务独立内核栈顶） */
    movq    g_x86_tss_rsp0(%rip), %rsp      ; ← 读的是【全局】变量
```

而这个全局量在 `boot/x86_64/tss.c` 里只在 CPU0 上更新：

```c
void x86_tss_set_rsp0(uint64_t rsp0) {
    uint32_t cpu_id = cpu_current()->cpu_id;
    g_tss[cpu_id].rsp0 = rsp0;          /* per-CPU TSS 更新了 */
    if (cpu_id == 0)
        g_x86_tss_rsp0 = rsp0;          /* ← 只有 CPU0 更新全局 */
}
```

**后果**：CPU1/2/3 上的 syscall 会把 trap frame 建在 **CPU0 上次调度的那个任务的内核栈**上。
如果那个任务此刻正在 CPU0 上跑，两块 CPU 就在同时读写同一段栈 —— 返回地址被互相覆盖，
`ret` 跳到垃圾地址。

这正是上面 `RIP=0x86` 的来历：CPU0 持有 `g_uart_hw_lock` 正在
`signal_check_uart()` 的取字符循环里，另一个核把它的栈踩了。

`SMP=1` 时不复现 —— 只有一个 CPU，全局量永远正确。

---

## 四、根因 2：`CPU_SCRATCH_RSP` 偏移过期，写坏了 `rq_lock`

`boot/x86_64/syscall_wrapper.S` 里手抄了 `cpu_t` 的字段偏移：

```asm
/* offsetof(cpu_t, scratch_rsp) — must match kernel/task/cpu.h */
.equ CPU_SCRATCH_RSP, 80
```

但 `cpu_t` 后来加了 `run_queue` / `rq_lock` / `irq_depth` / `preempt_schedule_depth` /
`local_ticks` 等字段，`scratch_rsp` 实际已经漂到 **104**。

80 正好落在 **`rq_lock.irq_flags`** 上（`rq_lock` 在 0x48，`irq_flags` 在 0x50=80）。
而 `sched_enqueue()` / `sched_dequeue()` 也会写目标核的 `rq_lock.irq_flags`
（`kernel/task/sched.c`）：

```c
spin_lock_irqsave(&tc->rq_lock);      /* 写 tc->rq_lock.irq_flags = 本核 RFLAGS */
...
spin_unlock_irqrestore(&tc->rq_lock);
```

**竞态**：CPU1 在 syscall 入口刚 `movq %rsp, %gs:80` 存好用户 RSP，还没执行
`pushq %gs:80`，此时 CPU0 因 `sched_enqueue()` 把 `g_cpus[1].rq_lock.irq_flags`
改成自己的 RFLAGS —— CPU1 推入 trap frame 的就成了**别人的 RFLAGS 而不是用户 RSP**，
任务返回时带着垃圾栈指针回用户态。

---

## 五、修复

| 文件 | 改动 |
|------|------|
| `kernel/task/cpu.h` | `cpu_t` 新增 `kernel_rsp0`（本核 TSS.RSP0 的镜像）；新增两条 `static_assert` 锁死 .S 可见的偏移 |
| `boot/x86_64/syscall_wrapper.S` | `CPU_SCRATCH_RSP` 80→104；新增 `CPU_KERNEL_RSP0`=112；换栈改读 `%gs:CPU_KERNEL_RSP0` |
| `boot/x86_64/tss.c` | 删除全局 `g_x86_tss_rsp0`；`x86_tss_init_cpu()` / `x86_tss_set_rsp0()` 同步写 per-CPU 的 `cpu_t::kernel_rsp0` |
| `boot/x86_64/tss.h` | 删除 `g_x86_tss_rsp0` 声明 |
| `kernel/main.c` | `x86_tss_init()` 从 `exception_init()` 之后**挪到 `cpu_init_bsp()` 之后**（见下） |

**为什么 main.c 要调顺序**：`cpu_init_bsp()` 会 `memset(&g_cpus[0], 0, ...)`，
放在它前面的 `x86_tss_init()` 写进去的 `kernel_rsp0` 会被清零，内核起来后第一次
syscall 就会把 RSP 装成 0。IDT 不使用 IST（`idt[vec].ist = 0`），所以 TSS 晚一点
初始化不影响异常处理。

**防回归**：偏移不再靠注释约束，而是编译期断言。以后任何人动 `cpu_t` 布局，
构建会直接失败并指出要改哪一行：

```c
/* kernel/task/cpu.h */
#if ARCH_X86_64
static_assert(offsetof(cpu_t, scratch_rsp) == 104,
              "CPU_SCRATCH_RSP in boot/x86_64/syscall_wrapper.S is stale");
static_assert(offsetof(cpu_t, kernel_rsp0) == 112,
              "CPU_KERNEL_RSP0 in boot/x86_64/syscall_wrapper.S is stale");
#endif
```

---

## 六、验证结果

| 配置 | 异常数 | LTP 结果 |
|------|--------|----------|
| SMP=4 修复前 | #PF @ CPU0 | 卡死在 `brk01` |
| SMP=4 修复后 | **0** | 25 个：24 PASS / 1 FAIL |
| SMP=1 基线 | 0 | 25 个：24 PASS / 1 FAIL |

唯一失败的 `epoll_wait04` 在 SMP=1 下同样失败，属既有功能性问题，与本 bug 无关。
AArch64 / RISC-V 全量重编通过。

---

## 七、排查方法（可复用）

**不要只靠 KVM 下的 gdb attach 采样。** 本次调试中 QEMU gdbstub 给出的
per-vCPU 寄存器读与内存读**互相矛盾**（同一个 CPU 既显示"持有锁"又显示"在自旋"），
浪费了不少时间。

更可靠的做法：**让 guest 自己记录、自己打印快照**。在可疑的自旋处加"每核 phase + RIP"
记录，超时后由该核 panic 并 dump 全部核的状态：

```c
volatile uint8_t  g_dbg_phase[AVATAR_MAX_CPUS];  /* 1=自旋 2=持有 3=持有且正在读硬件 */
volatile uint64_t g_dbg_rip[AVATAR_MAX_CPUS];

/* 自旋循环里 */
g_dbg_rip[cid] = dbg_rip();            /* lea (%%rip), %0 */
if (++spins == 30000000ULL) { /* dump 所有核的 phase/rip/task/irq_depth 后 panic */ }
```

一次就定位到 `cpu0: phase=2 held=1`（持有锁且卡在临界区）+ 那条 `#PF`。

另外：**`g_dbg_lock_held[]` 这类"谁持有锁"的数组比任何锁内部实现都直观**，
排查 SMP 死锁时值得第一时间加上。

---

## 八、检查清单

- [ ] x86_64 上**任何**手工切换内核栈的入口（SYSCALL / 未来可能加的快速路径），
      必须读 **per-CPU** 的值，不能读全局
- [ ] `%gs:` 访问 `cpu_t` 字段的 .S 文件，偏移必须与 `cpu.h` 的 `static_assert` 对齐
- [ ] 新增 per-CPU 状态时，确认它在 `cpu_init_bsp()` 的 `memset` **之后**才被初始化
- [ ] 看到"所有核卡在同一把锁"时，先查日志里是否有更早的 `CPU exception` / panic
- [ ] SMP 下排查死锁，优先用 guest 内自报快照，而不是 gdb 采样

---

## 附：本次调试中被绊到的构建陷阱（与 bug 本身无关，但会反复咬人）

**本仓库的 Makefile 不跟踪头文件依赖。** 编译时用了 `-MMD -MP` 生成 `build/**.d`，
但**没有 `-include` 它们**（见 `Makefile` 第 1188 行附近的注释，以及
`docs/PROJECT_REFERENCE.md` 第 241 行）。后果：

- 修改任何 `.h` → **不会**触发依赖它的 `.c` 重编
- 修改 `SMP=` / `LOG=` 等只进 CFLAGS 的量 → **不会**触发重编
  （`CONFIG_SMP_CPUS` 只被 `kernel/task/cpu.c` 使用，其它文件无感知）

本次实测：给 `cpu_t` 加字段后只跑 `make kernel`，`kernel_task_cpu.o` 没重编，
`g_cpus[]` 仍按旧的 `sizeof(cpu_t)`=112 分配，在 BSS 里**和紧随其后的
`g_secondary_idle[]` 重叠**：

```
ffff8000002f9800 B g_cpus            # 8 × 112 = 0x380（旧布局）
ffff8000002f9b80 b g_secondary_idle  # ← 与 g_cpus 尾部重叠
```

表现为 `cpu_current()->cpu_id` 读出 `[C3207776]` 这类垃圾编号，看起来
**完全像运行期内存踩踏**，实际是构建产物不一致。血的教训：那半小时是在追一个幽灵。

**规则**：

1. 改过任何 `.h`、或切换 `SMP=` / `LOG=` / 其它 CFLAGS 之后，
   一律先 `make PLATFORM=<platform> clean` 再全量重编。
2. 怀疑构建产物不一致时，用 `nm` 量一下相邻全局符号的间距是否符合预期：
   ```bash
   x86_64-linux-musl-nm -n build/qemu-virt-x86_64/kernel_x86_64.elf \
     | grep -E " g_cpus$| g_secondary_idle$"
   # 间距应等于 AVATAR_MAX_CPUS * sizeof(cpu_t)
   ```

> 若要根治，可在 Makefile 里把所有目标的 `.d` 汇总成一个变量后 `-include`，
> 并加一个 CFLAGS 戳文件让编译选项变化触发全量重编。（尚未实施）

---

## 附二：已知但本次未修的隐患

`include/spinlock.h` 的 `spinlock_noirq_t` 把 `irq_flags` **存在锁对象内部**：

```c
typedef struct {
    volatile uint64_t lock;
    volatile uint64_t irq_flags;   /* ← 每把锁一个，而不是每 CPU 一个 */
} spinlock_noirq_t;
```

`spin_lock_irqsave()` 在**取到锁之前**就写这个字段，所以**自旋的输家会覆盖赢家保存的标志**；
释放时 `spin_unlock_irqrestore()` 读到的是最后一个写者的值，可能让 CPU 带着 IF=0
（或意外 IF=1）离开临界区。

本次死锁**不是**它触发的（真凶已定位为第三节），但它在 SMP 下确实是隐患。
正确做法是让 `arch_irq_save()` 按值返回、由调用者各自保存在栈上，而不是塞进锁里。
