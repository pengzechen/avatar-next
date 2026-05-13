/*
 * include/x86_64/cpu_impl.h — x86_64 per-CPU 实现
 *
 * 基于 FS 基址寄存器
 * x86_64 的 System V AMD64 ABI 使用 FS base 指向 TLS
 * 内核可以使用它作为 per-CPU 数据指针。
 */

#ifndef _X86_64_CPU_IMPL_H
#define _X86_64_CPU_IMPL_H

#include "../types.h"

/* ── MSR 编号 ──────────────────────────────────────────── */

#define MSR_IA32_FS_BASE  0xC0000100U  /* FS base address MSR */
#define MSR_IA32_GS_BASE  0xC0000101U  /* GS base address MSR */

/* ── MSR 读写 ──────────────────────────────────────────── */

static inline uint64_t
read_msr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void
write_msr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFU);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

/* ── FS Base 访问 ──────────────────────────────────────── */

static inline uint64_t
read_fs_base(void)
{
    return read_msr(MSR_IA32_FS_BASE);
}

static inline void
write_fs_base(uint64_t value)
{
    write_msr(MSR_IA32_FS_BASE, value);
}

/* ── 获取当前 CPU ID ────────────────────────────────────── */

static inline uint32_t
get_current_cpu_id(void)
{
    /* FS base 的低 32 位存储 CPU ID */
    uint64_t fs_base = read_fs_base();
    return (uint32_t)(fs_base & 0xFFFFFFFFULL);
}

/* ── 获取当前 CPU 数据结构指针 ────────────────────────────── */

static inline void *
get_cpu_data_ptr(void)
{
    return (void *)read_fs_base();
}

#endif /* _X86_64_CPU_IMPL_H */
