/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/irq/irq.c — 中断控制器核心

   只做三件事：
     1. 保存控制器后端(core 由 IRQ 类驱动 probe 时 irq_bind)；
     2. 维护「设备中断号 → (handler, ctx)」表，号空间由后端定义；
     3. 把内核/驱动调用转发给后端。

   中断上下文不加锁：注册只发生在相应设备 probe 阶段（开中断之前），
   之后这张表是只读的。可见性用 wmb()/rmb() 保证。
*/

#include "driver/irq.h"
#include "klog.h"
#include "barrier.h"

const irq_ops_t *g_irq_ops;

typedef struct {
    irq_handler_fn fn;
    void *ctx;
} irq_slot_t;

static irq_slot_t g_irq_slots[IRQ_MAX_VECTORS];

void irq_bind(const irq_ops_t *ops)
{
    if (ops == NULL)
        return;

    wmb();
    g_irq_ops = ops;
    wmb();
}

int irq_register(unsigned irq, irq_handler_fn fn, void *ctx)
{
    if (g_irq_ops == NULL)
        return -1;
    if (irq >= IRQ_MAX_VECTORS || fn == NULL)
        return -2;

    g_irq_slots[irq].ctx = ctx;
    wmb();
    g_irq_slots[irq].fn = fn;
    wmb();
    return 0;
}

int irq_dispatch(unsigned irq, void *frame)
{
    if (irq >= IRQ_MAX_VECTORS)
        return -1;

    irq_handler_fn fn = g_irq_slots[irq].fn;
    if (fn == NULL) {
        /*
           中断风暴时每个中断一行会堵死串口；KLOG_WARN_SAMPLE 自带限流。
           这里在 ISR 上下文，不能打未采样的日志。
        */
        KLOG_WARN_SAMPLE("[irq] unhandled irq=%u\n", irq);
        return -1;
    }

    fn(frame, g_irq_slots[irq].ctx);
    return 0;
}

void irq_dispatch_frame(void *frame)
{
    if (g_irq_ops != NULL && g_irq_ops->dispatch != NULL)
        g_irq_ops->dispatch(frame);
}

int irq_enable(unsigned irq, bool on)
{
    if (g_irq_ops == NULL || g_irq_ops->enable == NULL)
        return -1;
    return g_irq_ops->enable(irq, on);
}

int irq_set_priority(unsigned irq, unsigned prio)
{
    if (g_irq_ops == NULL || g_irq_ops->set_priority == NULL)
        return -1;
    return g_irq_ops->set_priority(irq, prio);
}

int irq_secondary_init(void)
{
    if (g_irq_ops == NULL || g_irq_ops->secondary_init == NULL)
        return 0; /* 该控制器不需要 per-CPU 初始化 */
    return g_irq_ops->secondary_init();
}

int irq_send_ipi(uint32_t hw_id)
{
    if (g_irq_ops == NULL || g_irq_ops->send_ipi == NULL)
        return -1;
    return g_irq_ops->send_ipi(hw_id);
}

int irq_register_ipi(irq_handler_fn fn, void *ctx)
{
    if (g_irq_ops == NULL || g_irq_ops->register_ipi == NULL)
        return -1;
    return g_irq_ops->register_ipi(fn, ctx);
}

int irq_register_timer(irq_handler_fn fn, void *ctx)
{
    if (g_irq_ops == NULL || g_irq_ops->register_timer == NULL)
        return -1;
    return g_irq_ops->register_timer(fn, ctx);
}

int irq_timer_enable(void)
{
    if (g_irq_ops == NULL || g_irq_ops->timer_enable == NULL)
        return 0;
    return g_irq_ops->timer_enable();
}

uint64_t irq_cpu_hw_id(void)
{
    if (g_irq_ops != NULL && g_irq_ops->cpu_hw_id != NULL)
        return g_irq_ops->cpu_hw_id();
    return 0;
}

void irq_debug_dump(void)
{
    if (g_irq_ops != NULL && g_irq_ops->debug_dump != NULL)
        g_irq_ops->debug_dump();
}

bool irq_virt_supported(void)
{
    return g_irq_ops != NULL && g_irq_ops->virt_supported;
}
