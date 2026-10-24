# 多 vCPU：VM-exit 视角与设计准备

> 状态：**aarch64 已支持 2/4/8/12 vCPU**，宿主 SMP 可以小于 guest vCPU
> 数（已用 2 宿主 / 8 guest、4 宿主 / 8 guest、4 宿主 / 12 guest 做
> 40s 启动门禁）。**x86_64 已支持 1..12 vCPU**（AP 入口 = guest 写 ICR 发
> SIPI；实测 2/1、4/2、4/4、8/4、12/4 宿主/guest 组合，以及两个 4-vCPU
> VM 同时运行）。**riscv64 已支持 1/2/4/8/12 vCPU**（SBI HSM `hart_start`
> 启动 AP、SBI IPI 注入 `hvip.VSSIP`、SBI RFENCE 在目标入口执行；实测
> 2 宿主 / 2、4、8 guest，以及两个 4-vCPU VM 同时运行）。超分配置要求
> guest 镜像是 `CONFIG_PREEMPT=y` 且 bootargs 带 `clocksource=jiffies`
> （见 §5.6）。
> 默认构建仍是 `nr_vcpus=1`。两种开启方式：
>   - **运行期**（推荐）：宿主 shell 里 `/bin/vmm-run -c N`，不需要重编内核；
>   - **构建期**：`GUEST_VCPUS=N`，它是“未指定 `-c` 时的默认值”，也是
>     `GUEST_LINUX=1` 直启使用的值。

## 1. 核心模型

VM-exit 时拿到的**永远是发生这次退出的那颗 vCPU**：

```c
int vmm_arch_exit_handler(vcpu_t *vcpu);
```

多 vCPU 之后，处理一次 exit 不应该再去猜「当前是哪颗」或「VM 里还有谁」。
下面这组接口统一放在 `include/vmm/vmm.h`：

| 问题 | 接口 |
|---|---|
| 我是哪颗 vCPU | `vmm_vcpu_id(vcpu)` / `vmm_vcpu_is_bsp(vcpu)` |
| 我属于哪个 VM | `vmm_vcpu_vm(vcpu)`（等价 `vcpu->vm`） |
| 这个 VM 还有哪些 vCPU | `vmm_vm_for_each_vcpu(vm, v)` / `vmm_vm_vcpu(vm, id)` |
| 某颗 vCPU 在哪颗宿主 CPU 上 | `vmm_vcpu_host_cpu(vcpu)`，`-1` = 未运行 |
| 某颗 vCPU 的承载任务 | `vmm_vcpu_task(vcpu)` |
| 给目标 vCPU 注入一根线 | `vmm_vcpu_inject_irq(target, virq_line(...))` |

`host_cpu` 由 `vmm_run_vcpu()` 在**每次进入 guest 前**用
`get_current_cpu_id()` 刷新。调度器不做运行期迁移（任务入队即粘核），
所以它既是「最近一次 exit 的核」，也是「这颗 vCPU 被钉在哪颗核」。

典型写法：

```c
static int some_exit(vcpu_t *vcpu)
{
    vm_t *vm = vmm_vcpu_vm(vcpu);

    vmm_vm_for_each_vcpu(vm, peer) {
        if (peer == vcpu)
            continue;
        KLOG_DEBUG("vm%u vcpu%d on cpu%d\n",
                   vm->vmid, vmm_vcpu_id(peer), vmm_vcpu_host_cpu(peer));
    }

    /* 给 1 号 vCPU 注入一条中断（不要求它正在当前核上跑） */
    vmm_vcpu_inject_irq(vmm_vm_vcpu(vm, 1), virq_line(VIRQ_CONSOLE));
    return EL2_RESUME;
}
```

## 2. 中断注入：目标 vCPU 是显式参数

`vmm_arch_irq_raise(vcpu_t *target, virq_t irq)` 的参数语义是**目标 vCPU**，
不要求是当前正在运行的 vCPU。三个后端：

| 架构 | 后端 | 目标 vCPU 如何体现 |
|---|---|---|
| AArch64 | `vmm_vgic{,3}_set_pending(&vm->vgic{,3}, target->vcpu_id, line)` | per-vCPU pending/LR |
| RISC-V | `vplic_set_pending(vm, line)` | **没有 per-vCPU 维度**：pending 是源级全局的，各 context 自行 claim |
| x86_64 | `vlapic_raise_irq(vm, target->vcpu_id, vector)` | vLAPIC 按 (vm->slot, vcpu_id) 寻址 |

调用点示例（同一 VM 内给兄弟注入）：

```c
vmm_vcpu_inject_irq(vmm_vcpu_sibling(vcpu, 1), virq_line(VIRQ_VTIMER));
```

**不要在设备代码里直接摸 `vm->vgic` / `vm->ept` / `g_vlapic`** ——
控制器型号是架构私有的，跨 vCPU 的操作统一走这条「线」的接口。

## 3. 生命周期：last-one-out

单 vCPU 时代的规则是「vCPU 任务退出即 `vm_free()`」。多 vCPU 下这会在
第一颗退出时拆掉还有兄弟在跑的 stage-2，因此改成引用计数：

