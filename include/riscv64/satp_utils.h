/*
   include/riscv64/satp_utils.h - RISC-V Sv39 satp 操作工具

   统一封装 satp 寄存器读写, PGD 物理地址转换以及用户页表内核映射复制。
   所有需要操作 satp 或初始化用户页表的内核代码必须通过此头文件。

   内核高半区映射的唯一来源是 riscv64/mem_layout.h 的
   RV_KERNEL_MAP_LIST；这里不再逐条硬编码 0x100..0x103。
*/

#ifndef RISCV64_SATP_UTILS_H
#define RISCV64_SATP_UTILS_H

#include "mem_layout.h"
#include "types.h"

/*
   satp_read_pgd_phys - 读取当前 satp 并返回根页表物理地址
*/
static inline uint64_t satp_read_pgd_phys(void)
{
    uint64_t satp;
    __asm__ volatile("csrr %0, satp" : "=r"(satp)::"memory");
    return (satp & RV_SATP_PPN_MASK) << PAGE_SHIFT;
}

/*
   pgd_phys_to_satp - 根据根页表物理地址构造 Sv39 satp 值
   @pgd_phys: 根页表的物理地址(必须 4KB 对齐)
*/
static inline uint64_t pgd_phys_to_satp(uint64_t pgd_phys)
{
    return RV_SATP_MODE_SV39 | (pgd_phys >> PAGE_SHIFT);
}

/*
   用户页表只拥有 [0, RISCV64_KERNEL_L1_FIRST_IDX) 的根项；
   高半区内核映射从 FIRST_IDX 开始, 来自 RV_KERNEL_MAP_LIST。
*/
#define RISCV64_KERNEL_L1_FIRST_IDX RV_KERNEL_L1_BASE_INDEX

/*
   riscv64_copy_kernel_mappings - 将内核高半区 L1 条目复制到用户页表
   @user_l1:   用户根页表(内核虚拟地址, uint64_t[512])
   @kernel_l1: 内核根页表(内核虚拟地址, uint64_t[512])

   必须在创建任何用户页表时调用：U 态发生 trap 时, CPU 通过 stvec
   (内核高半区虚拟地址)跳转, 若用户页表中无内核映射则立即失联。

   复制哪些条目完全由 RV_KERNEL_MAP_LIST(mem_layout.h)决定,
   mmu.S 的启动映射也用同一份列表, 避免两边漂移。
*/
static inline void riscv64_copy_kernel_mappings(uint64_t *user_l1, const uint64_t *kernel_l1)
{
#define RV_COPY_KERNEL_MAP(pa, attr) user_l1[RV_KERNEL_HIGH_INDEX(pa)] = kernel_l1[RV_KERNEL_HIGH_INDEX(pa)];
    RV_KERNEL_MAP_LIST(RV_COPY_KERNEL_MAP)
#undef RV_COPY_KERNEL_MAP
}

#endif /* RISCV64_SATP_UTILS_H */
