/*
 * include/vmm_irq_route.h — 宿主 IRQ 路由：唤醒 vCPU 任务以投递 guest 中断
 *
 * 移植自 x-kernel: virt/kvmm/src/vdev/aarch64/irq_route.rs
 *
 * 场景：guest 的虚拟定时器（PPI 27）到期时，**真实宿主**的 PPI 27 也会
 * 触发。宿主 ISR 需要找到「当前 pCPU 上承载 vCPU 的任务」并唤醒它，
 * 由该任务在进入 guest 前把中断注入（经 vGIC LR）。
 *
 * 换言之：guest 的 vtimer 中断投递依赖这条「宿主硬件中断 → vCPU 任务」
 * 的唤醒链路；没有它，guest 只有在因别的原因退出时才会看到定时器中断。
 *
 * 与 kvmm 的差异：kvmm 用 kirq 的 irq desc / virq 映射框架；avatar 用
 * 简单的 `irq_install()` + `task_unblock()`，故此处只保留核心语义：
 *   - 每 pCPU 记录「拥有 vCPU 的任务」
 *   - 注册宿主 PPI 27 处理程序，触发时唤醒该任务
 *   - 已注册标志供 vGIC 决定是否走 LR HW 模式
 */
#ifndef VMM_IRQ_ROUTE_H
#define VMM_IRQ_ROUTE_H

#include "types.h"

struct vgic;
typedef struct vgic vgic_t;

/* guest 虚拟定时器 PPI（与 vgic.c / el2_run.c 一致）*/
#define HOST_VTIMER_IRQ  27

/*
 * vmm_irq_route_publish_owner — 记录当前任务为当前 pCPU 的 vCPU 承载者
 *
 * 在进入 guest 前（世界切换入口窗口）调用。
 */
void vmm_irq_route_publish_owner(void);

/*
 * vmm_irq_route_publish_vcpu — 记录当前 pCPU 上承载的 vCPU id
 *
 * 与 publish_owner() 一并调用；宿主 PPI 27 ISR 需要它来确定把虚拟定时器
 * 中断注入给哪个 vCPU。
 */
void vmm_irq_route_publish_vcpu(uint32_t vcpu_id);
void vmm_irq_route_publish_vgic(vgic_t *vgic);

/*
 * vmm_irq_route_clear_owner — 清除当前任务在所有 pCPU 上的承载者记录
 *
 * vCPU 任务退出时调用。
 */
void vmm_irq_route_clear_owner(void);

/*
 * vmm_irq_route_set_vtimer_enabled — 注册并使能/禁用宿主 vtimer 中断路由
 *
 * guest 在 GICD 中使能/禁用 PPI 27 时调用（见 vgicd.c）。
 * 首次使能时注册宿主 PPI 27 处理程序。
 */
void vmm_irq_route_set_vtimer_enabled(int enabled);

/*
 * vmm_irq_route_host_hwirq_for_guest_irq — 若 guest 中断走 LR HW 模式，
 * 返回其对应的宿主 INTID；否则返回 0。
 *
 * 对标 vgic.rs 的 host_hwirq_for_guest_irq()：只有已注册的宿主后端路由
 * （当前即 vtimer）才需要 LR 的 HW 位。
 */
uint32_t vmm_irq_route_host_hwirq_for_guest_irq(uint32_t guest_irq);

#endif /* VMM_IRQ_ROUTE_H */