```
vm_vcpu_started(vm)      创建任务之前 +1   （必须先加，任务可能立刻退出）
vm_vcpu_start_abort(vm)  创建任务失败 -1   （不销毁 VM，交给调用方错误路径）
vm_vcpu_stopped(vm)      任务退出 -1；归零才 vm_free()
```

`vm_t` 新增 `nr_vcpu_alive`，全部在 `g_vm_pool_lock` 下维护。
`vm_free()` 现在只在 `nr_vcpu_alive == 0` 时真正拆除，并用
`state = VM_DYING` 充当「销毁进行中」的排他标记，避免重复拆。

每颗 vCPU 的运行期状态：

```
VCPU_STATE_OFFLINE → VCPU_STATE_RUNNING → VCPU_STATE_STOPPED
```

- `OFFLINE`：任务未创建（将来也用于「AP 尚未被 CPU_ON」）；
- `RUNNING`：承载任务已创建并进入主循环；
- `STOPPED`：任务已退出，`host_cpu = -1`。

一颗 vCPU 退出（正常关机或错误）会置 **VM 级** `stop_req`，让兄弟 vCPU
在循环顶部各自退出；最后一个退出者负责 `vm_free()`。

## 4. 已同步修掉的单 vCPU 假设

| 位置 | 旧行为 | 现在 |
|---|---|---|
| `vcpu_task_create()` 亲和性 | `vm->slot % ncpus`：同一 VM 的兄弟全挤一颗核 | `(vm->slot + vcpu_id) % ncpus`（兄弟优先分散，单 vCPU 的 VM 按 slot 交替） |
| `vmm_console_pump()` | 每颗 vCPU 退出都抢宿主 UART RX FIFO | 只由 BSP 泵（单消费者资源） |
| `vlapic_*` 运行时访问 | 固定取 vcpu0（`vlapic_cur`） | 所有 API 显式收 `vcpu_id` |
| `vm->state = RUNNING/DYING` | 绕过池锁直接写 | `vm_state_set()` / last-one-out 统一管理 |

## 4a. Task 基础设施：跨核唤醒、抢占与次序

多 vCPU 之后，一颗 vCPU 在 VM-exit 里经常需要操作另一颗核上的兄弟。
下面这组原语就是为此补的，**不要**再用「直接改 task->state」或
「等下一次 timer tick」这类单核写法。

### 4a.1 跨核 kick

```
vmm_vcpu_kick(vcpu)
  → sched_resched_cpu(host_cpu)
      → cpu_send_ipi(cpu)
          AArch64: GIC SGI 15（GICv2/GICv3 各走 irq_send_ipi）
          RISC-V : SBI send_ipi → 目标 hart 的 SSIP
          x86_64 : LAPIC fixed-delivery IPI，IDT 向量 0xF0
```

目标核收到 IPI 只置 `need_resched`，真正的切换发生在：

- 宿主任务：中断返回路径 `sched_check_and_yield_from_trap()`；
- guest：IPI 被硬件转成一次 VM-exit / HS trap，VMM 主循环顶部
  `sched_check_and_yield()`。

因此 `vmm_vcpu_kick()` 对「宿主任务」和「guest 里的 vCPU」是同一套语义。
`vm_request_stop()` 现在会遍历所有 vCPU 逐个 kick —— 停止不再依赖
「兄弟恰好自己退出」。

### 4a.2 SMP 安全的阻塞/唤醒

`task_t` 新增 `on_cpu` / `wake_pending`，`sched_unblock()` 在目标核
`rq_lock` 下判定：

- 任务仍在 `on_cpu`（已置 BLOCKED、尚未切出）→ 只置 `wake_pending`；
  本核 `sched_schedule()` 在切出时补入队列；
- 已切出 → 直接入队，并在远端核上发 resched IPI。

这修掉了旧 `task_block()` 的 lost-wakeup / 「同一任务既在跑又在队列」
窗口。**新的等待逻辑请用 `completion_t`**（见 `kernel/task/completion.h`）：
它把 done 条件、等待队列和锁封在一起，天然避开这个窗口；AP 启动闸门
就应该建在它上面。

### 4a.3 中断注入的次序

所有 **VMM 侧**对某颗 vCPU 的注入统一走：

```c
vmm_vcpu_inject_irq(target, virq_line(...));
```

它做三件事，顺序不能换：

1. 取 `target->irq_lock`；
2. 调架构后端写 pending；
3. 释放锁（release 屏障）→ `vmm_vcpu_kick(target)`。

目标的 `vmm_arch_restore_guest_ctx()` / `save_guest_ctx()` 也持同一把
`irq_lock`，所以：

- 两次注入对同一 vCPU 有全序，不会互相覆盖 pending；
- 注入不会插进「LR 扫描/回读」的中间，状态要么本轮 entry 看到、要么
  被 kick 后下一轮看到；
- 宿主侧不再有「物理 vtimer ISR 直接注入」这条路径（`irq_route` 已删除）；
  vtimer 统一在每次 entry 由 `aarch64_check_vtimer()` 重新拉线。

**边界**：这把锁只覆盖 VMM 侧注入与 entry/exit 同步。guest 自己通过
GICD/vLAPIC MMIO 改 pending 发生在 exit handler 里（可能睡眠/让出），
不在锁内；它与远端注入的竞争按「下次 entry 的同步快照」收敛，不承诺
寄存器级的实时一致。若将来要把 guest MMIO 也纳入同一顺序域，需要把
pending 位图改成原子操作或给控制器加锁。

