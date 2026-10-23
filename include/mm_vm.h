/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef MM_VM_H
#define MM_VM_H

/*
   include/mm_vm.h — 用户虚拟内存操作公共接口

   将页表映射/查询等 VM 操作按架构分发,
   让上层(syscall/loader)不直接依赖 AArch64 私有符号。
*/

#include "arch.h"
#include "page.h"
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
/* AArch64 TTBR1 高半地址：唯一定义在 aarch64/mem_layout.h */
#include "aarch64/mem_layout.h"
#define KERNEL_VMA AARCH64_KERNEL_VMA_OFF
#elif ARCH_X86_64
/* x86_64 高半地址空间：唯一定义在 x86_64/mem_layout.h */
#include "x86_64/mem_layout.h"
#define KERNEL_VMA X86_KERNEL_VMA_OFF
#elif ARCH_RISCV64
/* RISC-V Sv39 高半区内核偏移：唯一定义在 riscv64/mem_layout.h */
#include "riscv64/mem_layout.h"
#define KERNEL_VMA RV_KERNEL_VMA_OFF
#else
#define KERNEL_VMA 0ULL
#endif

/* 虚拟地址转换宏 */
#define phys_to_virt(pa) ((void *)((uint64_t)(pa) + KERNEL_VMA))
#define virt_to_phys(va) ((uint64_t)(va) - KERNEL_VMA)

/* 页大小配置见 page.h(必须在架构特定头文件之前可用) */

#if ARCH_AARCH64
#include "aarch64/mm_vm.h"
#elif ARCH_RISCV64
#include "riscv64/mm_vm.h"
#elif ARCH_X86_64
#include "x86_64/mm_vm.h"
#else
#error "Unsupported architecture for mm_vm"
#endif

#endif /* MM_VM_H */
