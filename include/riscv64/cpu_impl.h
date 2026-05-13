/*
 * include/riscv64/cpu_impl.h — RISC-V 64 per-CPU 实现
 *
 * 基于 tp （Thread Pointer）寄存器
 * RISC-V 的 ABI 规约将 tp 用于 TLS（Thread Local Storage）
 * 内核可以扩展使用它作为 per-CPU 数据指针。
 */

#ifndef _RISCV64_CPU_IMPL_H
#define _RISCV64_CPU_IMPL_H

#include "../types.h"

/* ── TP 寄存器访问 ──────────────────────────────────────── */

static inline uint64_t
read_tp(void)
{
    uint64_t value;
    __asm__ volatile("mv %0, tp" : "=r"(value));
    return value;
}

static inline void
write_tp(uint64_t value)
{
    __asm__ volatile("mv tp, %0" :: "r"(value));
    /* 无需屏障，tp 赋值立即生效 */
}

/* ── 获取当前 CPU ID ────────────────────────────────────── */

static inline uint32_t
get_current_cpu_id(void)
{
    /* tp 的低 32 位存储 CPU ID */
    uint64_t tp_val = read_tp();
    return (uint32_t)(tp_val & 0xFFFFFFFFULL);
}

/* ── 获取当前 CPU 数据结构指针 ────────────────────────────── */

static inline void *
get_cpu_data_ptr(void)
{
    return (void *)read_tp();
}

#endif /* _RISCV64_CPU_IMPL_H */
