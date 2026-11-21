/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef X86_64_CPU_IMPL_H
#define X86_64_CPU_IMPL_H

/*
   x86_64 per-CPU 指针 / 硬件 ID 实现

   约定：
     IA32_GS_BASE        (MSR 0xC0000101) — 用户态运行时保存 cpu_t*
     IA32_KERNEL_GS_BASE (MSR 0xC0000102) — 内核态运行时保存 cpu_t*

   syscall_entry 执行 swapgs 后 GS_BASE = cpu_t*(从 KERNEL_GS_BASE 换入),
   可通过 gs:offset 访问 per-CPU 数据。sysretq 前再次 swapgs 换回。

   内核纯态路径(非 SYSCALL)仍通过 rdmsr(GS_BASE) 读取 cpu_t*。
*/

#include "types.h"

struct cpu;

#define X86_MSR_GS_BASE        0xC0000101U
#define X86_MSR_KERNEL_GS_BASE 0xC0000102U

static inline void arch_cpu_self_set(struct cpu *self)
{
    uint64_t v = (uint64_t)self;
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    __asm__ volatile("wrmsr" ::"c"(X86_MSR_GS_BASE), "a"(lo), "d"(hi) : "memory");
    __asm__ volatile("wrmsr" ::"c"(X86_MSR_KERNEL_GS_BASE), "a"(lo), "d"(hi) : "memory");
}

static inline struct cpu *arch_cpu_self_get(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(X86_MSR_GS_BASE));
    uint64_t v = ((uint64_t)hi << 32) | lo;
    return (struct cpu *)(uintptr_t)v;
}

/* x86 的硬件 ID 是 LAPIC ID，但本头文件不能依赖 LAPIC 映射，因此这里
   只返回 0 占位。BSP 在 cpu_init_bsp()、AP 在 cpu_secondary_bootstrap()
   中都改用 irq_cpu_hw_id() 读取真实值。
*/
static inline uint64_t arch_cpu_hw_id(void)
{
    return 0;
}

#endif /* X86_64_CPU_IMPL_H */
