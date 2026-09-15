/*
 * kernel/vmm/vdev/vgic_irq_route.c — 宿主 IRQ → vCPU 任务唤醒路由
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

/* 路由状态（对标 Rust 的 ROUTE_UNUSED/REGISTERING/REGISTERED）*/
#define ROUTE_UNUSED       0
#define ROUTE_REGISTERING  1
#define ROUTE_REGISTERED   2

#define MAX_ROUTE_CPUS  8

/* 每个 pCPU 上承载 vCPU 的任务 */
static task_t *g_owner_tasks[MAX_ROUTE_CPUS];

static volatile int g_route_state = ROUTE_UNUSED;

/* ── 宿主中断处理：唤醒承载 vCPU 的任务 ───────────────────── */
static void host_vtimer_irq_handler(uint64_t *frame)
{
    (void)frame;

    uint32_t cpu = get_current_cpu_id();
    task_t  *owner = (cpu < MAX_ROUTE_CPUS) ? g_owner_tasks[cpu] : NULL;

    /*
     * 唤醒该任务，让它在世界切换入口重新检查 vtimer 并注入 guest。
     * task_unblock 只把任务置回就绪态；真正的注入发生在它被调度、
     * 执行 vmm_arch_restore_guest_ctx() 时（那时会调 check_vtimer）。
     */
    if (owner)
        task_unblock(owner);
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
    if (guest_irq == HOST_VTIMER_IRQ && g_route_state == ROUTE_REGISTERED)
        return HOST_VTIMER_IRQ;
    return 0;
}

/* ── 承载者记录 ───────────────────────────────────────────── */
void vmm_irq_route_publish_owner(void)
{
    uint32_t cpu = get_current_cpu_id();

    if (cpu < MAX_ROUTE_CPUS)
        g_owner_tasks[cpu] = task_current();
}

void vmm_irq_route_clear_owner(void)
{
    task_t *cur = task_current();

    for (int i = 0; i < MAX_ROUTE_CPUS; i++) {
        if (g_owner_tasks[i] == cur)
            g_owner_tasks[i] = NULL;
    }
}
