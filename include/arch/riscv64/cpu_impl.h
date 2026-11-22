/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef RISCV64_CPU_IMPL_H
#define RISCV64_CPU_IMPL_H

/*
   RISC-V 64 per-CPU 指针 / 硬件 ID 实现。

   内核态用 tp 保存 cpu_t*。进入用户态前 switch.S 会恢复/清空用户 tp；
   U->S trap 的 C 入口会根据 sscratch 中的内核栈恢复内核 tp。
*/

#include "types.h"

struct cpu;
extern volatile uint64_t riscv64_boot_hartid;

static inline void arch_cpu_self_set(struct cpu *self)
{
    __asm__ volatile("mv tp, %0" ::"r"(self) : "memory");
}

static inline struct cpu *arch_cpu_self_get(void)
{
    struct cpu *self;
    __asm__ volatile("mv %0, tp" : "=r"(self));
    return self;
}

/* mhartid 在 S-mode 不可读；hartid 由 OpenSBI/HSM 传入并存入 cpu_t::hw_id。 */
static inline uint64_t arch_cpu_hw_id(void)
{
    return riscv64_boot_hartid;
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
   和帧指针, 正常 return 会从栈上读到垃圾 ra/fp 直接跳飞。
*/
static inline __attribute__((noreturn)) void arch_switch_to_idle_stack(uintptr_t new_sp)
{
    __asm__ volatile("mv sp, %0\n\t"
                     "mv fp, zero\n\t" /* 清零 FP, 标记栈帧链结束 */
                     "j  task_idle_loop\n\t" ::"r"(new_sp)
                     : "memory");
    __builtin_unreachable();
}

#endif /* RISCV64_CPU_IMPL_H */
