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

/* Keep declarations local to avoid dragging VMM internals into the IRQ layer. */
extern void vmm_vgic_inject_timer(uint32_t vcpu_id);

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
     * 关键：在这里**直接**把虚拟中断挂进 vGIC LR。
     *
     * 这个宿主中断是在 guest 运行中到达的：EL2 的异常处理打完就直接返回
     * guest，不会经过 vCPU 主循环的 vmm_arch_restore_guest_ctx()。所以
     * 「只唤醒任务」对当前不睡眠的 vCPU 任务毫无作用：只要 guest 一直不
     * trap（例如卡在 calibrate_delay() 里 `while (ticks == jiffies)
     * cpu_relax();` 等 tick），它就永远等不到注入 —— 死锁。
     *
     * ISR 返回后 guest 立刻通过 GICH LR 看到虚拟 IRQ，随后访问软件
     * GICC_IAR/EOIR 完成 ack/EOI。
     */
    if (cpu < MAX_ROUTE_CPUS) {
        vmm_vgic_inject_timer(g_owner_vcpu[cpu]);
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

        /* Keep the host vtimer PPI in the existing guest-owned IRQ flow.
         * The software vGIC consumes it only as a trigger source, but the
         * low-level IRQ path still relies on this mark for the timer PPI. */
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
     * 软件 vGIC 不使用 HW=1 list register。宿主 PPI 27 只作为「该给
     * guest 送 tick 了」的触发源，虚拟 PPI 27 的生命周期完全在 vgic core
     * 的 pending/active 位图里维护。
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
