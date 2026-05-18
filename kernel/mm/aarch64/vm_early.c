/*
 * kernel/mm/vm.c - AArch64 虚拟内存管理
 *
 * 参考：ref/Avatar/kernel/boot/earlypage.c
 * 实现 48 位页表，使用 TTBR0/TTBR1 双页表结构
 */

#include "mm_vm.h"
#include "aarch64/early_vm.h"
#include "aarch64/mmu.h"
#include "klog.h"
#include "string.h"

/* ── 全局变量 ───────────────────────────────────────────────────── */

/*
 * 页表结构（2 级，使用 1GB 块映射）
 *
 * TTBR0（低地址空间）：用户空间（暂不使用）
 * TTBR1（高地址空间）：内核空间
 */

/* TTBR1 页表（高地址空间，内核使用） */
static uint64_t kernel_pt0[1] __attribute__((aligned(PAGE_SIZE)));    /* L0 页表 */
static uint64_t kernel_pt1[512] __attribute__((aligned(PAGE_SIZE)));  /* L1 页表 */

/* TTBR0 页表（低地址空间，用于开启MMU前的恒等映射） */
static uint64_t boot_pt0[1] __attribute__((aligned(PAGE_SIZE)));    /* L0 页表 */
static uint64_t boot_pt1[512] __attribute__((aligned(PAGE_SIZE)));  /* L1 页表 */

/* 页表项标志（参考 earlypage.c） */
#define PTE_TABLE_FLAGS  0b11  /* 页表项标志（有效+表） */

/* ── 辅助函数 ───────────────────────────────────────────────────── */

/**
 * set_table_entry - 设置页表项指向下一级页表
 */
static inline void set_table_entry(uint64_t *pte, uint64_t *next_table) {
    *pte = (uint64_t)next_table | PTE_TABLE_FLAGS;
}

/**
 * set_block_entry - 设置 1GB 块映射页表项
 */
static inline void set_block_entry(uint64_t *pte, uint64_t base_addr, uint64_t flags) {
    *pte = base_addr | flags;
}

/* ── 内核页表 ───────────────────────────────────────────────────── */

/**
 * vm_kernel_pgtable - 获取内核页表基址（虚拟地址）
 */
pte_t *vm_kernel_pgtable(void) {
    return (pte_t *)kernel_pt0;
}

/**
 * vm_get_kernel_pgtable - 获取内核页表基址（物理地址）
 *
 * 返回：内核页表物理地址（用于 TTBR1_EL1）
 */
uint64_t vm_get_kernel_pgtable(void) {
    return (uint64_t)kernel_pt0;
}

/**
 * vm_get_boot_pgtable - 获取启动页表基址（物理地址）
 *
 * 返回：启动页表物理地址（用于 TTBR0_EL1，低地址恒等映射）
 */
uint64_t vm_get_boot_pgtable(void) {
    return (uint64_t)boot_pt0;
}

/* ── 初始化 ─────────────────────────────────────────────────────── */

/**
 * vm_init - 初始化虚拟内存管理器
 *
 * 建立初始页表映射（使用两个独立页表）：
 * - TTBR0（低地址空间）：暂时全 0
 * - TTBR1（高地址空间）：
 *   - Entry[0]: 0x0000_0000 -> 0x0000_0000（设备内存，1GB）
 *   - Entry[1]: 0x4000_0000 -> 0x4000_0000（普通内存，1GB）
 *
 * 返回：0 表示成功，负值表示失败
 */
