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
#if ARCH_AARCH64
#include "vmm/vmm_irq_route.h"
#endif
#include "klog.h"
#include "string.h"
#include "task/task.h"
#include "task/switch.h"
#include "task/sched.h"

#if ARCH_AARCH64
#include "aarch64/stage2.h"
#include "vmm/vmm_vpl011.h"
#include "uart/uart.h"
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

    /* 初始化 Stage-2 页表（identity map）*/
    stage2_init(vm->cfg.mem_base, vm->cfg.mem_size);

    /*
     * MMIO 设备模拟：只映射 guest RAM，其余 IPA 置为无效，使设备访问
     * 陷入 EL2。移植自 kvmm mm/stage2.rs 的默认布局。
     */
    stage2_enable_mmio_trap();

    mmio_bus_init(&vm->mmio_bus_storage);

    /* 建立 MMIO 总线并注册虚拟设备（虚拟 PL011 控制台）*/
#if DRIVER_GIC_V3
    if (vmm_vgic3_init(&vm->vgic3, (uint32_t)nr) != 0)
        return -1;
#else
    if (vmm_vgic_init(&vm->vgic, (uint32_t)nr) != 0)
        return -1;
#endif

    if (vpl011_init(&vm->vpl011_dev, &vm->mmio_bus_storage) != 0) {
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
    vmm_vgic3_hw_init();
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
#endif
}

#if ARCH_AARCH64
/*
 * vmm_console_pump — 宿主控制台 → guest 虚拟 PL011 的输入桥
 *
 * 轮询宿主真实 PL011 的 RX FIFO，把用户按键推进 vpl011 的 RX FIFO
 * （对标 kvmm 的 RxChannel::push；注入中断由 VMM 在进入 guest 前做）。
 *
 * 为什么轮询而不是让宿主收 RX 中断：guest 运行期间宿主中断是关的，
 * 自己的 RX 中断根本进不来。而退出路径的调用频率足够高 ——
 *   - guest 空闲：每条 WFI 都陷入 EL2，实测 ~3 万次/秒；
 *   - guest 满载：宿主定时器每 10ms 也会把它踹回宿主一次。
 * 对交互式控制台来说，最坏 10ms 的输入延迟完全够用。
 *
 * uart_rx_ready() 在 UART 未开中断时直接查硬件 FR.RXFE 位，非阻塞，
 * 所以这里必须先用它把关 —— uart_getc() 在没有数据时是阻塞的。
 *
 * ⚠️ 只在 vpl011 的 TX 通道**关闭**时才能跑。helper 模式（/bin/vmm-run）
 * 下宿主 tty 层独占真实 UART：用户按键由 helper 从自己的 stdin 读走，
 * 再 write() 到 /dev/vmm。此时若本函数也在跑，两个消费者会从同一个硬件
 * FIFO 抢字节（tty.c 的 signal_check_uart() 是第一个，本函数是第二个），
 * 谁先跑谁拿到，输入会随机丢给错误的一方。
 */
static void vmm_console_pump(void)
{
    if (vpl011_tx_channel_enabled())
        return;

    while (uart_rx_ready())
        vpl011_push_rx((uint8_t)uart_getc());
}
#endif /* ARCH_AARCH64 */

/* ── 宿主侧 guest 生命周期 ─────────────────────────────────── */
/*
 * 两个标志都由「谁创建/结束 vCPU 任务」维护，不从 guest 侧访问：
 *   running  — vcpu_task_create() 置位，vcpu_task_fn() 退出前清零。
 *              /dev/vmm 用它判断「还能不能写 guest 输入」以及 bootlinux
 *              要不要返回 -EBUSY。
 *   stop     — /dev/vmm 的 close 置位；主循环每轮检查，见到就返回。
 */
static volatile int g_vmm_guest_running;
static volatile int g_vmm_stop_requested;

void vmm_request_stop(void)
{
    g_vmm_stop_requested = 1;
}

int vmm_guest_running(void)
{
    return g_vmm_guest_running;
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
        if (g_vmm_stop_requested) {
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
        stage2_activate();
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

#if ARCH_AARCH64
        /* 每次回到宿主都顺手收一次控制台输入（见 vmm_console_pump）*/
        vmm_console_pump();
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

/* ── vCPU 任务入口（内核任务函数）────────────────────────── */
static void vcpu_task_fn(void *arg)
{
    vcpu_t *vcpu = (vcpu_t *)arg;

    KLOG_INFO("[vmm] vcpu%d task started\n", vcpu->vcpu_id);

    int rc = vmm_run_vcpu(vcpu);

    /* 先清 running：/dev/vmm 的 bootlinux 靠它判断旧 guest 是否已收尾 */
    g_vmm_guest_running = 0;

    if (rc == 0)
        KLOG_INFO("[vmm] vcpu%d exited normally\n", vcpu->vcpu_id);
    else
        KLOG_ERROR("[vmm] vcpu%d exited with error %d\n", vcpu->vcpu_id, rc);

    /*
     * 摘掉「本 pCPU 的 vCPU 承载任务」登记。
     *
     * GICv3 下宿主 vtimer 不由 ISR 注入（见 irq_route.c 的说明），所以这纯
     * 属清账；但 GICv2 路径会用它，而这张表是按任务指针匹配的 —— 任务退出
     * 后指针会被复用，留着旧值就可能让 ISR 去 unblock / 注入一个已经不是
     * vCPU 的任务。
     *
     * irq_route.c 只在 aarch64 的 vdev 列表里，其它架构没有这个符号。
     */
#if ARCH_AARCH64
    vmm_irq_route_clear_owner();
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

        /*
         * 从这里起就算「guest 在跑」：/dev/vmm 的 write/poll 会立刻看到，
         * 不必等任务真正被调度上 CPU。同时清掉上一轮可能残留的停止请求，
         * 否则重启 guest 时主循环第一轮就会直接退出。
         */
        g_vmm_stop_requested = 0;
        g_vmm_guest_running  = 1;

        KLOG_INFO("[vmm] vcpu%d task created (id=%u) pinned to cpu0\n",
                  vcpu->vcpu_id, t->id);
    } else {
        KLOG_ERROR("[vmm] failed to create vcpu%d task\n", vcpu->vcpu_id);
    }
    return t;
}
