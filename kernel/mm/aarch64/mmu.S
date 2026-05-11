#include "../../../include/aarch64/tcr.h"
#include "../../../include/aarch64/mair.h"

/*
 * mmu_init - 初始化并启用 MMU
 * x0: TTBR0_EL1 值（用户空间页表）
 * x1: TTBR_EL1 值（内核空间页表，物理地址）
 *
 * 配置：
 * - 48 位虚拟地址
 * - TTBR0：用户空间（0x0000_0000_0000_0000 - 0x0000_ffff_ffff_ffff）
 * - TTBR1：内核空间（0xffff_0000_0000_0000 - 0xffff_ffff_ffff_ffff）
 * - MAIR：设备内存、普通内存、无缓存内存
 */
.global mmu_init
mmu_init:
    /* 设置 TTBR0_EL1（用户空间页表） */
    msr ttbr0_el1, x0

    /* 设置 TTBR1_EL1（内核空间页表） */
    msr ttbr1_el1, x1

    ldr x0, =(TCR_EL1)
    msr tcr_el1, x0

    ldr x0, =(MAIR_VALUE)
    msr mair_el1, x0

    /* 再启用 MMU */
    mrs x2, sctlr_el1
    orr x2, x2, #(1 << 0)      /* SCTLR_EL1.M - MMU 使能 */
    orr x2, x2, #(1 << 23)     /* SCTLR_EL1.SPAN=1 - 陷入 EL2 时不自动设置 PAN */
    msr sctlr_el1, x2

    /* 内存屏障 */
    dsb sy
    isb

    ret

/*
 * mmu_disable - 禁用 MMU（调试用）
 */
.global mmu_disable
mmu_disable:
    mrs x0, sctlr_el1
    bic x0, x0, #(1 << 0)      /* SCTLR_EL1.M - MMU 禁用 */
    bic x0, x0, #(1 << 2)      /* SCTLR_EL1.C - 数据缓存禁用 */
    bic x0, x0, #(1 << 12)     /* SCTLR_EL1.I - 指令缓存禁用 */
    msr sctlr_el1, x0

    /* 内存屏障 */
    dsb sy
    isb

    ret
