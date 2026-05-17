#ifndef AARCH64_CPU_IMPL_H
#define AARCH64_CPU_IMPL_H

/*
 * AArch64 per-CPU 指针 / 硬件 ID 实现
 *
 * 在 VHE 下 TPIDR_EL1 别名为 TPIDR_EL2，与 EL2 host 内核透明兼容。
 */

#include "types.h"

/* 前向声明：cpu_t 在 kernel/task/cpu.h 中定义 */
struct cpu;

static inline void
arch_cpu_self_set(struct cpu *self)
{
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(self) : "memory");
    __asm__ volatile("isb" ::: "memory");
}

static inline struct cpu *
arch_cpu_self_get(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(v));
    return (struct cpu *)(uintptr_t)v;
}

/* MPIDR_EL1 低 24 位为 Affinity 0/1/2；QEMU virt 单簇时 Aff0 即 CPU 序号 */
static inline uint64_t
arch_cpu_hw_id(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v & 0xFFFFFFULL;
}

#endif /* AARCH64_CPU_IMPL_H */