## 5. 各架构启用状态

| 前置条件 | aarch64 | riscv64 | x86_64 |
|---|---|---|---|
| AP 启动闸门（`completion_t`） | ✅ | ✅ `completion_wait(&start)` | ✅ `completion_wait(&start)` |
| AP 入口（CPU_ON / HSM / SIPI） | ✅ PSCI `CPU_ON`/`AFFINITY_INFO` | ✅ SBI HSM `hart_start` / `hart_get_status` | ✅ ICR SIPI → `vmm_vcpu_start()` |
| guest IPI（SGI / IPI / ICR） | ✅ `ICC_SGI1R_EL1` 陷入 → pending + kick | ✅ SBI `send_ipi` → pending + kick → 入口 `hvip.VSSIP` | ✅ ICR fixed/lowest，physical/logical flat/shorthand |
| guest CPU 描述 | ✅ `linux-gicv3-smp{2,4,8,12}.dtb`（GICR = N×0x20000） | ✅ `linux-smp{2,4,8,12}.dtb`（N 个 cpu@i + PLIC 的 N 个 S-mode context） | ✅ MP 表每 vCPU 一条 type-0（APIC ID == vcpu_id） |
| 控制器 per-vCPU 状态 | ✅ vGIC（VMCR/AP1R0、SGI 边沿 pending），见 5.2 | ✅ vPLIC（每 context enable/active/threshold；**pending 是源级全局**） | ✅ vLAPIC（ID/LDR/ISR/IRR/定时器） |
| VMM 侧注入与 entry/exit 互斥 | ✅ `vcpu->irq_lock` | ✅ `vcpu->irq_lock` | ✅ |
| guest 自己写 GICD/vLAPIC 与远端注入的严格顺序 | ⚠️ 未收口（见 4a.3） | ⚠️ | ⚠️ |
| VM 级全量启动/回滚 | ⚠️ 逐 vCPU 创建；失败置 stop_req | 同左 | 同左 |

### 5.1 aarch64 的实现要点

- **AP 任务先建、闸门后放行**：`guest_loader_run_linux()` 为 `vcpu[1..N-1]`
  建任务，它们启动后睡在 `vcpu->start`（completion）上；BSP 最后建。
- **PSCI CPU_ON**（`el2_run.c`）：按 DT `reg`（= vMPIDR 的 Aff0）找目标
  vCPU，`vmm_vcpu_start()` 清零 sysregs（SCTLR_EL1=0）、置 `elr/spsr/x0`，
  再 `completion_complete_all()`。AP 在另一颗宿主核上被 resched IPI 唤醒。
- **PSCI AFFINITY_INFO**：OFFLINE → 1（OFF），其余 → 0（ON）。
- **SGI**：guest 写 `ICC_SGI1R_EL1` 会陷入 EL2（EC=0x18）。`handle_sgi1r()`
  解析 TargetList/IRM/INTID，对每个目标 `vmm_vgic3_set_sgi_pending()`，
  再 `vmm_vcpu_kick()` 强制它退出 guest；下一次 entry 的 `sync_entry` 把
  SGI 排进 LR。
- **宿主摊核**：vCPU 任务 `(slot + vcpu_id) % ncpus`；宿主 SMP=2 时
  4/8 个 vCPU 落在 cpu0/1。**`cpu_bring_up_all()` 必须在创建 vCPU 任务
  之前**（否则 g_num_cpus=1，任务全钉 cpu0）。
- **WFI 立刻让出、WFE 限频让出**：`HCR_EL2.TWI/TWE=1` 会把 guest 空闲的
  WFI 和 qspinlock 里的 WFE 都变成 VM-exit。WFI（真 idle）必须每次
  `task_yield()`，否则空闲 vCPU 会和正在启动的 BSP 抢核。
  WFE 是自旋等待：**每次 WFE 都切任务**会形成上下文切换风暴，锁持有者
  拿不到稳定 CPU 时间（实测 1-2% 启动停滞）；**完全不让出**又会在 waiter
  屏蔽 IRQ 时让锁持有者等宿主 tick 等到 40s 以上。现在按 `ESR.ISS[0]`
  区分，WFE 最多每 1ms `task_yield()` 一次。

### 5.2 vGIC 的硬件状态必须按 vCPU 保存

`ICH_VMCR_EL2` 和 `ICH_AP1R0_EL2` 是 **per-pCPU 硬件**，但内容属于当前
在上面跑的 vCPU。同一 VM 的多个 vCPU 会在同一颗宿主核上分时跑，所以
它们必须和 `ICH_LR<n>_EL2` 一样按 vCPU 保存/恢复，不能只按 VM 存一份。
少存这两项时，vCPU A 的 PMR/IGRPEN/active priority 会被 vCPU B 覆盖；
vCPU 越多越容易命中，实测表现是 vtimer 的 LR 卡在 Active、guest idle
后再也醒不过来（宿主侧 vtimer 注入计数一直涨）。

当前实现：

