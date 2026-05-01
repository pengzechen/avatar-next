#ifndef AARCH64_CPU_H
#define AARCH64_CPU_H

#include "types.h"

/**
 * aarch64_enable_neon - 启用 NEON/浮点指令
 *
 * 在 CPACR_EL1 寄存器中设置 FPEN 位，允许在 EL0 使用 NEON 和浮点指令。
 *
 * CPACR_EL1.FPEN [21:20]:
 *   - 0b00: 根据 CPACR_EL1.TTA 决定
 *   - 0b01: 执行 NEON/FP 时 Trap 到 EL1
 *   - 0b10: 执行 NEON/FP 时 Trap 到 EL2
 *   - 0b11: 允许在 EL0 使用 NEON/FP
 *
 * 注意：必须在内核初始化时调用，否则 NEON 指令会触发异常
 */
static inline void
aarch64_enable_neon(void)
{
    uint64_t cpacr;
    asm volatile(
        "mrs %0, cpacr_el1\n"       /* 读取 CPACR_EL1 */
        "orr %0, %0, #(3 << 20)\n"  /* 设置 FPEN[21:20] = 0b11 */
        "msr cpacr_el1, %0\n"       /* 写回 CPACR_EL1 */
        "isb\n"                     /* 指令同步屏障，确保修改生效 */
        : "=r"(cpacr)
        :
        : "memory");
}

/**
 * aarch64_get_cpacr - 读取 CPACR_EL1 寄存器
 *
 * 返回值: CPACR_EL1 寄存器的当前值
 */
static inline uint64_t
aarch64_get_cpacr(void)
{
    uint64_t cpacr;
    asm volatile(
        "mrs %0, cpacr_el1\n"
        : "=r"(cpacr)
        );
    return cpacr;
}

/**
 * aarch64_is_neon_enabled - 检查 NEON 是否已启用
 *
 * 返回值: 1 表示已启用，0 表示未启用
 */
static inline int
aarch64_is_neon_enabled(void)
{
    uint64_t cpacr = aarch64_get_cpacr();
    return ((cpacr >> 20) & 0x3) == 0x3;
}

/**
 * aarch64_disable_neon - 禁用 NEON/浮点指令
 *
 * 将 CPACR_EL1.FPEN 设置为 0b01，使 NEON/FP 指令触发异常
 *
 * 警告：禁用后执行 NEON 指令会导致异常
 */
static inline void
aarch64_disable_neon(void)
{
    uint64_t cpacr = aarch64_get_cpacr();
    cpacr &= ~(0x3 << 20);           /* 清除 FPEN 位 */
    cpacr |= (1 << 20);              /* 设置 FPEN = 0b01 */
    asm volatile(
        "msr cpacr_el1, %0\n"
        "isb\n"
        :
        : "r"(cpacr)
        : "memory");
}

#endif /* AARCH64_CPU_H */
