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
#include "klog.h"
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
/*
 * 由 vm.c 的 vm_create() 调用，所以不能是 static。
 * 本函数下一步会搬到 kernel/vmm/aarch64/vm_init.c（那时目录本身就是守卫）。
 */
int aarch64_vm_init(vm_t *vm)
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