- `vgic3_vcpu_t` 里保存 `ich_vmcr`、`ich_ap1r0` 和 16 个 `lr[]`；
- `vmm_vgic3_lr_switch_in()` 在 LR 归属切换时先保存上一个 vCPU 的
  VMCR/AP1R0，再装入当前 vCPU 的副本；
- `ICH_AP1R1/2/3_EL2` 不碰：QEMU 上实测会触发 "Unknown reason"，
  当前优先级位数下 AP1R0 已覆盖。

**SGI 是边沿触发，pending 生命周期与 PPI/SPI 不同**：

- 写进 LR 的那一刻消费软件 pending，之后同一 INTID 的新 SGI 才能再置位；
- 如果 vCPU 在 LR 仍 Pending 时被切走（硬件 LR 马上要清空），把 pending
  放回软件状态，切回来再注入；
- guest ack 时**不能**清 SGI 的 pending —— 否则 active 期间新到的同号
  IPI 会被抹掉，表现为跨核唤醒随机丢失、RCU/启动卡死。
- PPI/SPI 保持电平语义：pending 保留到 ack，设备侧每轮 entry 重新拉线。

**EOImode 与 DIR（中断取消/去激活）**：GICv3 有 deactivate 语义。
EOImode=0 时 `ICC_EOIR1` 一步完成 priority drop + deactivate；EOImode=1
时 EOI 只降优先级，另由 `ICC_DIR` 去激活。Linux guest 在 EL1 下
`is_hyp_mode_available()==false`，会关闭 split EOI/Deactivate 模式，
所以**当前 guest 用 EOImode=0，不会写 DIR**。代码仍设置
`ICH_HCR_EL2.TDIR=1` 并实现 `vmm_vgic3_deactivate()`，覆盖未来
EOImode=1 的 guest（active 中断可能已不在 LR 里，硬件 DIR 会丢，
必须由 VMM 清软件 active 位）；对当前 Linux guest 是 no-op。

### 5.3 stage-2 按需分页必须串行

guest 锁/内存缺页会由多个 vCPU 并发触发。`l3_ensure()` /
`stage2_map_page()` / `stage2_map_block()` 是「检查未映射 → PMM 分配 →
写 L2/L3 表项」，没有任何原子性；两个 vCPU 同时缺页会各自分配一套页或
L3 表并互相覆盖。被覆盖的那组页从 guest 视角看就是**内存被替换成了
另一组清零页**。

实测症状极具迷惑性：所有 vCPU 的 x3 指向同一个 qspinlock，全部停在
`queued_spin_lock_slowpath`，但没有任何一个 vCPU 在临界区里 —— 锁的
owner "消失"了。2 分 8 约 1-2% 启动停滞，vGIC/WFE 修复都消不掉。

修复：

- `s2_ctx_t.map_lock` 串行化缺页路径的「映射 + 复查」；
- 缺页成功的判据用 `stage2_lookup()` 得到的 `pa`，**不是**
  `stage2_map_block()` 的返回值：块已被另一个 vCPU 装好时它返回 0，
  但 lookup 命中，这是成功，不能当 PMM exhausted 退出 VM。

修复后 2 分 8 连续 200 次通过。

### 5.4 guest 的 FP/SIMD 现场必须按 vCPU 保存

`q0-q31 + FPCR/FPSR` 是**每 PCPU 一份**的物理状态，不属于某个 vCPU。
guest 在 EL0 用户态大量使用 NEON（静态 busybox 里有 1600+ 条 SIMD 指令），
而宿主 C 代码也被允许生成 SIMD；更关键的是同一 VM 的多颗 vCPU 会在同一
颗宿主核上分时跑。少保存这一组现场时：

- 宿主在 guest exit 后执行的 C 代码可以直接覆盖 guest 的 q 寄存器；
- vCPU A 被换下、vCPU B 的 guest 用户态跑一段 NEON 后再换回 A，
  A 的 NEON 现场已经全是 B 的值。

实测（8 个 EL0 进程 / 8 guest vCPU，每个进程把 q0-q31 填成 pid 相关图案、
延迟后回存比对）：

- 修复前：数秒内 `MISMATCH pid=1 count=1..6`，init 随后被内核杀掉
  （SIGILL，`Attempted to kill init! exitcode=0x00000004`）；
- 修复后：35 s 运行 0 MISMATCH。

实现见 `kernel/vmm/aarch64/el2_vmcs.S`：`vcpu_t.fpsimd[66]`（offset 576）
在 `el2_trap_exit` 保存、在 `el2_enter_guest` 恢复；宿主自己的
`d8-d15 + FPCR/FPSR` 由 `el2_enter_guest` 的 96 B 栈帧守住（AAPCS64）。
细节和验证方法见 `docs/arch/aarch64/FP_SIMD_CONTEXT.md` §三.5。

> 注意：FPCR/FPSR **没有 EL 银行**（汇编器没有 `fpcr_el1/el2` 编码），
> guest 用户态/内核和宿主共用同一份，所以这三组现场必须一起保存。

### 5.5 x86_64 的实现要点（INIT/SIPI + vLAPIC IPI）

与 aarch64 的 PSCI 模型不同，x86 的 AP 是 **guest 自己**通过写 ICR 启动的：
Linux 从 MP 表发现 CPU，BSP 写 ICR 发 INIT（deassert）→ SIPI，AP 从实模式
`vector << 12` 开始执行，再自己切到保护/长模式。VMM 要做的是四件事：

