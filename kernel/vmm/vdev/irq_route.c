/*
 * kernel/vmm/vdev/irq_route.c — 宿主 IRQ → vCPU 任务唤醒路由
 *
 * 移植自 x-kernel: virt/kvmm/src/vdev/aarch64/irq_route.rs，适配 Avatar OS：
 *   - kirq desc/virq 框架 → avatar 的 irq_install() + task_unblock()
 *   - ktask::interrupt_task() → task_unblock()
 *   - SpinNoIrq → 简单静态数组（IRQ 上下文访问，单核路径）
 */

#include "vmm_irq_route.h"
#include "klog.h"
#include "task/task.h"
#include "task/cpu.h"
#include "exception.h"
#include "irq/irq.h"      /* irq_install / irq_enable_irq / irq_disable_irq */

/*
 * vgic 与 vmm 的入口在此按需声明：vmm_vgic.h / vmm.h 里的 GICH_* 宏与
 * gicv2.h（经 irq/irq.h 引入）重名，整头文件包含会触发重定义告警。
 */
extern void vmm_vgic_inject_timer(uint32_t vcpu_id);
extern void vmm_arch_raise_vi(void);

/* 路由状态（对标 Rust 的 ROUTE_UNUSED/REGISTERING/REGISTERED）*/
#define ROUTE_UNUSED       0
#define ROUTE_REGISTERING  1
#define ROUTE_REGISTERED   2

#define MAX_ROUTE_CPUS  8

/* 每个 pCPU 上承载 vCPU 的任务 */
static task_t *g_owner_tasks[MAX_ROUTE_CPUS];

/* 每个 pCPU 上承载的 vCPU id（宿主 ISR 直接注入用）*/
static uint32_t g_owner_vcpu[MAX_ROUTE_CPUS];

static volatile int g_route_state = ROUTE_UNUSED;

/* ── 宿主中断处理：注入 guest 虚拟定时器中断 ───────────────── */
static void host_vtimer_irq_handler(uint64_t *frame)
{
    (void)frame;

    uint32_t cpu = get_current_cpu_id();
    task_t  *owner = (cpu < MAX_ROUTE_CPUS) ? g_owner_tasks[cpu] : NULL;

    /* 任务若是睡着的（将来支持 WFI 真睡眠时）需要唤醒 */
    if (owner)
        task_unblock(owner);

    /*
     * 关键：在这里**直接**把虚拟中断挂进 vGIC 并同步到 GICH LR。
     *
     * 这个宿主中断是在 guest 运行中到达的：EL2 的异常处理打完就直接返回
     * guest，不会经过 vCPU 主循环的 vmm_arch_restore_guest_ctx()。所以
     * 「只唤醒任务」对当前不睡眠的 vCPU 任务毫无作用：只要 guest 一直不
     * trap（例如卡在 calibrate_delay() 里 `while (ticks == jiffies)
     * cpu_relax();` 等 tick），它就永远等不到注入 —— 死锁。
     *
     * ISR 返回后 guest 立刻看到 GICV 上的虚拟中断，自行 ack/EOI；HW=1 的
     * LR 让 guest 的 EOI 直接 deactivate 物理 PPI 27（因此宿主 ISR 里不能
     * 写 GICC_DIR，见 boot/aarch64/exception.c）。
     */
    if (cpu < MAX_ROUTE_CPUS) {
        vmm_vgic_inject_timer(g_owner_vcpu[cpu]);
        /* guest 正在 EL1 跑，立刻把 vIRQ 拉起来（不必等它 trap 回 EL2）*/
        vmm_arch_raise_vi();
    }
}

/* ── 注册/使能路由 ────────────────────────────────────────── */
void vmm_irq_route_set_vtimer_enabled(int enabled)
{
    if (enabled) {
        if (g_route_state == ROUTE_REGISTERED)
            return;
        if (g_route_state != ROUTE_UNUSED)
            return;                 /* 正在注册中 */

        g_route_state = ROUTE_REGISTERING;

        irq_install(HOST_VTIMER_IRQ, host_vtimer_irq_handler);
        irq_enable_irq(HOST_VTIMER_IRQ);

        /*
         * PPI 27 由 guest 的虚拟 EOI 负责 deactivate（vGIC 的 HW=1 list
         * register 把物理 27 映射到 guest 的虚拟 27）。宿主侧处理它时只能
         * 做优先级下降（EOIR），**绝不能写 GICC_DIR**，否则物理中断在 guest
         * 还没收到之前就被 deactivate 了，虚拟中断也就投不进 guest。
         * 见 boot/aarch64/exception.c: handle_irq_exception()。
         */
        irq_mark_guest_owned(HOST_VTIMER_IRQ);

        g_route_state = ROUTE_REGISTERED;
        KLOG_INFO("[irq_route] host vtimer IRQ %u registered\n",
                  (unsigned)HOST_VTIMER_IRQ);
        return;
    }

    /* 禁用 */
    if (g_route_state != ROUTE_REGISTERED)
        return;

    irq_disable_irq(HOST_VTIMER_IRQ);
}

uint32_t vmm_irq_route_host_hwirq_for_guest_irq(uint32_t guest_irq)
{
    /*
     * 恒返回 0：vGIC 不使用 HW=1 的 list register。
     *
     * HW=1 要求物理中断与虚拟中断严格配对（guest 的虚拟 EOI 去 deactivate
     * 物理中断），但宿主中断入口是按「处理完就 EOI+DIR」的通用流程走的，
     * 而 PPI 27 又被标记为 guest-owned（宿主不能写 DIR，见 exception.c），
     * 于是物理 27 会长期停在 active：HW=1 的 LR 在这期间不向 guest 投递，
     * 而 guest 收不到虚拟中断就无法用 EOI 去 deactivate 它 —— 互锁。
     *
     * 纯虚拟 LR 只由 LR 自身状态决定投递，与物理中断的 active 状态解耦；
     * LR 的生命周期由 vmm_vgic_sync_exit() 在每个退出点上完全接管，
     * 下次 entry 依据 pending 位图重建。宿主 PPI 27 只作为「该给 guest
     * 送 tick 了」的触发源（ISR 里直接注入）。
     */
    (void)guest_irq;
    return 0;
}

/* ── 承载者记录 ───────────────────────────────────────────── */
void vmm_irq_route_publish_owner(void)
{
    uint32_t cpu = get_current_cpu_id();

    if (cpu < MAX_ROUTE_CPUS)
        g_owner_tasks[cpu] = task_current();
}

void vmm_irq_route_publish_vcpu(uint32_t vcpu_id)
{
    uint32_t cpu = get_current_cpu_id();

    if (cpu < MAX_ROUTE_CPUS)
        g_owner_vcpu[cpu] = vcpu_id;
}

void vmm_irq_route_clear_owner(void)
{
    task_t *cur = task_current();

    for (int i = 0; i < MAX_ROUTE_CPUS; i++) {
        if (g_owner_tasks[i] == cur)
            g_owner_tasks[i] = NULL;
    }
}
