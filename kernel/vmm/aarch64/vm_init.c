/*
 * kernel/vmm/aarch64/vm_init.c — AArch64 的 VM 初始化
 *
 * 从 vmm.c 搬过来的：那是「架构无关」的文件，却装着本架构 124 行的完整初始化
 * （stage-2、MMIO 总线、vGIC、vPL011、vCPU 数组），而它的两个对等物
 * （x86 的 vmm_arch_vm_init / riscv 的 vmm_arch_vm_init）一直就在各自架构目录里。
 *
 * 由 vm.c 的 vm_create() 调用（Step 4b 之后统一成 vmm_arch_vm_init 钩子）。
 * 本目录已被 Makefile 的 _KERNEL_ARCH_MODULES 对非 aarch64 整目录 filter-out，
 * 所以外面那层 `#if ARCH_AARCH64` 是冗余的、已去掉。
 */

#include "vmm/vmm.h"
#include "vmm/vmm_mmio.h"
#include "vmm/vmm_console.h"
#include "klog.h"
#include "string.h"
#include "aarch64/stage2.h"
#include "vmm/vmm_vpl011.h"
#if DRIVER_GIC_V3
#include "vmm/vmm_vgicv3.h"
#else
#include "vmm/vmm_vgicd.h"
#include "vmm/vmm_vgic.h"
#include "vmm/vmm_vgicc.h"
#endif

/* ── AArch64 VM 初始化（vmm_arch_vm_init 钩子）─────────────── */
int vmm_arch_vm_init(vm_t *vm)
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
