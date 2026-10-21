# 多 vCPU：VM-exit 视角与设计准备

> 状态：**地基已落地，尚未启用 `nr_vcpus > 1`**。
> 当前所有调用方仍传 `nr_vcpus = 1`，现有行为与单 vCPU 完全一致。

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
| `vcpu_task_create()` 亲和性 | `vm->slot % ncpus`：同一 VM 的兄弟全挤一颗核 | `(vm->slot * MAX_VCPUS + vcpu_id) % ncpus` |
| `vmm_console_pump()` | 每颗 vCPU 退出都抢宿主 UART RX FIFO | 只由 BSP 泵（单消费者资源） |
| `vlapic_*` 运行时访问 | 固定取 vcpu0（`vlapic_cur`） | 所有 API 显式收 `vcpu_id` |
| `vm->state = RUNNING/DYING` | 绕过池锁直接写 | `vm_state_set()` / last-one-out 统一管理 |

## 5. 尚未做：真正启用多 vCPU 的前置条件

按建议实现顺序：

1. **AP 启动闸门。**
   `guest_loader_run_linux()` 目前只配置并创建 `vcpu[0]`。启用多 vCPU
   需要为非 BSP 创建任务，但**任务必须先在闸门处等待**，不能直接跳进
   guest 入口。需要一个可睡眠的 mailbox（`start_pending` + 等待队列）；
   注意 `task_block()` 的 lost-wakeup 窗口，不能只靠「查标志 + task_block」。

2. **每架构的 AP 入口。**
   - AArch64：PSCI `CPU_ON`（现在 `el2_run.c` 直接返回 `NOT_SUPPORTED`）；
     `context_id` / entry 要落到目标 vCPU 的 `elr/spsr/r[0]`。
   - RISC-V：SBI HSM `hart_start`（现在未实现，见 `hext_run.c` 的注释）；
     hartid ↔ vcpu_id 映射要固定。
   - x86：INIT/SIPI（现在 `vlapic.c` 明确忽略）。SIPI 的 vector 就是 AP
     入口页，需要先把 trampoline 写进 guest 低端内存。

3. **IPI / SGI 的跨 vCPU 语义。**
   - AArch64：`vmm_vgic3_set_sgi_pending()` 记录源核位图，但 GICv3 的 LR
     没有「SGI 源核」字段；真正跑起来需要 `ICC_SGI1R_EL1` 陷入与按目标
     vCPU 分发。
   - RISC-V：vPLIC 的 pending 是源级全局的，没有 per-hart pending；
     SBI IPI 需要单独实现。
   - x86：ICR 目前只处理「发给自己」（`vlapic.c` 的 `reg_write`），
     需要按 destination 字段寻址到目标 vLAPIC。

4. **跨 CPU 写入的原子性。**
   `vmm_vgic{,3}_set_pending()` 是普通 `|=`，`vlapic_raise_irq()` 也没有锁。
   单 vCPU 时写入者只可能是「即将进入 guest 的那颗核」，所以安全；
   多 vCPU 后，一颗 vCPU 在别的核上给兄弟置 pending 会与目标的
   sync/take 并发。启用前必须把这些位图改成原子操作（或加控制器锁）。

5. **guest 侧的 CPU 描述。**
   `nr_vcpus > 1` 时，aarch64/riscv 的 DTB 要有对应 `cpu@N` 节点与
   `enable-method`，x86 的 MP 表要有对应处理器项；否则 guest 根本不会
   去启动 AP。

6. **VM 级全量启动/回滚。**
   现在每个 vCPU 各自 `vcpu_task_create()`。多 vCPU 应提供
   `vm_start_vcpus(vm)`：全部创建成功才算启动；中途失败要置 `stop_req`
   并等待已创建的 vCPU 退出（不能直接 `vm_free()`，计数不为 0）。

## 6. 回归门禁

- 三架构 `make PLATFORM=<p> clean && make PLATFORM=<p> kernel`
- 单 vCPU 行为不变：
  - `tools/vmm_helper_regress.sh x86_64 1 1`
  - `tools/vmm_helper_regress.sh aarch64 1 1`
  - `tools/vmm_helper_regress.sh riscv64 1 1`
- 启用多 vCPU 后追加：
  - `tools/vmm_helper_regress.sh <arch> [N] [smp] --restart`
  - `tools/vmm_multivm_regress.sh <arch> [smp]`
  - x86 直启门禁 `tools/boot_regress.sh [N]`
