/*
 * driver/irq.h  —  统一中断控制器接口（aarch64 only）
 *
 * 根据 DRIVER_GIC_VERSION（默认 2）选择 GICv2 或 GICv3。
 * 通过 make 传 CFLAGS+=-DDRIVER_GIC_VERSION=3 切换到 GICv3。
 *
 * 统一 API：
 *   irq_init()              初始化 GIC（GICD + GICC/Redistributor）
 *   irq_enable_irq(n)       使能 IRQ n
 *   irq_disable_irq(n)      禁用 IRQ n
 *   irq_ack()               应答中断，返回 IRQ 号（读 IAR）
 *   irq_eoi(irq)            End-of-Interrupt（写 EOIR）
 *   中断开关不在本文件：统一用 include/aarch64/exception_impl.h 的
 *   arch_irq_enable() / arch_irq_disable()（见 docs/basic/INTERRUPT_MASKING.md）。
 */

#ifndef DRIVER_IRQ_H
#define DRIVER_IRQ_H

#include "platform_cfg.h"
#include "types.h"

/* ============================================================
 * GICv2
 * ============================================================ */
#if DRIVER_GIC_V2

    #include "irq/gicv2.h"

    #define irq_init()              gic_virtual_init()
    #define irq_init_secondary()    gic_init_secondary()
    #define irq_enable_irq(n)       gic_enable_int((int32_t)(n), 1)
    #define irq_disable_irq(n)      gic_enable_int((int32_t)(n), 0)

    /* GICv2：通过 MMIO 读 GICC_IAR / 写 GICC_EOIR */
    static inline uint32_t irq_ack(void)
    {
        uint32_t iar = gic_read_iar();
        return gic_iar_irqnr(iar);
    }

    static inline void irq_eoi(uint32_t irq)
    {
        gic_write_eoir(irq);
    }

    /* CPU 级中断开关：统一用 include/aarch64/exception_impl.h 的
     * arch_irq_enable()/arch_irq_disable()（这里曾有一份 enable_irqs/
     * disable_irqs 的副本，0 调用者，已删）。 */

/* ============================================================
 * GICv3
 * ============================================================ */
#elif DRIVER_GIC_V3

    #include "irq/gicv3.h"

    #define irq_init()              gicv3_init()
    #define irq_init_secondary()    gicv3_init_secondary()
    #define irq_enable_irq(n)       gicv3_enable_int((int)(n), true)
    #define irq_disable_irq(n)      gicv3_enable_int((int)(n), false)

    /* GICv3：通过系统寄存器读 ICC_IAR1_EL1 / 写 ICC_EOIR1_EL1 */
    static inline uint32_t irq_ack(void)
    {
        uint64_t iar;
        __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(iar));   /* ICC_IAR1_EL1 */
        return (uint32_t)(iar & ICC_IAR_INTID_MASK);
    }

    static inline void irq_eoi(uint32_t irq)
    {
        __asm__ volatile("msr S3_0_C12_C12_1, %0" :: "r"((uint64_t)irq)); /* ICC_EOIR1_EL1 */
    }

#endif  /* DRIVER_GIC_* */

#endif  /* DRIVER_IRQ_H */
