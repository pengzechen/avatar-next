# 多 vCPU：VM-exit 视角与设计准备

> 状态：**aarch64 已支持 2/4/8/12 vCPU**，宿主 SMP 可以小于 guest vCPU
> 数（已用 2 宿主 / 8 guest、4 宿主 / 8 guest、4 宿主 / 12 guest 做
> 40s 启动门禁）。riscv64 / x86_64 的 AP 入口仍未实现，只支持单 vCPU。
> 默认构建仍是 `nr_vcpus=1`；多 vCPU 由 `GUEST_VCPUS=N` 显式开启。

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
| AP 启动闸门（`completion_t`） | ✅ | 结构已备，未接 SBI HSM | 结构已备，未接 SIPI |
| AP 入口（CPU_ON / HSM / SIPI） | ✅ PSCI `CPU_ON`/`AFFINITY_INFO` | ❌ | ❌ |
| guest IPI（SGI / IPI / ICR） | ✅ `ICC_SGI1R_EL1` 陷入 → pending + kick | ❌ | ❌ |
| guest CPU 描述 | ✅ `linux-gicv3-smp{2,4,8,12}.dtb`（GICR = N×0x20000） | ❌ | ❌ |
| vGIC per-vCPU 状态（VMCR/AP1R0、SGI 边沿 pending） | ✅ 见 5.2 | — | — |
| VMM 侧注入与 entry/exit 互斥 | ✅ `vcpu->irq_lock` | ✅ | ✅ |
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

## 6. aarch64 多 vCPU guest 实测

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
| 2 | 8 | 200 | **200/200**（WFI/WFE 限频修复后） |
| 4 | 8 | 30 | 30/30 |
| 4 | 12 | 30 | 30/30 |

三架构回归（同一提交）：

- LTP：aarch64 / riscv64 / x86_64 各 **63/63 PASS**（0 FAIL / 0 BROK）。
- 原 `VMM_TEST=1` 测试：三架构都能启动并创建三个线程，持续运行到超时。

> 历史坑：
> 1. 只按 VM 保存 `ich_vmcr`、SGI pending 在注入时不清/在 ack 时乱清 ——
>    2 分 8 约 20% 启动停滞，vGIC 按 vCPU 保存/恢复后降到 1-2%。
> 2. WFE 的让出策略：每次让出会形成切换风暴；完全不让出会出现 42s 的
>    极端慢启动。改成 1ms 限频后，2 分 8 连续 200 次通过。

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
  - `tools/vmm_helper_regress.sh <arch> [N] [smp] --restart`
  - `tools/vmm_multivm_regress.sh <arch> [smp]`
  - x86 直启门禁 `tools/boot_regress.sh [N]`
