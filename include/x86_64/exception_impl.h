#ifndef X86_64_EXCEPTION_IMPL_H
#define X86_64_EXCEPTION_IMPL_H

#include "types.h"

/*
 * x86_64 中断屏蔽原语 —— **全项目唯一的定义**（汇编除外）。
 *
 * 由 include/exception.h 统一暴露。任何 C 代码要关/开中断、要读中断状态，
 * 都用这里的接口，不要再写 `cli` / `sti` / `pushfq; popq`。
 *
 * RFLAGS.IF = bit 9。
 */

/* 读当前 RFLAGS（不改动状态） */
static inline uint64_t
arch_irq_flags(void)
{
    uint64_t flags;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags) : : "memory");

    return flags;
}

/* 读是否允许 IRQ（抢占判断用） */
static inline int
arch_irq_is_enabled(void)
{
    return (arch_irq_flags() & (1UL << 9)) != 0;
}

/* 关中断并返回旧 RFLAGS（必须用 arch_irq_restore 配对恢复） */
static inline uint64_t
arch_irq_save(void)
{
    uint64_t flags;

    __asm__ volatile(
        "pushfq         \n"
        "popq %0        \n"
        "cli            \n"
        : "=r"(flags)
        :
        : "memory");

    return flags;
}

/* 恢复到 arch_irq_save()/arch_irq_flags() 取到的状态（POPFQ 恢复 IF） */
static inline void
arch_irq_restore(uint64_t flags)
{
    __asm__ volatile(
        "pushq %0       \n"
        "popfq          \n"
        :
        : "r"(flags)
        : "memory", "cc");
}

/* 无条件关中断（不需要旧状态时用） */
static inline void
arch_irq_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}

/* 无条件开中断（新任务首次运行时使用） */
static inline void
arch_irq_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

#endif /* X86_64_EXCEPTION_IMPL_H */
