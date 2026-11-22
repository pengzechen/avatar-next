/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef AARCH64_CPU_IMPL_H
#define AARCH64_CPU_IMPL_H

/*
   AArch64 per-CPU 指针 / 硬件 ID 实现

   在 VHE 下 TPIDR_EL1 别名为 TPIDR_EL2, 与 EL2 host 内核透明兼容。
*/

#include "types.h"

/* 前向声明：cpu_t 在 include/task/cpu.h 中定义 */
struct cpu;

static inline void arch_cpu_self_set(struct cpu *self)
{
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(self) : "memory");
    __asm__ volatile("isb" ::: "memory");
}

static inline struct cpu *arch_cpu_self_get(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(v));
    return (struct cpu *)(uintptr_t)v;
}

/* MPIDR_EL1 低 24 位为 Affinity 0/1/2；QEMU virt 单簇时 Aff0 即 CPU 序号 */
static inline uint64_t arch_cpu_hw_id(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v & 0xFFFFFFULL;
}

/* ── idle 支撑 ──────────────────────────────────────────── */

/* idle 循环里的一条等待指令(中断唤醒) */
static inline void arch_cpu_idle_wait(void)
{
    __asm__ volatile("wfi" ::: "memory");
}

/*
   arch_switch_to_idle_stack - 切到 idle 专用栈并跳进 idle 主循环(不返回)

   必须绕过编译器生成的函数 epilogue：新栈上还没有本函数的返回地址
   和帧指针, 正常 return 会从栈上读到垃圾 LR/FP 直接跳飞。
*/
static inline __attribute__((noreturn)) void arch_switch_to_idle_stack(uintptr_t new_sp)
{
    __asm__ volatile("mov sp, %0\n\t"
                     "mov x29, xzr\n\t" /* 清零 FP, 标记栈帧链结束 */
                     "b   task_idle_loop\n\t" ::"r"(new_sp)
                     : "memory");
    __builtin_unreachable();
}

#endif /* AARCH64_CPU_IMPL_H */
