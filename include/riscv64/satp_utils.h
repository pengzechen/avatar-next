/*
 * include/riscv64/satp_utils.h - RISC-V Sv39 satp 操作工具
 *
 * 统一封装 satp 寄存器读写、PGD 物理地址转换以及用户页表内核映射复制。
 * 所有需要操作 satp 或初始化用户页表的内核代码必须通过此头文件，
 * 禁止在各模块内散落内联汇编和硬编码索引。
 */

#ifndef RISCV64_SATP_UTILS_H
#define RISCV64_SATP_UTILS_H

#include "types.h"

/*
 * Sv39 satp 寄存器布局：
 *   [63:60] MODE  = 8  → Sv39
 *   [59:44] ASID  = 0  （当前内核不使用 ASID）
 *   [43:0]  PPN        → 根页表物理地址 >> 12
 */
#define SATP_PPN_MASK    0x00000FFFFFFFFFFFULL   /* bits [43:0] */
#define SATP_SV39_MODE   (8ULL << 60)            /* MODE = Sv39 */

/*
 * 内核高半区 L1（Sv39 根页表）索引，由 KERNEL_VMA 严格派生：
 *
 *   KERNEL_VMA = 0xffffffc000000000
 *   L1_idx = (KERNEL_VMA >> 30) & 0x1ff
 *          = (0xffffffc000000000 >> 30) & 0x1ff
 *          = 0x3fffffffc0 & 0x1ff  →  0x100 = 256
 *
 *   L1[0x100] → KERNEL_VMA + 0x00000000..0x3fffffff  (MMIO 高半别名)
 *   L1[0x102] → KERNEL_VMA + 0x80000000..0xbfffffff  (DRAM 高半别名，含内核代码/数据)
 */
#define RISCV64_KERNEL_L1_MMIO_IDX  0x100U
#define RISCV64_KERNEL_L1_RAM_IDX   0x102U

/**
 * satp_read_pgd_phys - 读取当前 satp 并返回根页表物理地址
 */
static inline uint64_t satp_read_pgd_phys(void)
{
    uint64_t satp;
    __asm__ volatile("csrr %0, satp" : "=r"(satp) :: "memory");
    return (satp & SATP_PPN_MASK) << 12;
}

/**
 * pgd_phys_to_satp - 根据根页表物理地址构造 Sv39 satp 值
 * @pgd_phys: 根页表的物理地址（必须 4KB 对齐）
 */
static inline uint64_t pgd_phys_to_satp(uint64_t pgd_phys)
{
    return SATP_SV39_MODE | (pgd_phys >> 12);
}

/**
 * riscv64_copy_kernel_mappings - 将内核高半区 L1 条目复制到用户页表
 * @user_l1:   用户根页表（内核虚拟地址，uint64_t[512]）
 * @kernel_l1: 内核根页表（内核虚拟地址，uint64_t[512]）
 *
 * 必须在创建任何用户页表时调用：U 态发生 trap 时，CPU 通过 stvec
 * （内核高半区虚拟地址）跳转，若用户页表中无内核映射则立即失联。
 */
static inline void riscv64_copy_kernel_mappings(uint64_t *user_l1,
                                                const uint64_t *kernel_l1)
{
    user_l1[RISCV64_KERNEL_L1_MMIO_IDX] = kernel_l1[RISCV64_KERNEL_L1_MMIO_IDX];
    user_l1[RISCV64_KERNEL_L1_RAM_IDX]  = kernel_l1[RISCV64_KERNEL_L1_RAM_IDX];
}

#endif /* RISCV64_SATP_UTILS_H */
