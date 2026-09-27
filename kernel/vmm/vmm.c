/*
 * kernel/vmm/vmm.c — VM 管理与 vCPU 执行主循环
 *
 * 架构无关层：vmm_run_vcpu 通过架构钩子驱动 guest 执行。
 * 各架构在 arch/el2_run.c 或 arch/vmx.c 中提供钩子实现：
 *   vmm_arch_restore_guest_ctx / vmm_arch_enter_guest
 *   vmm_arch_exit_handler       / vmm_arch_save_guest_ctx
 */

#include "vmm/vmm.h"
#include "vmm/vmm_mmio.h"
#include "vmm/vmm_console.h"
#if ARCH_AARCH64
#include "vmm/vmm_irq_route.h"
#endif
#include "uart/uart.h"
#include "klog.h"
#include "spinlock.h"     /* VM 池的槽位锁 */
#include "string.h"
#include "task/switch.h"
#include "task/sched.h"

#if ARCH_AARCH64
#include "aarch64/stage2.h"
#include "vmm/vmm_vpl011.h"
#if DRIVER_GIC_V3
#include "vmm/vmm_vgicv3.h"
#else
#include "vmm/vmm_vgicd.h"
#include "vmm/vmm_vgic.h"
#include "vmm/vmm_vgicc.h"
#endif

/* ── AArch64 VM 初始化 ────────────────────────────────────── */
static int aarch64_vm_init(vm_t *vm)
{
    int i;
    int nr = vm->cfg.nr_vcpus;

    if (nr < 1 || nr > MAX_VCPUS) {
        KLOG_ERROR("[vmm] vm_create: invalid nr_vcpus=%d\n", nr);
        return -1;
    }

    /*
     * 初始化本 VM 自己的 Stage-2 —— **空表**。
     *
     * 空表即"全 trap"：guest 访问任何 IPA 都会陷入 EL2，由缺页处理决定是
     * 模拟设备还是分配一页 RAM。所以不再需要老版本那句
     * stage2_enable_mmio_trap()（它是"先把 4 GiB 全映射再清掉非 RAM"，
     * 本质是同一件事，只是绕了一圈）。
     *
     * 调用方（guest_loader）随后用 stage2_map_range() 把内核映像/DTB/initrd/
     * 初始栈这几块宿主自己要写的地方显式映射进去，其余全靠缺页。
     */
    stage2_vm_init(&vm->s2, (uint32_t)vm->slot, vm->vmid,
                   vm->cfg.mem_base, vm->cfg.mem_size);

    mmio_bus_init(&vm->mmio_bus_storage);

    /* 建立 MMIO 总线并注册虚拟设备（虚拟 PL011 控制台）*/
#if DRIVER_GIC_V3
    if (vmm_vgic3_init(&vm->vgic3, (uint32_t)nr) != 0)
        return -1;
#else
    if (vmm_vgic_init(&vm->vgic, (uint32_t)nr) != 0)
        return -1;
#endif

    if (vpl011_init(vm, &vm->vpl011_dev, &vm->mmio_bus_storage) != 0) {
        KLOG_WARN("[vmm] vpl011 registration failed\n");
    }

    /*
     * vGIC layering:
     *   - vgic/vgic3 : VM-level virtual interrupt lifecycle state
     *   - vgicd/vgic3d: guest GICD MMIO configuration and forwarding
     *   - vgicc      : (GICv2) per-vCPU GICC MMIO + GICH LR cache
     *   - vgic3r     : (GICv3) per-vCPU redistributor（SGI/PPI 的 GICR）
     */
#if DRIVER_GIC_V3
    /* GICv3：GICD 只管 SPI，SGI/PPI 在 GICR；CPU interface 是系统寄存器，
     * VMM 靠 ICH_LR<n>_EL2 注入 —— 没有 GICC MMIO 设备。*/
    if (vgic3r_init(&vm->vgic3r_dev, &vm->mmio_bus_storage, &vm->vgic3) != 0) {
        KLOG_WARN("[vmm] vgic3r registration failed\n");
    }
    if (vgic3d_init(&vm->vgic3d_dev, &vm->mmio_bus_storage, &vm->vgic3) != 0) {
        KLOG_WARN("[vmm] vgic3d registration failed\n");
    }
    /*
     * 注意：**不在这里**开 ICH_HCR_EL2.En。
     * 那是每 CPU 的系统寄存器，必须设在真正跑 vCPU 的核上，而本函数跑在
     * 调用者的核上（从 /dev/vmm 启动时是用户态 helper 所在的核）。
     * 见 vmm_vgic3_hw_init() 的注释 —— SMP>1 时在这里设会直接让 guest 收不到
     * 虚拟中断。真正的设置点在 vmm_arch_restore_guest_ctx()，每次进 guest 前。
     */
#else
    /* 虚拟 CPU 接口：GICC MMIO + per-vCPU GICH LR 缓存。*/
    if (vgicc_init(&vm->vgicc_dev, &vm->mmio_bus_storage, &vm->vgic) != 0) {
        KLOG_WARN("[vmm] vgicc registration failed\n");
    }
    /* 虚拟 GICv2：GICD/GICC 由 MMIO 模拟，vgic core 维护软件中断状态。*/
    if (vgicd_init(&vm->vgicd_dev, &vm->mmio_bus_storage, &vm->vgic) != 0) {
        KLOG_WARN("[vmm] vgicd registration failed\n");
    }
#endif
    vm->mmio_bus = &vm->mmio_bus_storage;

#if DRIVER_GIC_V3
    KLOG_INFO("[vmm] MMIO bus ready: PL011 @0x%llx, GICD @0x%llx, GICR @0x%llx\n",
              (unsigned long long)VPL011_BASE,
              (unsigned long long)VGIC3D_BASE,
              (unsigned long long)VGIC3R_BASE);
#else
    KLOG_INFO("[vmm] MMIO bus ready: PL011 @0x%llx, GICD @0x%llx\n",
              (unsigned long long)VPL011_BASE,
              (unsigned long long)VGICD_BASE);
#endif

    /* 初始化每个 vCPU 的状态 */
    for (i = 0; i < nr; i++) {
        vcpu_t *vcpu = &vm->vcpus[i];
        memset(vcpu, 0, sizeof(*vcpu));
        vcpu->vcpu_id  = i;
        vcpu->launched = 0;
        vcpu->vm       = vm;
    }
    vm->nr_vcpus = nr;

    KLOG_INFO("[vmm] vm_create: %d vCPU(s) initialized, mem=0x%llx+0x%llx\n",
              nr, vm->cfg.mem_base, vm->cfg.mem_size);
    return 0;
}
#endif /* ARCH_AARCH64 */

