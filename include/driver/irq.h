/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/irq.h — 中断控制器公共接口

   驱动/内核只依赖这里的接口, 不再包含 irq/gicv3.h、irq/gicv2.h、
   irq/plic.h、irq/lapic.h 等具体控制器头文件。

   分工：
     - 控制器后端(GICv2/GICv3/PLIC/LAPIC)在 probe 时 irq_bind(&ops)；
     - 设备驱动用 irq_register()/irq_enable()/irq_set_priority() 注册自己的
       中断线，号空间就是硬件号(GIC INTID / PLIC source / IDT vector)；
     - 架构异常入口只调用 irq_dispatch_frame(frame)，由后端完成
       ack → 查表 → handler → eoi/complete 的全过程。

   处理器签名统一为 (frame, ctx)：frame 是架构 trap frame，设备驱动一般
   忽略；ctx 是注册时传入的私有指针，免去每根中断线写一个薄包装。
*/

#ifndef DRIVER_IRQ_H
#define DRIVER_IRQ_H

#include "types.h"

/* GIC INTID 号空间上限(1024)；PLIC/IDT 都远小于它 */
#define IRQ_MAX_VECTORS 1024

typedef struct {
    uintptr_t gicd;
    uintptr_t gicr;
    uintptr_t gicc;
    uintptr_t gich;
    uintptr_t plic;
    uintptr_t clint;
    uintptr_t ioapic; /* x86 IOAPIC MMIO 基址；0 = 未配置 */
    bool virt;        /* 是否提供虚拟中断接口（GICv3） */
} irq_cfg_t;

typedef void (*irq_handler_fn)(void *frame, void *ctx);

/*
   控制器后端操作表。除 dispatch 外都可为 NULL：
     dispatch           必选。架构异常入口的唯一回调，负责完整的一次
                        中断应答/分发/结束。
     enable             设备中断线开关。
     set_priority       设备中断线优先级。
     secondary_init     次级核 per-CPU 初始化。
     send_ipi           向硬件 ID(hw_id) 发 resched IPI。
     register_ipi       注册 resched IPI 的处理器（号由后端决定）。
     register_timer     注册本地定时器中断（号由后端决定）。
     timer_enable       在次级核上打开本地定时器中断。
     cpu_hw_id          读取当前核的硬件 ID(MPIDR/hartid/LAPIC ID)。
     debug_dump         可选的控制器状态 dump（诊断用）。
*/
typedef struct {
    void (*dispatch)(void *frame);
    int (*enable)(unsigned irq, bool on);
    int (*set_priority)(unsigned irq, unsigned prio);
    int (*secondary_init)(void);
    int (*send_ipi)(uint32_t hw_id);
    int (*register_ipi)(irq_handler_fn fn, void *ctx);
    int (*register_timer)(irq_handler_fn fn, void *ctx);
    int (*timer_enable)(void);
    uint64_t (*cpu_hw_id)(void);
    void (*debug_dump)(void);
    bool virt_supported;
} irq_ops_t;

/* 当前绑定的后端（由 irq.c 定义）。 */
extern const irq_ops_t *g_irq_ops;

/* 后端绑定（probe 时调用）。重复绑定无副作用。 */
void irq_bind(const irq_ops_t *ops);

/* 是否有中断控制器后端；未绑定时所有接口返回 -1。 */
static inline bool irq_available(void)
{
    return g_irq_ops != NULL;
}

/*
   irq_register - 注册一条设备中断线
   返回 0 成功；-1 无控制器；-2 号越界。
   中断上下文不会并发写这张表（注册发生在开中断之前），读侧无锁。
*/
int irq_register(unsigned irq, irq_handler_fn fn, void *ctx);

/* 核心分发表：供后端 dispatch 调用；返回 0 已分发，-1 无处理器。 */
int irq_dispatch(unsigned irq, void *frame);

/* 架构异常入口：交给控制器后端完成 ack→handler→eoi。 */
void irq_dispatch_frame(void *frame);

int irq_enable(unsigned irq, bool on);
int irq_set_priority(unsigned irq, unsigned prio);
int irq_secondary_init(void);
int irq_send_ipi(uint32_t hw_id);
int irq_register_ipi(irq_handler_fn fn, void *ctx);
int irq_register_timer(irq_handler_fn fn, void *ctx);
int irq_timer_enable(void);
uint64_t irq_cpu_hw_id(void);
void irq_debug_dump(void);

/* 虚拟中断接口能力（VMM 在运行期查询，不再用 #if DRIVER_GIC_V3）。 */
bool irq_virt_supported(void);

#endif /* DRIVER_IRQ_H */
