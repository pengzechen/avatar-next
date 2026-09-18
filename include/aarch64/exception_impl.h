#ifndef AARCH64_EXCEPTION_IMPL_H
#define AARCH64_EXCEPTION_IMPL_H

#include "types.h"

/*
 * AArch64 中断屏蔽原语 —— **全项目唯一的定义**（汇编除外）。
 *
 * 由 include/exception.h 统一暴露。任何 C 代码要关/开中断、要读中断状态，
 * 都用这里的接口，不要再写 `msr daifset` / `mrs daif`。
 *
 * DAIF 位：D(9) A(8) I(7) F(6)。本内核只用 DAIF.I 屏蔽 IRQ。
 *   - arch_irq_is_enabled() 问的是"IRQ 是否放行"，即 DAIF.I == 0。
 */

/* 读当前 DAIF（不改动状态） */
static inline uint64_t
arch_irq_flags(void)
{
    uint64_t daif;

    __asm__ volatile("mrs %0, daif" : "=r"(daif) : : "memory");

    return daif;
}

/* 读是否允许 IRQ（抢占判断用：处于 irqsave 临界区时为 0） */
static inline int
arch_irq_is_enabled(void)
{
    return (arch_irq_flags() & (1UL << 7)) == 0;
}

/* 关中断并返回旧状态（必须用 arch_irq_restore 配对恢复） */
static inline uint64_t
arch_irq_save(void)
{
    uint64_t daif;

    __asm__ volatile(
        "mrs %0, daif       \n"
        "msr daifset, #2    \n"
        : "=r"(daif)
        :
        : "memory");

    return daif;
}

/* 恢复到 arch_irq_save()/arch_irq_flags() 取到的状态 */
static inline void
arch_irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
}

/* 无条件关中断（不需要旧状态时用） */
static inline void
arch_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

/* 无条件开中断（新任务首次运行时使用） */
static inline void
arch_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

#endif /* AARCH64_EXCEPTION_IMPL_H */
