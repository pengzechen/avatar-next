#ifndef MM_VM_H
#define MM_VM_H

/*
 * include/mm_vm.h — 用户虚拟内存操作公共接口
 *
 * 将页表映射/查询等 VM 操作按架构分发，
 * 让上层（syscall/loader）不直接依赖 AArch64 私有符号。
 */

#include "arch.h"
#include "types.h"

#if ARCH_AARCH64
#  include "aarch64/mm_vm.h"
#elif ARCH_RISCV64
#  include "riscv64/mm_vm.h"
#elif ARCH_X86_64
#  include "x86_64/mm_vm.h"
#else
#  error "Unsupported architecture for mm_vm"
#endif


/* ── 内核虚拟地址偏移（架构特定） ───────────────────────────────── */

#if ARCH_AARCH64
/* AArch64 TTBR1 高半地址 */
#define KERNEL_VMA  0xffff000000000000ULL
#elif ARCH_X86_64
/* x86_64 高半地址空间 */
#define KERNEL_VMA  0xffff800000000000ULL
#elif ARCH_RISCV64
/* RISC-V MMU 尚未实现，内核直接运行在物理地址空间，偏移为 0 */
#define KERNEL_VMA  0ULL
#else
#define KERNEL_VMA  0ULL
#endif

/* 虚拟地址转换宏 */
#define phys_to_virt(pa) ((void *)((uint64_t)(pa) + KERNEL_VMA))
#define virt_to_phys(va) ((uint64_t)(va) - KERNEL_VMA)

/* 页大小配置 */
#define PAGE_SIZE       4096
#define PAGE_SHIFT      12


#endif /* MM_VM_H */
