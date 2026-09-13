#ifndef RISCV64_CPU_IMPL_H
#define RISCV64_CPU_IMPL_H

/*
 * RISC-V 64 per-CPU 指针 / 硬件 ID 实现。
 *
 * 内核态用 tp 保存 cpu_t*。进入用户态前 switch.S 会恢复/清空用户 tp；
 * U->S trap 的 C 入口会根据 sscratch 中的内核栈恢复内核 tp。
 */

#include "types.h"

struct cpu;
extern volatile uint64_t riscv64_boot_hartid;

static inline void
arch_cpu_self_set(struct cpu *self)
{
    __asm__ volatile("mv tp, %0" :: "r"(self) : "memory");
}

static inline struct cpu *
arch_cpu_self_get(void)
{
    struct cpu *self;
    __asm__ volatile("mv %0, tp" : "=r"(self));
    return self;
}

/* mhartid 在 S-mode 不可读；hartid 由 OpenSBI/HSM 传入并存入 cpu_t::hw_id。 */
static inline uint64_t
arch_cpu_hw_id(void)
{
    return riscv64_boot_hartid;
}

#endif /* RISCV64_CPU_IMPL_H */
