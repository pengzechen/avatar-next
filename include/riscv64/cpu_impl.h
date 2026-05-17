#ifndef RISCV64_CPU_IMPL_H
#define RISCV64_CPU_IMPL_H

/*
 * RISC-V 64 per-CPU 指针 / 硬件 ID 实现
 *
 * ⚠️ TLS 冲突说明：
 *   RISC-V 的 tp 寄存器在用户态被 musl/pthread 用作 TLS 基址，
 *   内核态如果也占用 tp，需要在 trap 入口做用户/内核 tp 交换，
 *   且需要一个独立暂存（如 mscratch 或自建 per-hart 表）来恢复内核 tp。
 *   当前 sscratch 已被占用存内核栈指针，没有空闲的暂存 CSR。
 *
 * Phase 0 决策：暂不使用 tp。
 *   - arch_cpu_self_set() 为 no-op
 *   - arch_cpu_self_get() 返回 NULL，由 cpu_current() 回退到 &g_cpus[0]
 *   - 单核场景完全够用，且不破坏 pthread TLS。
 *
 * 后续 SMP 阶段（Phase ≥ 2）需要：
 *   1) 修改 boot/riscv64/exception.S：U→S 入口保存用户 tp 后，
 *      从 per-hart 表（按 sscratch 拿到的内核栈反查，或经 SBI hartid）
 *      重新加载内核 tp；S→U 出口已经从 trap_frame 恢复用户 tp，无需改动。
 *   2) 切换 switch.S：上下文切换时 tp 不变（per-CPU 跟核走，不跟任务走）。
 *   3) 改本文件为 mv tp / mv %0, tp 形式。
 */

#include "types.h"

struct cpu;

static inline void
arch_cpu_self_set(struct cpu *self)
{
    (void)self;   /* Phase 0：不占用 tp，避免破坏用户 TLS */
}

static inline struct cpu *
arch_cpu_self_get(void)
{
    return NULL;  /* 由 cpu_current() 回退到 &g_cpus[0] */
}

/* mhartid 在 S-mode 不可读；hartid 由 OpenSBI 在启动时通过 a0 传入，
 * 后续阶段存入 cpu_t::hw_id。Phase 0 单核：返回 0。 */
static inline uint64_t
arch_cpu_hw_id(void)
{
    return 0;
}

#endif /* RISCV64_CPU_IMPL_H */
