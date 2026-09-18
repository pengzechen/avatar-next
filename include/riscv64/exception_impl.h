#ifndef RISCV64_EXCEPTION_IMPL_H
#define RISCV64_EXCEPTION_IMPL_H

#include "types.h"

/*
 * RISC-V 64 中断屏蔽原语 —— **全项目唯一的定义**（汇编除外）。
 *
 * 由 include/exception.h 统一暴露。任何 C 代码要关/开中断、要读中断状态，
 * 都用这里的接口，不要再写 `csrci sstatus` / `csrr sstatus`。
 *
 * sstatus.SIE = bit 1。注意这里的 restore 恢复的是**整份 sstatus**
 * （与改动前一致），不是只写 SIE 位。
 */

/* 读当前 sstatus（不改动状态） */
static inline uint64_t
arch_irq_flags(void)
{
    uint64_t status;

    __asm__ volatile("csrr %0, sstatus" : "=r"(status) : : "memory");

    return status;
}

/* 读是否允许 IRQ（抢占判断用） */
static inline int
arch_irq_is_enabled(void)
{
    return (arch_irq_flags() & 2UL) != 0;
}

/* 关中断并返回旧 sstatus（必须用 arch_irq_restore 配对恢复） */
static inline uint64_t
arch_irq_save(void)
{
    uint64_t status;

    /* csrrci: 读出旧值后清除 bit1 (SIE) */
    __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(status) : : "memory");

    return status;
}

/* 恢复到 arch_irq_save()/arch_irq_flags() 取到的状态 */
static inline void
arch_irq_restore(uint64_t flags)
{
    __asm__ volatile("csrw sstatus, %0" : : "r"(flags) : "memory");
}

/* 无条件关中断（不需要旧状态时用） */
static inline void
arch_irq_disable(void)
{
    __asm__ volatile("csrci sstatus, 2" ::: "memory");
}

/* 无条件开中断（新任务首次运行时使用） */
static inline void
arch_irq_enable(void)
{
    __asm__ volatile("csrsi sstatus, 2" ::: "memory");
}

#endif /* RISCV64_EXCEPTION_IMPL_H */
