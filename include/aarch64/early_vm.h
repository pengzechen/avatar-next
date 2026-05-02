#ifndef VM_H
#define VM_H

#include "types.h"
#include "mmu.h"

/* ── 内存布局配置 ───────────────────────────────────────────────────── */

/* 物理内存区域（QEMU virt: 2GB @ 0x4000_0000） */
#define RAM_START    0x40000000UL
#define RAM_SIZE     (2UL * 1024 * 1024 * 1024)  /* 2GB */
#define RAM_END      (RAM_START + RAM_SIZE)

/* 设备内存区域（0x0000_0000 - 0x0000_0001_0000_0000，即 0-1GB） */
#define DEVICE_START  0x00000000UL
#define DEVICE_SIZE   (1UL * 1024 * 1024 * 1024)  /* 1GB */
#define DEVICE_END    (DEVICE_START + DEVICE_SIZE)

/* 内核使用的物理内存（1GB - 2GB，即 0x4000_0000 - 0x8000_0000） */
#define KERNEL_PHYS_START  0x40000000UL
#define KERNEL_PHYS_SIZE   (1UL * 1024 * 1024 * 1024)  /* 1GB */
#define KERNEL_PHYS_END    (KERNEL_PHYS_START + KERNEL_PHYS_SIZE)

/* ── 页表管理 ───────────────────────────────────────────────────── */

/**
 * vm_init - 初始化虚拟内存管理器
 *
 * 建立初始页表映射：
 * - 0 - 1GB（+偏移）：设备内存（nGnRnE）
 * - 1GB - 2GB（+偏移）：普通内存（Write-Back）
 *
 * 返回：0 表示成功，负值表示失败
 */
uint64_t vm_init(void);

/**
 * vm_get_boot_pgtable - 获取启动页表基址（物理地址）
 *
 * 返回：启动页表物理地址（用于 TTBR0_EL1，低地址恒等映射）
 */
uint64_t vm_get_boot_pgtable(void);

/**
 * vm_get_kernel_pgtable - 获取内核页表基址（物理地址）
 *
 * 返回：内核页表物理地址（用于 TTBR1_EL1）
 */
uint64_t vm_get_kernel_pgtable(void);


#endif /* VM_H */