/* ── 公开 API ─────────────────────────────────────────────── */

int vm_create(vm_t *vm)
{
#if ARCH_AARCH64
    return aarch64_vm_init(vm);
#elif ARCH_X86_64
    extern int vmx_vm_init(vm_t *vm);
    return vmx_vm_init(vm);
#elif ARCH_RISCV64
    extern int hext_vm_init(vm_t *vm);
    return hext_vm_init(vm);
#endif
}

#if VMM_GUEST_LINUX_SUPPORTED
/*
 * vmm_console_pump — 宿主控制台 → guest 虚拟 UART 的输入桥
 *
 * 轮询宿主真实 UART 的 RX FIFO，把用户按键推进 guest 控制台 vdev 的
 * RX FIFO（对标 kvmm 的 RxChannel::push；注入中断由 VMM 在进入 guest
 * 前做，见各架构的 vmm_arch_restore_guest_ctx）。
 *
 * 为什么轮询而不是让宿主收 RX 中断：guest 运行期间宿主中断是关的，
 * 自己的 RX 中断根本进不来。而退出路径的调用频率足够高 ——
 *   - guest 空闲：每条 WFI 都陷入 hypervisor，实测 ~3 万次/秒；
 *   - guest 满载：宿主定时器每 10ms 也会把它踹回宿主一次。
 * 对交互式控制台来说，最坏 10ms 的输入延迟完全够用。
 *
 * uart_rx_ready() 在 UART 未开中断时直接查硬件 FIFO 状态位，非阻塞，
 * 所以这里必须先用它把关 —— uart_getc() 在没有数据时是阻塞的。
 *
 * ⚠️ 只在控制台 TX 通道**关闭**时才能跑。helper 模式（/bin/vmm-run）
 * 下宿主 tty 层独占真实 UART：用户按键由 helper 从自己的 stdin 读走，
 * 再 write() 到 /dev/vmm。此时若本函数也在跑，两个消费者会从同一个硬件
 * FIFO 抢字节（tty.c 的 signal_check_uart() 是第一个，本函数是第二个），
 * 谁先跑谁拿到，输入会随机丢给错误的一方。
 */
static void vmm_console_pump(vm_t *vm)
{
    /*
     * 归属在别的 VM 手上就让位 —— 从前这是一个全局的 tx_channel 开关，
     * 多 VM 之后每个 VM 各有自己的 console_owned：只有"没有前台 VM"时才
     * 需要 VMM 自己去泵主机控制台，否则宿主 tty 层（helper）独占真实 UART。
     */
    if (vmm_console_tx_channel_enabled(vm))
        return;

    while (uart_rx_ready())
        vmm_console_push_rx(vm, (uint8_t)uart_getc());
}
#endif /* VMM_GUEST_LINUX_SUPPORTED */