uint64_t vm_init(void) {
#ifdef PLATFORM_RK3588
    /*
     * platform_conf_scan() 在 kernel_main 中才运行，现在 dw_uart_base / reg_shift
     * 仍是编译期默认值（RISC-V QEMU：base=0x10000000, reg_shift=0）。
     * 在此直接覆写为 RK3588 物理值，使后续 KLOG_INFO 能在 MMU 启用前写到真实串口：
     *   reg_shift=2 → 寄存器步长 4 字节（LSR = base + 0x14 = 0xFEB50014）
     *   reg_shift=0 会导致 LSR 地址=0xFEB50005（错！THRE 永远读不到 → 死循环）
     */
    extern uintptr_t dw_uart_base;
    extern uint8_t   dw_uart_reg_shift;
    dw_uart_base      = 0xFEB50000UL;
    dw_uart_reg_shift = 2;
#endif
    KLOG_INFO("Initializing VM...\n");

    /* 清空所有页表 */
    memset(kernel_pt0, 0, sizeof(kernel_pt0));
    memset(kernel_pt1, 0, sizeof(kernel_pt1));
    memset(boot_pt0, 0, sizeof(boot_pt0));
    memset(boot_pt1, 0, sizeof(boot_pt1));

    /*
     * 设置 TTBR1 页表（高地址空间，内核使用）
     *
     * TTBR1 覆盖的虚拟地址范围：[0xffff000000000000, 0xffffffffffffffff]
     *
     * 页表结构：
     * - L0（PT0）：1 个 entry，指向 L1 页表
     * - L1（PT1）：512 个 entry，每个映射 1GB
     *
     * 映射规则：
     * - 访问虚拟地址 0xffff000000000000 + offset
     * - 页表索引计算使用 offset（去掉符号扩展）
     * - 例如：0xffff00000000400000 -> 索引 0x0000000000040000
     */

    /* L0 页表：entry[0] 指向 L1 页表 */
    set_table_entry(&kernel_pt0[0], kernel_pt1);

    /*
     * L1 页表：1GB 块映射，内容由平台决定
     *
     * QEMU virt:
     *   [0] 0x00000000-0x3FFFFFFF  设备内存 (UART@0x09000000, GIC@0x08000000)
     *   [1] 0x40000000-0x7FFFFFFF  普通内存 (QEMU RAM，内核在此)
     *   [2] 0x80000000-0xBFFFFFFF  普通内存 (QEMU 2GB 扩展)
     *
     * RK3588:
     *   [0] 0x00000000-0x3FFFFFFF  普通内存 (RAM，内核在 0x400000)
     *   [3] 0xC0000000-0xFFFFFFFF  设备内存 (UART@0xFEB50000, GIC@0xFE600000)
     */
#ifdef PLATFORM_RK3588
    set_block_entry(&kernel_pt1[0], 0x00000000ULL, PTE_NORMAL_MEMORY);  /* RAM Bank0 0x00000000-0x3FFFFFFF */
    set_block_entry(&kernel_pt1[1], 0x40000000ULL, PTE_NORMAL_MEMORY);  /* RAM Bank1 0x40000000-0x7FFFFFFF */
    set_block_entry(&kernel_pt1[2], 0x80000000ULL, PTE_NORMAL_MEMORY);  /* RAM Bank2 0x80000000-0xBFFFFFFF */
    set_block_entry(&kernel_pt1[3], 0xC0000000ULL, PTE_DEVICE_MEMORY);  /* MMIO      0xC0000000-0xFFFFFFFF */
#else
    set_block_entry(&kernel_pt1[0], 0x00000000ULL, PTE_DEVICE_MEMORY);  /* 设备内存（0x00000000 - 0x3fffffff） */
    set_block_entry(&kernel_pt1[1], 0x40000000ULL, PTE_NORMAL_MEMORY);  /* 普通内存（0x40000000 - 0x7fffffff） */
    set_block_entry(&kernel_pt1[2], 0x80000000ULL, PTE_NORMAL_MEMORY);  /* 普通内存（0x80000000 - 0xbfffffff） */
#endif

    /*
     * 设置 TTBR0 页表（低地址空间，恒等映射，MMU 开启后低地址代码继续可用）
     */
    set_table_entry(&boot_pt0[0], boot_pt1);

#ifdef PLATFORM_RK3588
    set_block_entry(&boot_pt1[0], 0x00000000ULL, PTE_NORMAL_MEMORY);  /* RAM Bank0 0x00000000-0x3FFFFFFF */
    set_block_entry(&boot_pt1[1], 0x40000000ULL, PTE_NORMAL_MEMORY);  /* RAM Bank1 0x40000000-0x7FFFFFFF */
    set_block_entry(&boot_pt1[2], 0x80000000ULL, PTE_NORMAL_MEMORY);  /* RAM Bank2 0x80000000-0xBFFFFFFF */
    set_block_entry(&boot_pt1[3], 0xC0000000ULL, PTE_DEVICE_MEMORY);  /* MMIO      0xC0000000-0xFFFFFFFF */
#else
    set_block_entry(&boot_pt1[0], 0x00000000ULL, PTE_DEVICE_MEMORY);  /* 设备内存（0x00000000 - 0x3fffffff） */
    set_block_entry(&boot_pt1[1], 0x40000000ULL, PTE_NORMAL_MEMORY);  /* 普通内存（0x40000000 - 0x7fffffff） */
    set_block_entry(&boot_pt1[2], 0x80000000ULL, PTE_NORMAL_MEMORY);  /* 普通内存（0x80000000 - 0xbfffffff） */
#endif

    KLOG_INFO("VM page tables initialized\n");
    KLOG_INFO("TTBR0 base: 0x%llx  TTBR1 base: 0x%llx\n",
              (uint64_t)boot_pt0, (uint64_t)kernel_pt0);

    return (uint64_t)kernel_pt0;
}