1. **描述 CPU**：`guest_boot.c` 的 MP 表每个 vCPU 一条 type-0 条目，
   APIC ID 约定 == `vcpu_id`。三处必须一致：MP 表、`CPUID.1.EBX[31:24]`、
   vLAPIC 的 ID 寄存器。`IA32_APIC_BASE` 的 BSP 位只有 0 号 vCPU 置位。
2. **建 AP 任务**：与 aarch64 相同，AP 任务先建、睡在 `vcpu->start` 上；
   BSP 最后建。AP 的 VMCS 不在装载期建，由第一次 entry 在**自己那颗宿主核**
   上建（`vmcs_ready` 守卫）。
3. **SIPI → 实模式 VMCS**：`vlapic.c` 的 ICR 写处理解析目标后调
   `vmm_vcpu_start(target, vector << 12, 0)`；`vmcs_init_guest()` 走
   `g_ap_reset` 分支，构造 16 位实模式现场（详见 X86_GUEST_LINUX.md §12）。
4. **IPI**：ICR fixed/lowest 的 destination 解析覆盖 physical、logical flat
   与 self/all/all-but-self shorthand，经 `vmm_vcpu_inject_vector()` 持目标
   `irq_lock` 投递再 kick。

多 vCPU 顺带修掉的四个坑（每个都只在多核下才现形）：

| 坑 | 症状 | 修法 |
|---|---|---|
| AP 实模式 `GUEST_CR0` 用了架构 reset 值 0x60000010（NE=0） | VM-entry reason 33，`inst_error` 读成 0，寄存器快照看着全合法 | `CR0 = ET\|NE = 0x30`（VMX fixed0 去掉 PE/PG 后仍要求 NE=1） |
| vLAPIC 定时器按**墙钟**计时 | 宿主核被多个 vCPU 分时（1 核 2 guest 核）时，guest ISR 跨过多个 tick；每次 entry 都再次到期 → 永远在处理中断，CPU bring-up 的 callout 握手都执行不到 | 定时器改用 `exec_ns`（只累计 guest 真正执行的时间），由 `vmm_run_vcpu()` 在 entry/exit 前后量 TSC 差累加 |
| DCR 分频表整体错位一格（0x3 当 ÷8，实际 ÷16） | guest tick 快一倍，jiffies 也快一倍 | 按 SDM 表 11-19 重写（0xB = ÷1） |
| EPT 缺页路径无锁；且把 `map_block()==0` 当 PMM 耗尽 | 两个 vCPU 同时缺页到同一 2 MiB 块会互相覆盖；兄弟已装好时返回 0 会被误判为失败并停 VM | `ept_ctx_t.map_lock` 串行化；成功判据改成 `x86_ept_lookup()` |

另外两条较小的：

- `vmm_arch_irq_raise()`（IO-APIC 设备的电平线）现在按重定向表的目的地
  过滤，否则一条只路由给 CPU0 的串口中断会在每颗 vCPU 上各投一次；
- `guest_va_to_gpa()` 取物理地址时只保留 bit 12-51 —— 原来 `e & ~0xFFF`
  会把 NX(bit 63) 带进 GPA，栈/数据页（PTE 带 NX）的翻译结果会变成
  `0x800000000_xxxxxxxx`，用它查 EPT 必然失败。

**x86 的 INIT 语义是"不完整"的**：对 OFFLINE 的 AP 什么都不做（它本来就在
等 SIPI）；对已 RUNNING 的 vCPU 的 INIT（关机/热插拔）不复位 VMCS。正常
启动流程只用 INIT(deassert)+SIPI 拉起从未运行过的 AP，够用。

### 5.6 riscv64 的实现要点（SBI HSM + IPI + RFENCE）

Linux RISC-V 的 SMP 路径完全走 SBI：从 DTB 的 `cpu@i` 发现 hart，BSP 逐个
`SBI HSM hart_start(hartid, start_addr, opaque)`。VMM 侧对应四块：

1. **AP 入口**（`handle_vs_ecall` → `sbi_hsm_handler` → `vmm_vcpu_start`）：
   SBI HSM 规定的复位现场是 sepc=start_addr、satp=0、sstatus.SIE=0、
   a0=hartid、a1=opaque（Linux 传的是 per-CPU 的 `sbi_hart_boot_data`，
   不是 DTB）。`hstatus_save` 预置 SPVP=1，否则第一次 sret 会落到 VU-mode。
   AP 任务仍然是先建、睡在 `vcpu->start`。
   `hart_stop`（CPU 热插拔）回 `NOT_SUPPORTED`，避免把 vCPU 任务退出误当成
   整个 VM 停机；正常关机走 SRST。
