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
 *   enable_irqs()           使能 CPU 级中断（清 DAIF.I）
 *   disable_irqs()          禁用 CPU 级中断（设 DAIF.I）
 */

#ifndef DRIVER_IRQ_H
#define DRIVER_IRQ_H

#include "driver_cfg.h"
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

    /* CPU 级中断开关（来自 gicv2.h 的 enable/disable_interrupts） */
    #define enable_irqs()           enable_interrupts()
    #define disable_irqs()          disable_interrupts()

/* ============================================================
 * GICv3
 * ============================================================ */
#elif DRIVER_GIC_V3

    #include "irq/gicv3.h"

    #define irq_init()              gicv3_init()
    #define irq_init_secondary()    gicv3_init()
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

    /* CPU 级中断开关（直接操作 DAIF） */
    static inline void enable_irqs(void)
    {
        __asm__ volatile("msr daifclr, #2" ::: "memory");
    }

    static inline void disable_irqs(void)
    {
        __asm__ volatile("msr daifset, #2" ::: "memory");
    }

#endif  /* DRIVER_GIC_* */

#endif  /* DRIVER_IRQ_H */