/* ── 宿主侧 guest 生命周期：VM 池 ─────────────────────────────
 *
 * 从前这里只有一个 VM —— guest_loader.c 里一个 `static vm_t vm`，加上下面
 * 两个全局标志（running / stop）。那套从数据结构上就假定了"内核里最多一个
 * VM"：第二个 VM 会直接覆盖第一个的对象，而 stop 请求也没法说清是给谁的。
 *
 * 现在改成静态池。为什么不用动态分配：vm_t 里嵌着 vGIC（GICv3 时约 70 KB）
 * 与 stage-2 静态表，sizeof 有几十 KB，动态分配要几十页**连续**物理内存、
 * 还会丢掉表所需的页对齐保证；静态池一共几百 KB BSS，相对内核窗口可忽略。
 *
 * 并发规则（重要）：
 *   - 槽位与 state 由 g_vm_pool_lock 保护（IRQ-safe：vmm_dev 的任务上下文
 *     和 vCPU 任务上下文都会碰）；
 *   - **跨任务只传 vmid，不传 vm_t *，也不做引用计数** —— 谁要操作某个 VM
 *     就现场 vm_get(vmid) 取一次、用完即放；
 *   - **销毁由该 VM 的 vCPU 任务自己完成**（vmm_run_vcpu 返回后调 vm_free），
 *     杜绝"任务还在跑、槽位已被回收"。外部想销毁就置 stop_req 等它收拾。
 */
static vm_t g_vm_pool[MAX_VMS];
static spinlock_noirq_t g_vm_pool_lock = SPINLOCK_NOIRQ_INIT;
static uint32_t g_next_vmid = 1;

vm_t *vm_alloc(void)
{
    uint64_t flags;
    vm_t *found = NULL;
    int i;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    for (i = 0; i < MAX_VMS; i++) {
        if (g_vm_pool[i].state == VM_FREE) {
            found = &g_vm_pool[i];
            memset(found, 0, sizeof(*found));
            found->slot  = i;
            found->vmid  = g_next_vmid;
            g_next_vmid  = (g_next_vmid >= 255) ? 1 : (g_next_vmid + 1);
            found->state = VM_LOADING;
            break;
        }
    }
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);

    if (found)
        KLOG_INFO("[vmm] vm%u allocated (slot %d)\n", found->vmid, found->slot);
    else
        KLOG_WARN("[vmm] vm_alloc: no free slot (MAX_VMS=%d)\n", MAX_VMS);
    return found;
}

vm_t *vm_get(uint32_t vmid)
{
    uint64_t flags;
    vm_t *found = NULL;
    int i;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    for (i = 0; i < MAX_VMS; i++) {
        if (g_vm_pool[i].state != VM_FREE && g_vm_pool[i].vmid == vmid) {
            found = &g_vm_pool[i];
            break;
        }
    }
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);
    return found;
}

/*
 * vm_free — 由该 VM 的 vCPU 任务在退出前调用（见上面的并发规则）
 *
 * 目前只回收 stage-2（连同它按需分配的那些物理页）。vGIC / vpl011 的状态
 * 都是 vm_t 的一部分，跟着槽位一起被 vm_alloc 的 memset 清掉。
 */
void vm_free(vm_t *vm)
{
    uint64_t flags;

    if (!vm || vm->state == VM_FREE)
        return;

#if ARCH_AARCH64
    stage2_vm_destroy(&vm->s2);     /* 释放按需页 + L3 表 */
#elif ARCH_RISCV64
    rv_gstage_vm_destroy(&vm->gstage);  /* 释放按需页 + L0 表 */
#elif ARCH_X86_64
    x86_ept_vm_destroy(&vm->ept);   /* 释放按需页 + PT 表 */
#endif

    KLOG_INFO("[vmm] vm%u freed (slot %d)\n", vm->vmid, vm->slot);

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    memset(vm, 0, sizeof(*vm));
    vm->state = VM_FREE;
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);
}

int vm_count_used(void)
{
    uint64_t flags;
    int n = 0;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    for (int i = 0; i < MAX_VMS; i++)
        if (g_vm_pool[i].state != VM_FREE)
            n++;
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);
    return n;
}

void vm_request_stop(vm_t *vm)
{
    if (vm)
        vm->stop_req = 1;
}