2. **IPI**：`SBI send_ipi(a0=mask, a1=base)` 只给目标 vCPU 置
   `ipi_pending`/`ipi_seq` 并 kick；**hvip 是每 hart 的 CSR**，远端写不到，
   真正置 `HVIP_VSSIP` 必须等目标下一次入口（`vcpu_software_irq_on_entry`）。
   退出时怎么判断"这次 IPI 被 guest 消费了"是这里唯一的难点：
   guest 清 `sip.SSIP` 会连带清掉 `hvip.VSSIP`（两者是同一个挂起位的两个
   视图），但 guest 运行期间**新来的** IPI 也会让 VSSIP 保持 0（远端碰不到
   本 hart 的 hvip）。只看 VSSIP 会误清新 IPI → IPI 风暴。这里用
   `ipi_seq` 快照：入口记 seq，exit 时序号没变且 VSSIP 已被清，才清 pending
   （KVM 用 `irqs_pending_mask` 解决同一问题）。
3. **RFENCE**：SBI 要求目标 hart 自己执行 `fence.i` / `sfence.vma`；vCPU 是
   分时任务，没法当场代执行，于是记到目标的 `remote_fence`，由它下一次进
   guest 前在 `vcpu_remote_fence_on_entry()` 执行。地址/ASID 一律忽略、全刷，
   语义永远安全；HFENCE_*（嵌套虚拟化）按 KVM 做法 NOP 成功。
4. **guest CPU 描述**：`tools/gen_rv64_smp_dtb.py` 生成
   `linux-smp{2,4,8,12}.dtb`，N 个 `cpu@i` + `cpuN_intc`，PLIC 的
   `interrupts-extended` 按 **cpu0..cpuN-1 的顺序** 列 N 个 S-mode context。
   这个顺序必须和 vPLIC 的「context N == vCPU N」严格一致；写反的症状是
   某颗核永远收不到外部中断，且没有任何报错。生成脚本自带回读自检。

多 vCPU 顺带修掉/加固的东西：

| 项 | 症状 | 修法 |
|---|---|---|
| vPLIC 的 pending 曾是每 context 一份 | 同一个 UART 中断同时投给两颗核，各 claim 一次，8250 RX 只有一个消费者 → 随机丢输入 | 改成源级全局 pending + 每 context 的 enable/active/threshold（claim 清全局挂起位，PLIC 语义） |
| G-stage 表操作无锁；`map_block()==0` 当 PMM 耗尽 | 两个 vCPU 同缺一个 2 MiB 块会重复建表/双映射；兄弟已装好时返回 0 被误判成 OOM 停 VM | `gstage_ctx_t.map_lock` 串行化；成功判据改成 `rv_gstage_lookup()` |
| 每次入口按 map_gen 全刷 G-stage TLB | 4 vCPU 并行启动时几乎每轮都刷，启动从秒级退化到几十秒 | 删掉代际刷。RISC-V 允许缓存「不可翻译」，负缓存只会再 fault，缺页路径会本地刷；映射建立后不改指向 |
| guest 运行期间宿主定时器 trap 不经过宿主通用陷阱分发器 | `timer_handler` 不执行 → STIP 不再重装；多 vCPU 分时后表现为启动无声卡死 | `vmm_arch_exit_handler` 里用合成 trap_frame 调 `riscv_dispatch_host_irq()` |
| WFI 只做 yield 后立刻重进 guest | 空闲 vCPU 变成 WFI exit/entry 忙循环，超分时 CPU 被吃光 | WFI 挂到 per-vCPU `wfi_wake` completion；kick 与宿主 tick 唤醒。BSP 每 tick 回主循环一次，保住直启模式的宿主控制台输入泵 |

## 6. aarch64 多 vCPU guest 实测

### 6.1 运行期指定（helper 模式，推荐）

不需要为核数重编内核：用默认单核构建即可，rootfs 里预装了全部候选 DTB。

```bash
# 默认 GUEST_VCPUS=1 的 helper 模式内核（不是 GUEST_LINUX=1 直启变体）
make PLATFORM=qemu-virt-aarch64 SMP=2 clean
make PLATFORM=qemu-virt-aarch64 SMP=2 LOG=info kernel rootfs

# 按 tools/vmm_helper_regress.sh 的方式起 QEMU，或直接：
make PLATFORM=qemu-virt-aarch64 SMP=2 LOG=info run-fs

# 宿主 shell 里：
/bin/vmm-run -c 8
```

判据：

- helper 打印 `new VM (vmN, 8 vCPU)`；
- 宿主日志有 `[vmmdev] boot: starting guest (8 vCPU(s))`、
  `[vmm] vm_create: 8 vCPU(s) initialized`、8 条 `vmN vcpuX task started`；
- guest 里 `grep -c ^processor /proc/cpuinfo` = 8，
  `dmesg | grep "Brought up"` 含 8 CPUs，最后出现 `~ #`。

`-c` **隐含 `-n`**：已有 guest 时总是再新建一个，旧 guest 留在后台继续跑
（运行中的 VM 改不了核数）。不带 `-c` 时使用构建期 `GUEST_VCPUS` 的默认值。
支持的计数：aarch64 的 `1/2/4/8/12`（要求 GICv3 构建）；x86_64/riscv64 只有
`1`，`-c 2` 会立即返回 `EINVAL`。

DTB 按**实际核数**选择（rootfs 预装，见 Makefile 的
`GUEST_LINUX_BASE_DTB_SRC` / `GUEST_LINUX_MULTI_DTBS`）：

