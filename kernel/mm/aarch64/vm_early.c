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

    /* L1 页表：使用 1GB 块映射 */
    set_block_entry(&kernel_pt1[0], 0x00000000ULL, PTE_DEVICE_MEMORY);  /* 设备内存（0x00000000 - 0x3fffffff） */
    set_block_entry(&kernel_pt1[1], 0x40000000ULL, PTE_NORMAL_MEMORY);  /* 普通内存（0x40000000 - 0x7fffffff） */
    set_block_entry(&kernel_pt1[2], 0x80000000ULL, PTE_NORMAL_MEMORY);  /* 普通内存（0x80000000 - 0xbfffffff，QEMU -m 2G 的上半部分） */

    KLOG_INFO("TTBR1 page table setup:\n");
    KLOG_INFO("  kernel_pt0[0] -> kernel_pt1: 0x%llx\n", kernel_pt0[0]);
    KLOG_INFO("  kernel_pt1[0] (device @ 0x00000000): 0x%llx\n", kernel_pt1[0]);
    KLOG_INFO("  kernel_pt1[1] (normal @ 0x40000000): 0x%llx\n", kernel_pt1[1]);
    KLOG_INFO("  kernel_pt1[2] (normal @ 0x80000000): 0x%llx\n", kernel_pt1[2]);

    /*
     * 验证地址映射：
     * - 内核代码 @ 0x40080000：
     *   - PUD 索引：(0x40080000 >> 30) & 0x1ff = 1
     *   - 查 kernel_pt1[1] -> 0x40000000
     *   - 最终物理地址：0x40000000 + 0x80000 = 0x40080000 ✓
     *
     * - UART @ 0x09000000：
     *   - PUD 索引：(0x09000000 >> 30) & 0x1ff = 0
     *   - 查 kernel_pt1[0] -> 0x00000000
     *   - 最终物理地址：0x00000000 + 0x09000000 = 0x09000000 ✓
     */
    KLOG_INFO("Address mapping verification:\n");
    KLOG_INFO("  Kernel code @ 0x40080000 -> PUD[1] -> 0x%llx\n",
              0x40000000ULL + (0x40080000ULL & 0x3fffffff));
    KLOG_INFO("  UART @ 0x09000000 -> PUD[0] -> 0x%llx\n",
              0x00000000ULL + (0x09000000ULL & 0x3fffffff));

    /*
     * 检查 UART 地址映射
     * UART 基地址：0x09000000
     * 虚拟地址：0xffff000000000000 + 0x09000000 = 0xffff0000000900000
     */
    KLOG_INFO("UART address check:\n");
    KLOG_INFO("  Physical: 0x09000000\n");
    KLOG_INFO("  Virtual: 0x%llx\n", KERNEL_VMA + 0x09000000ULL);

    /*
     * 设置 TTBR0 页表（低地址空间，用于开启MMU后的恒等映射）
     *
     * TTBR0 覆盖的虚拟地址范围：[0x0000000000000000, 0x0000ffffffffffff]
     *
     * 映射规则：
     * - 访问虚拟地址 0x00000000xxxxxx（低地址）
     * - 直接映射到物理地址 0x00000000xxxxxx（恒等映射）
     * - 这样开启MMU后，低地址代码可以继续运行
     */
    set_table_entry(&boot_pt0[0], boot_pt1);

    /* L1 页表：使用 1GB 块映射，建立恒等映射 */
    set_block_entry(&boot_pt1[0], 0x00000000ULL, PTE_DEVICE_MEMORY);  /* 设备内存（0x00000000 - 0x3fffffff） */
    set_block_entry(&boot_pt1[1], 0x40000000ULL, PTE_NORMAL_MEMORY);  /* 普通内存（0x40000000 - 0x7fffffff） */
    set_block_entry(&boot_pt1[2], 0x80000000ULL, PTE_NORMAL_MEMORY);  /* 普通内存（0x80000000 - 0xbfffffff，QEMU -m 2G 的上半部分） */

    KLOG_INFO("TTBR0 page table setup (identity mapping):\n");
    KLOG_INFO("  boot_pt0[0] -> boot_pt1: 0x%llx\n", boot_pt0[0]);
    KLOG_INFO("  boot_pt1[0] (device @ 0x00000000): 0x%llx\n", boot_pt1[0]);
    KLOG_INFO("  boot_pt1[1] (normal @ 0x40000000): 0x%llx\n", boot_pt1[1]);
    KLOG_INFO("  boot_pt1[2] (normal @ 0x80000000): 0x%llx\n", boot_pt1[2]);

    KLOG_INFO("VM initialized successfully\n");
    KLOG_INFO("TTBR0 page table base: 0x%llx (boot)\n", (uint64_t)boot_pt0);
    KLOG_INFO("TTBR1 page table base: 0x%llx (kernel)\n", (uint64_t)kernel_pt0);

    return (uint64_t)kernel_pt0;
}