/*
 * vmm_guest_running — 是否有**任意** VM 正在跑
 *
 * 保留这个"全局"语义是因为 /dev/vmm 的若干判断（还能不能往 guest 写、
 * bootlinux 要不要 -EBUSY）在单 VM 视角下就是这么用的。多 VM 的精细判断
 * 走 vm_get(vmid)->state。
 */
int vmm_guest_running(void)
{
    return vm_count_used() > 0;
}

/* ── VMM 主循环（架构无关）────────────────────────────────── */
/*
 * vmm_run_vcpu — vCPU 执行主循环
 *
 * 通过架构钩子抽象 eret（AArch64）/ vmlaunch+vmresume（x86）差异。
 * 返回 0：guest 正常退出；-1：未处理 exit。
 */
int vmm_run_vcpu(vcpu_t *vcpu)
{
    KLOG_INFO("[VMM] Starting vcpu%d\n", vcpu->vcpu_id);

    while (1) {
        /*
         * 宿主请求停止（/dev/vmm 的 close）。
         *
         * 在循环顶部查而不是在别处：这里正好是「上一次 guest 已经退出、
         * HCR_EL2 已被 el2_trap_exit 还原成 host 模式（TGE=1、VM=0）」
         * 的状态，直接 return 不会把 Stage-2 或 guest 向量表留在生效状态。
         */
        if (vcpu->vm->stop_req) {
            KLOG_INFO("[VMM] vcpu%d: stop requested, leaving guest loop\n",
                      vcpu->vcpu_id);
            return 0;
        }

        /*
         * 主动让出 CPU。
         *
         * 必须有这个调用：guest 退出走的是 VMM 自己的 guest_vec_table，
         * **不经过** sched_check_and_yield_from_trap() —— 那个钩子挂在宿主
         * 正常异常向量表的返回路径上。所以 vCPU 任务在循环里从不进入调度器，
         * 宿主其它任务会被完全饿死。实测症状：宿主 shell 里跑 /bin/vmm-run，
         * helper 卡在启动 guest 的那次 write 之后就再也不动了（连它自己的
         * banner 都打不出来），因为再也抢不到 cpu0。
         *
         * 位置与上面的停止检查相同：此刻上一次 guest 已退出、HCR_EL2 已被
         * el2_trap_exit 还原成 host 模式，在这里切任务是安全的。
         * sched_check_and_yield() 自己判 need_resched 与中断上下文，没有
         * 待调度任务时只是一次廉价判断。
         */
        sched_check_and_yield();


        uint64_t irq_flags = arch_irq_save();

#if ARCH_AARCH64
        /* 每轮进 guest 前重写本 VM 的 VTCR/VTTBR —— 本核可能刚跑过别的 VM
         * 的 vCPU 任务（时间片轮转），不重写就会用错页表。这也是"挂起后
         * 恢复不需要额外 stage-2 动作"的原因。*/
        stage2_activate(&vcpu->vm->s2);
#endif

        /* 恢复 guest 上下文（AArch64: EL1 sysregs；x86: no-op）*/
        vmm_arch_restore_guest_ctx(vcpu);

        /* 进入 guest（AArch64: eret; x86: vmlaunch/vmresume）*/
        int ok = vmm_arch_enter_guest(vcpu);
        if (!ok) {
            arch_irq_restore(irq_flags);
            KLOG_ERROR("[VMM] vcpu%d: guest entry failed\n", vcpu->vcpu_id);
            return -1;
        }
        vcpu->launched = 1;

        /* 保存 guest 上下文，再处理可能会阻塞/调度/打印的 VM-exit。*/
        vmm_arch_save_guest_ctx(vcpu);

        arch_irq_restore(irq_flags);

#if VMM_GUEST_LINUX_SUPPORTED
        /* 每次回到宿主都顺手收一次控制台输入（见 vmm_console_pump）*/
        vmm_console_pump(vcpu->vm);
#endif

        /* 处理 VM exit */
        int ret = vmm_arch_exit_handler(vcpu);

        switch (ret) {
        case EL2_RESUME:
            continue;
        case EL2_VMEXIT:
            KLOG_INFO("[VMM] vcpu%d: guest exited normally\n", vcpu->vcpu_id);
            return 0;
        case EL2_VMABORT:
            KLOG_WARN("[VMM] vcpu%d: guest aborted\n", vcpu->vcpu_id);
            return 0;
        case EL2_VMSKIP:
            continue;
        case EL2_EXIT:
        default:
            KLOG_ERROR("[VMM] vcpu%d: unhandled exit, stopping VMM\n",
                       vcpu->vcpu_id);
            return -1;
        }
    }
}

