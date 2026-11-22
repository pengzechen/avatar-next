/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef MM_VM_H
#define MM_VM_H

/*
   include/mm/mm_vm.h — 用户虚拟内存操作公共接口

   将页表映射/查询等 VM 操作按架构分发,
   让上层(syscall/loader)不直接依赖 AArch64 私有符号。
*/

#include "arch.h"
#include "mm/page.h"
#include "types.h"

/*
   页映射权限。三架构的 mm_vm_map_pages() 共用这套取值, 禁止再传裸 0/1/2。
*/
typedef enum {
    VM_PERM_USER = 0,        /* 用户态：RWX + 普通内存 */
    VM_PERM_KERNEL = 1,      /* 内核态：RWX + 普通内存 */
    VM_PERM_KERNEL_MMIO = 2, /* 内核态：RW + 设备/非缓存内存 */
} vm_perm_t;

/* ── 内核虚拟地址偏移(架构特定) ───────────────────────────────── */

#if ARCH_AARCH64
/* AArch64 TTBR1 高半地址：唯一定义在 arch/aarch64/mem_layout.h */
#include "arch/aarch64/mem_layout.h"
#define KERNEL_VMA AARCH64_KERNEL_VMA_OFF
#elif ARCH_X86_64
/* x86_64 高半地址空间：唯一定义在 arch/x86_64/mem_layout.h */
#include "arch/x86_64/mem_layout.h"
#define KERNEL_VMA X86_KERNEL_VMA_OFF
#elif ARCH_RISCV64
/* RISC-V Sv39 高半区内核偏移：唯一定义在 arch/riscv64/mem_layout.h */
#include "arch/riscv64/mem_layout.h"
#define KERNEL_VMA RV_KERNEL_VMA_OFF
#else
#define KERNEL_VMA 0ULL
#endif

/* 虚拟地址转换宏 */
#define phys_to_virt(pa) ((void *)((uint64_t)(pa) + KERNEL_VMA))
#define virt_to_phys(va) ((uint64_t)(va) - KERNEL_VMA)

/* 页大小配置见 page.h(必须在架构特定头文件之前可用) */

#if ARCH_AARCH64
#include "arch/aarch64/mm_vm.h"
#elif ARCH_RISCV64
#include "arch/riscv64/mm_vm.h"
#elif ARCH_X86_64
#include "arch/x86_64/mm_vm.h"
#else
#error "Unsupported architecture for mm_vm"
#endif

/* ── 内核根页表 ────────────────────────────────────────────────── */

/*
   mm_vm_kernel_root_phys - 内核根页表的物理地址

     x86_64  : PML4(CR3 指向的内核页表)
     RISC-V  : Sv39 根页表(satp.PPN)
     AArch64 : 恒 0 —— 内核映射在 TTBR1, 不存在与用户页表共享的根表

   该值在 boot 阶段由 mm_vm_kernel_root_init() 记录一次, 运行期不变。
   调度器从用户任务切回内核任务、以及为用户进程构造页表时都要用它。
*/
uint64_t mm_vm_kernel_root_phys(void);

/*
   mm_vm_kernel_root_init - 在仍运行于内核地址空间时调用一次

   x86_64/RISC-V 的用户地址空间共享根表中的内核半区, 所以用户任务切出去
   之后就不能再从 CR3/satp 现读内核根表了 —— 必须在这里先记下来。
*/
void mm_vm_kernel_root_init(void);

#endif /* MM_VM_H */
