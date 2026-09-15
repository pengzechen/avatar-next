/*
 * kernel/vmm/vmm.c — VM 管理与 vCPU 执行主循环
 *
 * 架构无关层：vmm_run_vcpu 通过架构钩子驱动 guest 执行。
 * 各架构在 arch/el2_run.c 或 arch/vmx.c 中提供钩子实现：
 *   vmm_arch_restore_guest_ctx / vmm_arch_enter_guest
 *   vmm_arch_exit_handler       / vmm_arch_save_guest_ctx
 */

#include "vmm.h"
#include "vmm_mmio.h"
#include "klog.h"
#include "string.h"
#include "task/task.h"

#if ARCH_AARCH64
#include "aarch64/stage2.h"
#include "vmm_vpl011.h"
#include "vmm_vgicd.h"
#include "vmm_vgic.h"

/* ── 全局 MMIO 总线与虚拟设备实例（静态存储，单 VM）────────── */
static mmio_bus_t    g_mmio_bus;
static mmio_device_t g_vpl011_dev;
static mmio_device_t g_vgicd_dev;

/* ── AArch64 VM 初始化 ────────────────────────────────────── */
static int aarch64_vm_init(vm_t *vm)
{
    int i;
    int nr = vm->cfg.nr_vcpus;

    if (nr < 1 || nr > MAX_VCPUS) {
        KLOG_ERROR("[vmm] vm_create: invalid nr_vcpus=%d\n", nr);
        return -1;
    }

    /* 初始化 Stage-2 页表（identity map）*/
    stage2_init(vm->cfg.mem_base, vm->cfg.mem_size);

    /*
     * MMIO 设备模拟：只映射 guest RAM，其余 IPA 置为无效，使设备访问
     * 陷入 EL2。移植自 kvmm mm/stage2.rs 的默认布局。
     */
    stage2_enable_mmio_trap();

    /* 建立 MMIO 总线并注册虚拟设备（虚拟 PL011 控制台）*/
    mmio_bus_init(&g_mmio_bus);
    if (vpl011_init(&g_vpl011_dev, &g_mmio_bus) != 0) {
        KLOG_WARN("[vmm] vpl011 registration failed\n");
    }
    /* 虚拟 GICv2：分两半
     *   vgicd — 分发器（guest MMIO 访问落到这里）
     *   vgic  — 注入核心（维护挂起/使能位图 + GICH 列表寄存器）*/
    if (vgicd_init(&g_vgicd_dev, &g_mmio_bus, (uint32_t)nr) != 0) {
        KLOG_WARN("[vmm] vgicd registration failed\n");
    }
    vmm_vgic_init((uint32_t)nr);

    /*
     * GICH 映射：vGIC 把挂起中断整理进列表寄存器（LR）后，需要写入真实的
     * GICH 寄存器才能让 guest 收到中断。地址来自平台配置（platform.conf 的
     * `gich` 项，QEMU virt = 0x08030000），由 platform_get_mmio 自动加上
     * KERNEL_VMA，**不是猜测的地址**。
     *
     * 若平台未提供该项（返回 0）则保持「软件侧模式」：vGIC 只维护位图与
     * 影子 LR，不写硬件——避免写入非法地址。
     */
    {
        extern uintptr_t platform_get_mmio(const char *block, const char *key);
        uintptr_t gich = platform_get_mmio("irq", "gich");
        vmm_vgic_set_gich_base(gich);
    }
    vm->mmio_bus = &g_mmio_bus;

    KLOG_INFO("[vmm] MMIO bus ready: PL011 @0x%llx, GICD @0x%llx\n",
              (unsigned long long)VPL011_BASE,
              (unsigned long long)VGICD_BASE);

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

#if ARCH_X86_64
/* x86_64 VM 初始化在 vmx.c 中完成（vmx_vm_init），此处调用 */
extern int vmx_vm_init(vm_t *vm);
#endif

#if ARCH_RISCV64
/* RISC-V H-extension VM 初始化在 hext_run.c 中完成 */
extern int hext_vm_init(vm_t *vm);
#endif

/* ── 公开 API ─────────────────────────────────────────────── */

int vm_create(vm_t *vm)
{
#if ARCH_AARCH64
    return aarch64_vm_init(vm);
#elif ARCH_X86_64
    return vmx_vm_init(vm);
#elif ARCH_RISCV64
    return hext_vm_init(vm);
#else
    (void)vm;
    KLOG_WARN("[vmm] vm_create: VMM not supported on this arch\n");
    return -1;
#endif
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

    /* 恢复 guest 上下文（AArch64: EL1 sysregs；x86: no-op）*/
    vmm_arch_restore_guest_ctx(vcpu);

    while (1) {
        /* 进入 guest（AArch64: eret; x86: vmlaunch/vmresume）*/
        int ok = vmm_arch_enter_guest(vcpu);
        if (!ok) {
            KLOG_ERROR("[VMM] vcpu%d: guest entry failed\n", vcpu->vcpu_id);
            return -1;
        }
        vcpu->launched = 1;

        /* 处理 VM exit */
        int ret = vmm_arch_exit_handler(vcpu);

        switch (ret) {
        case EL2_RESUME:
            continue;
        case EL2_VMEXIT:
            KLOG_INFO("[VMM] vcpu%d: guest exited normally\n", vcpu->vcpu_id);
            vmm_arch_save_guest_ctx(vcpu);
            return 0;
        case EL2_VMABORT:
            KLOG_WARN("[VMM] vcpu%d: guest aborted\n", vcpu->vcpu_id);
            vmm_arch_save_guest_ctx(vcpu);
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

/* ── vCPU 任务入口（内核任务函数）────────────────────────── */
static void vcpu_task_fn(void *arg)
{
    vcpu_t *vcpu = (vcpu_t *)arg;

    KLOG_INFO("[vmm] vcpu%d task started\n", vcpu->vcpu_id);

#if ARCH_AARCH64 || ARCH_X86_64 || ARCH_RISCV64
    int rc = vmm_run_vcpu(vcpu);
    if (rc == 0)
        KLOG_INFO("[vmm] vcpu%d exited normally\n", vcpu->vcpu_id);
    else
        KLOG_ERROR("[vmm] vcpu%d exited with error %d\n", vcpu->vcpu_id, rc);
#else
    KLOG_WARN("[vmm] VMM not supported on this arch\n");
#endif

    task_exit();
}

struct task *vcpu_task_create(vcpu_t *vcpu, uint8_t priority)
{
    char name[TASK_NAME_LEN];
    name[0] = 'v'; name[1] = 'c'; name[2] = 'p'; name[3] = 'u';
    name[4] = '0' + (char)(vcpu->vcpu_id & 0xF);
    name[5] = '\0';

    struct task *t = task_create(name, vcpu_task_fn, vcpu, priority);
    if (t) {
        /* vcpu 状态（VMCS / VHE 寄存器 / SBI HSM 等）尚未支持跨核迁移。
         * 暂时全部钉到 BSP，待后续实现 vcpu 跨核迁移再放开。 */
        task_set_cpu_affinity(t, 0);
        KLOG_INFO("[vmm] vcpu%d task created (id=%u) pinned to cpu0\n",
                  vcpu->vcpu_id, t->id);
    } else {
        KLOG_ERROR("[vmm] failed to create vcpu%d task\n", vcpu->vcpu_id);
    }
    return t;
}