| 实际 vCPU | 使用的 DTB |
|---|---|
| 1（构建默认也是 1） | `/guests/linux/linux.dtb` |
| 1（构建默认是多核） | `/guests/linux/linux-1cpu.dtb` |
| 2/4/8/12 | `/guests/linux/linux-gicv3-smpN.dtb` |

> NPU 构建（`GUEST_NPU=1`）只有单核 DTB。`-c N>1` 会选无 NPU 节点的 smp
> DTB —— 该 VM **不做 NPU 透传**（与构建期 `GUEST_VCPUS>1` 的限制一致）；
> 单核仍使用带 NPU 节点的 `linux.dtb`。

### 6.2 构建期（直启）

```bash
# 2 宿主核 / 8 guest vCPU；其余组合只改 SMP/GUEST_VCPUS
make PLATFORM=qemu-virt-aarch64 SMP=2 GUEST_VCPUS=8 GUEST_LINUX=1 clean
make PLATFORM=qemu-virt-aarch64 SMP=2 GUEST_VCPUS=8 GUEST_LINUX=1 kernel rootfs
timeout 40 make PLATFORM=qemu-virt-aarch64 SMP=2 GUEST_VCPUS=8 \
    GUEST_LINUX=1 LOG=info test-guest-linux
```

判据（8 vCPU 实测输出）：

```
smp: Bringing up secondary CPUs ...
CPU1: Booted secondary processor 0x0000000001
...
CPU7: Booted secondary processor 0x0000000007
smp: Brought up 1 node, 8 CPUs
SMP: Total of 8 processors activated.
~ #
```

宿主侧能看到 8 个 `vm1 vcpuN task started`，在 cpu0/cpu1 上各 4 个。
`GUEST_VCPUS=N` 会自动选 `imgs/guests/aarch64/linux-gicv3-smpN.dtb`
（N 个 cpu@N、GICR 区 N×0x20000），并把 `GUEST_LINUX_NR_VCPUS=N`
传给 guest_loader。

启动稳定性矩阵（QEMU 超时 40s，干净构建）：

| 宿主 SMP | GUEST_VCPUS | 运行次数 | 结果 |
|---|---|---|---|
| 2 | 4 | 50 | 50/50 |
| 2 | 8 | 200 | 198/200（stage-2 map_lock + WFE 限频版；失败的 2 次是 FP/SIMD 现场缺失，见 §5.4） |
| 4 | 8 | 30 | 30/30 |
| 4 | 12 | 30 | 30/30 |

三架构回归（同一提交）：

- LTP：aarch64 / riscv64 / x86_64 各 **63/63 PASS**（0 FAIL / 0 BROK）。
- 原 `VMM_TEST=1` 测试：三架构都能启动并创建三个线程，持续运行到超时。

> 历史坑：
> 1. 只按 VM 保存 `ich_vmcr`、SGI pending 在注入时不清/在 ack 时乱清 ——
>    2 分 8 约 20% 启动停滞，vGIC 按 vCPU 保存/恢复后降到 1-2%。
> 2. WFE 的让出策略：每次让出会形成切换风暴；完全不让出会出现 42s 的
>    极端慢启动。改成 1ms 限频。
> 3. stage-2 缺页并发覆盖：所有 vCPU 排在同一把 qspinlock 上、owner
>    却不存在。`map_lock` 串行化 + 用 lookup 的 pa 判定成功后，2 分 8
>    连续 200 次通过。

### 6.3 x86_64 实测（helper 模式，运行期 `-c`）

不需要 DTB，也不需要为核数重编内核（默认 `GUEST_VCPUS=1` 的 helper 构建即可）：

```bash
make PLATFORM=qemu-virt-x86_64 SMP=2 clean
make PLATFORM=qemu-virt-x86_64 SMP=2 LOG=info kernel rootfs

# 4 vCPU / 2 宿主核（故意超售）
VCPUS=4 tools/vmm_helper_regress.sh x86_64 1 2

# 含 Ctrl+T k 停止 + 重启
VCPUS=4 tools/vmm_helper_regress.sh x86_64 2 2 --restart
```

判据：

- 宿主日志：`Linux vCPU task id=... running (4 vCPU(s))` + 4 条
  `vm1 vcpuX task started`（`(slot + vcpu_id) % ncpu` 摊到宿主核上）；
- 4 次 `SIPI -> vcpuN`；
- guest 里 `grep -c ^processor /proc/cpuinfo` = 4，最后出现 `~ #`。

**多个 4-vCPU VM 同时跑**（q35 / 宿主 2 核实测）：

```
Launching busybox shell
/ # /bin/vmm-run -c 4        → guest uname + grep -c ^processor = 4
~ # Ctrl+T d                 → 回宿主，vm1 留在后台
/ # /bin/vmm-run -c 4        → 第二个 VM，同样 4 vCPU
/ # /bin/vmm-run -l          → 两行 RUNNING（vm1 / vm2）
```

实测矩阵（QEMU/KVM 嵌套，helper 模式）：

| 宿主 SMP | guest vCPU | 场景 | 结果 |
|---|---|---|---|
| 2 | 1 | 单 vCPU 基线（2 轮） | 2/2 |
| 1 | 2 | 一个宿主核跑两个 vCPU | 1/1 |
| 2 | 4 | 超售 2× + `--restart` | 2/2 |
| 4 | 4 / 8 / 12 | 独立核、flat 与 physflat IPI | 各 1/1 |
| 2 | 4 + 4 | 两个 4-vCPU VM 同时运行 | 两个 guest 都到 shell，`-l` 两行 RUNNING |

