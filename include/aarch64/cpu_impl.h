/*
 * include/aarch64/cpu_impl.h — AArch64 per-CPU 实现
 *
 * 基于 TPIDR_EL1 （Thread Pointer ID Register for EL1）
 * 内核可以将其用作 per-CPU 数据指针。
 */

#ifndef _AARCH64_CPU_IMPL_H
#define _AARCH64_CPU_IMPL_H

#include "../types.h"

/* ── TPIDR_EL1 访问 ──────────────────────────────────────── */

static inline uint64_t
read_tpidr_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, TPIDR_EL1" : "=r"(value));
    return value;
}

static inline void
write_tpidr_el1(uint64_t value)
{
    __asm__ volatile("msr TPIDR_EL1, %0" :: "r"(value));
    __asm__ volatile("isb sy");  /* 指令同步屏障，确保立即生效 */
}

/* ── 获取当前 CPU ID ────────────────────────────────────── */

static inline uint32_t
get_current_cpu_id(void)
{
    /* TPIDR_EL1 的低 32 位存储 CPU ID */
    uint64_t tpidr = read_tpidr_el1();
    return (uint32_t)(tpidr & 0xFFFFFFFFULL);
}

/* ── 获取当前 CPU 数据结构指针 ────────────────────────────── */

/*
 * 注意：这需要在 cpu.h 中与 cpu_t 配合使用。
 * 当在内核中使用 per_cpu() 宏时，这个函数会被自动调用。
 */
static inline void *
get_cpu_data_ptr(void)
{
    return (void *)read_tpidr_el1();
}

#endif /* _AARCH64_CPU_IMPL_H */
