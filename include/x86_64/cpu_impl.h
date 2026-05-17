#ifndef X86_64_CPU_IMPL_H
#define X86_64_CPU_IMPL_H

/*
 * x86_64 per-CPU 指针 / 硬件 ID 实现
 *
 * 约定：IA32_GS_BASE (MSR 0xC0000101) 保存当前 cpu_t* 指针。
 * 当前内核还没启用 swapgs，所以始终从 GS_BASE 读取，
 * 用户态引入后再加 swapgs 切换 KERNEL_GS_BASE。
 */

#include "types.h"

struct cpu;

#define X86_MSR_GS_BASE        0xC0000101U

static inline void
arch_cpu_self_set(struct cpu *self)
{
    uint64_t v  = (uint64_t)self;
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    __asm__ volatile("wrmsr" :: "c"(X86_MSR_GS_BASE), "a"(lo), "d"(hi) : "memory");
}

static inline struct cpu *
arch_cpu_self_get(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(X86_MSR_GS_BASE));
    uint64_t v = ((uint64_t)hi << 32) | lo;
    return (struct cpu *)(uintptr_t)v;
}

/* LAPIC ID 需要 LAPIC 映射就绪后才能读；Phase 0 返回 0 占位，
 * 后续 SMP 阶段改为 cpu_current()->hw_id（BSP 启动时存入）。 */
static inline uint64_t
arch_cpu_hw_id(void)
{
    return 0;
}

#endif /* X86_64_CPU_IMPL_H */