> 关键前提是 §5.5 表格第二行：vLAPIC 定时器必须按 **guest 执行时间** 计时。
> 用墙钟时，只要宿主核数少于 vCPU 总数，guest 的 CPU bring-up 就会卡在
> `cpu_callout_mask` 握手（BSP 被 tick 风暴困在 ISR 里，一条普通指令都跑不了）。

### 6.4 riscv64 实测（helper 模式，运行期 `-c`）

| 宿主 SMP | guest vCPU | 场景 | 结果 |
|---|---|---|---|
| 2 | 2 | 基线（不超分） | 1/1，约 4s |
| 2 | 4 | 超售 2× + `--restart` | 1/1，约 13s（两次启动） |
| 2 | 8 | 超售 4× | 1/1，约 23s |
| 2 | 4 + 4 | 两个 4-vCPU VM 同时运行 | 两个 guest 都到 uname，两个 `vcpu0 task started` |
| 2 | 4 / 8 | 稳定性连跑 | 4/2 6/6、8/2 6/6（3.3–16.5s） |

前提（缺一不可）：

- `imgs/guests/rv64/linux.bin` 是 `CONFIG_PREEMPT=y` 的（由原镜像的
  IKCONFIG 提取配置、只改抢占模型重建；镜像里的 `uname -a` 应带 `PREEMPT`）。
  非抢占 guest 在超分时会在 `stop_machine` 等跨 CPU 握手上偶发卡几十秒。
- guest vCPU 数 > 宿主 CPU 数时，`guest_loader.c` 自动把 bootargs 换成带
  `clocksource=jiffies` 的那一版，跳过 guest 启动期的时钟源切换
  `stop_machine`（§5.6 表格最后一行）。`guest vCPU <= 宿主 CPU` 时仍用
  `riscv_clocksource` 的高分辨率时间。

## 7. 回归门禁

- 三架构 `make PLATFORM=<p> clean && make PLATFORM=<p> kernel`
- 三架构 LTP（自动化）：
  `python3 tests/ltp/run_qemu_ltp.py $(make PLATFORM=<p> -n test-ltp | ...)`
  —— 当前基线 63/63 PASS
- 三架构原 VMM 测试：`make PLATFORM=<p> VMM_TEST=1 run` —— 应看到
  `=== Avatar OS: ... VMM Test ===`、Thread 1/2/3 创建并持续打印
- 单 vCPU 行为不变：
  - `tools/vmm_helper_regress.sh x86_64 1 1`
  - `tools/vmm_helper_regress.sh aarch64 1 1`
  - `tools/vmm_helper_regress.sh riscv64 1 1`
- 跨核路径（**必须 SMP>=2**，否则 IPI/摊核根本不执行）：
  - `tools/vmm_helper_regress.sh <arch> 1 2 --restart`
  - `tools/vmm_multivm_regress.sh <arch> 2` —— 两个 VM 应分别落在 cpu0/cpu1
  - `tools/task_infra_test.sh <arch> 2` —— 用户态 dist/ping，验证跨核唤醒
    延迟与摊核；A/B 方法与实测结果见 `docs/basic/TASK_INFRA_TEST.md`
  - `make ... TASK_TEST=1 kernel rootfs` +
    `tools/task_completion_test.sh <arch> 2` —— completion 广播的用户态 A/B
- 启用多 vCPU 后追加：
  - `VCPUS=8 tools/vmm_helper_regress.sh aarch64 [N] [smp] --restart`
    —— 脚本会发送 `/bin/vmm-run -c 8`，并断言宿主日志里的
    `running (8 vCPU(s))`；
  - x86_64 多 vCPU：
    - `VCPUS=4 tools/vmm_helper_regress.sh x86_64 [N] 2 --restart`
      （超售路径，必须跑；1 核 2 vCPU 也应当通过）
    - `VCPUS=12 tools/vmm_helper_regress.sh x86_64 [N] 4`
      （>8 核走 physflat 物理目标 IPI）
    - 多 VM + 多 vCPU：两个 `/bin/vmm-run -c 4` 各自跑到 uname，
      `vmm-run -l` 两行 RUNNING（`tools/vmm_multivm_regress.sh` 只覆盖
      1-vCPU VM，多 vCPU 组合按 §6.3 手动跑）
  - riscv64 多 vCPU（SBI HSM/IPI/RFENCE）：
    - `VCPUS=4 tools/vmm_helper_regress.sh riscv64 [N] 2 --restart`
      （超售：2 宿主核 / 4 guest vCPU）
    - `VCPUS=8 tools/vmm_helper_regress.sh riscv64 [N] 2`
      （超售：2 宿主核 / 8 guest vCPU）
    - `tools/vmm_multivm_regress.sh riscv64 2`
  - `tools/vmm_helper_regress.sh <arch> [N] [smp] --restart`
  - `tools/vmm_multivm_regress.sh <arch> [smp]`
  - x86 直启门禁 `tools/boot_regress.sh [N]`
