/*
 * kernel/vmm/aarch64/virq.c — AArch64 的「拉线」适配器
 *
 * 把一根 virq_t（号码是 GIC INTID）接到 vGIC 后端。见 include/vmm/vmm_virq.h
 * 说明为什么只统一「线」而不统一「控制器」。
 *
 * 单独一个文件而不是塞进 el2_run.c：那个文件目前不 include vgic 的两个头，
 * 塞进去会为了一行转发新增两条 include（宏冲突的经典入口）。本目录已被
 * Makefile 的 _KERNEL_ARCH_MODULES 自动收编，不需要改构建。
 */

#include "vmm/vmm.h"
#include "vmm/vmm_vgic.h"
#if DRIVER_GIC_V3
#include "vmm/vmm_vgicv3.h"
#endif

void vmm_arch_irq_raise(vcpu_t *vcpu, virq_t irq)
{
    vm_t *vm;

    if (!vcpu)
        return;
    vm = vcpu->vm;
    if (!vm)
        return;

    /*
     * 这里**不做任何使能判断**：guest 有没有 enable 这条线由 vGIC 在投递时
     * （LR 装载 / pending & enabled & !active）判定。在 raise 时刻先判一次会
     * 改变可观测行为 —— 见 vmm_virq.h。
     */
#if DRIVER_GIC_V3
    vmm_vgic3_set_pending(&vm->vgic3, (uint32_t)vcpu->vcpu_id, irq.line);
#else
    vmm_vgic_set_pending(&vm->vgic, (uint32_t)vcpu->vcpu_id, irq.line);
#endif
}
