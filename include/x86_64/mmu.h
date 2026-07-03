#ifndef X86_64_MMU_H
#define X86_64_MMU_H

#include "types.h"

/* ══════════════════════════════════════════════════════════════════
 * x86_64 页表结构（4 级页表）
 * ══════════════════════════════════════════════════════════════════
 *
 * 虚拟地址布局（48 位）：
 *   [47:39] PML4  (Page Map Level 4)      - 9 bits, 512 entries
 *   [38:30] PDPT  (Page Directory Pointer) - 9 bits, 512 entries
 *   [29:21] PD    (Page Directory)         - 9 bits, 512 entries
 *   [20:12] PT    (Page Table)             - 9 bits, 512 entries
 *   [11:0]  Offset                         - 12 bits
 *
 * 内核映射：
 *   内核虚拟地址：0xffff800000000000 - 0xffffffffffffffff (高 128 TB)
 *   用户虚拟地址：0x0000000000000000 - 0x00007fffffffffff (低 128 TB)
 */

/* ── 页表索引宏 ───────────────────────────────────────────────────── */

#define GET_PML4_INDEX(vaddr)  ((((uint64_t)(vaddr)) >> 39) & 0x1FF)
#define GET_PDPT_INDEX(vaddr)  ((((uint64_t)(vaddr)) >> 30) & 0x1FF)
#define GET_PD_INDEX(vaddr)    ((((uint64_t)(vaddr)) >> 21) & 0x1FF)
#define GET_PT_INDEX(vaddr)    ((((uint64_t)(vaddr)) >> 12) & 0x1FF)

/* 获取页内偏移 */
#define GET_PAGE_OFFSET(vaddr) ((uint64_t)(vaddr) & 0xFFF)

/* ── 页表项标志位 ───────────────────────────────────────────────────── */

/* 基本标志 */
#define PTE_PRESENT     (1ULL << 0)   /* Present (P) - 页面在内存中 */
#define PTE_WRITABLE    (1ULL << 1)   /* Read/Write (R/W) - 可写 */
#define PTE_USER        (1ULL << 2)   /* User/Supervisor (U/S) - 用户可访问 */
#define PTE_PWT         (1ULL << 3)   /* Page-level Write-Through */
#define PTE_PCD         (1ULL << 4)   /* Page-level Cache Disable */
#define PTE_ACCESSED    (1ULL << 5)   /* Accessed (A) - 页面被访问过 */
#define PTE_DIRTY       (1ULL << 6)   /* Dirty (D) - 页面被写入过 */
#define PTE_HUGE        (1ULL << 7)   /* Page Size (PS) - 大页（2MB/1GB） */
#define PTE_GLOBAL      (1ULL << 8)   /* Global (G) - 全局页（TLB 不刷新） */

/* 扩展标志（位 9-11 可供软件使用） */
#define PTE_SW_BIT1     (1ULL << 9)   /* Available for software */
#define PTE_SW_BIT2     (1ULL << 10)  /* Available for software */
#define PTE_SW_BIT3     (1ULL << 11)  /* Available for software */
#define PTE_NOFREE      PTE_SW_BIT1   /* Shared page: don't free on unmap */

/* NX（No eXecute）标志 - 位 63 */
#define PTE_NX          (1ULL << 63)  /* Execute Disable (XD/NX) */

/* 物理地址掩码（位 12-51，40 位物理地址） */
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/* ── 预定义页表项组合 ────────────────────────────────────────────── */

/* 内核页表项：存在 + 可写 + 全局 */
#define PTE_KERNEL      (PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL)

/* 内核代码：存在 + 只读 + 全局 */
#define PTE_KERNEL_RO   (PTE_PRESENT | PTE_GLOBAL)

/* 内核代码（可执行）：存在 + 只读 + 全局 */
#define PTE_KERNEL_CODE (PTE_PRESENT | PTE_GLOBAL)

/* 内核数据：存在 + 可写 + 全局 + 不可执行 */
#define PTE_KERNEL_DATA (PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_NX)

/* 用户页表项：存在 + 可写 + 用户 + 不可执行 */
#define PTE_USER_DATA   (PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)

/* 用户代码：存在 + 只读 + 用户 */
#define PTE_USER_CODE   (PTE_PRESENT | PTE_USER)

/* 用户代码（可执行，可写）：存在 + 可写 + 用户 */
#define PTE_USER_RWX    (PTE_PRESENT | PTE_WRITABLE | PTE_USER)

/* 用户栈：存在 + 可写 + 用户 + 不可执行 */
#define PTE_USER_STACK  (PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)

/* 设备内存：存在 + 可写 + 缓存禁用 + Write-Through + 不可执行 */
#define PTE_DEVICE      (PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX)

/* ── 页表项类型定义 ───────────────────────────────────────────────── */

/*
 * x86_64 页表项（64 位）
 * 使用 union 以支持不同的解释方式
 */
typedef union {
    struct {
        uint64_t present    : 1;   /* 0: Present */
        uint64_t writable   : 1;   /* 1: Read/Write */
        uint64_t user       : 1;   /* 2: User/Supervisor */
        uint64_t pwt        : 1;   /* 3: Page-level Write-Through */
        uint64_t pcd        : 1;   /* 4: Page-level Cache Disable */
        uint64_t accessed   : 1;   /* 5: Accessed */
        uint64_t dirty      : 1;   /* 6: Dirty (仅在最后一级) */
        uint64_t huge       : 1;   /* 7: Page Size (PS) / PAT */
        uint64_t global     : 1;   /* 8: Global */
        uint64_t sw_bits    : 3;   /* 9-11: Available for software */
        uint64_t phys_addr  : 40;  /* 12-51: Physical address (4KB aligned) */
        uint64_t sw_bits2   : 11;  /* 52-62: Available for software */
        uint64_t nx         : 1;   /* 63: No Execute */
    } bits;
    
    /* 表项（指向下一级页表） */
    struct {
        uint64_t present    : 1;
        uint64_t writable   : 1;
        uint64_t user       : 1;
        uint64_t pwt        : 1;
        uint64_t pcd        : 1;
        uint64_t accessed   : 1;
        uint64_t ignored1   : 1;
        uint64_t reserved   : 1;   /* Must be 0 for table entry */
        uint64_t ignored2   : 4;
        uint64_t next_table : 40;  /* 下一级页表的物理地址（PPN） */
        uint64_t ignored3   : 11;
        uint64_t nx         : 1;
    } table;
    
    /* 大页（2MB 在 PD，1GB 在 PDPT） */
    struct {
        uint64_t present    : 1;
        uint64_t writable   : 1;
        uint64_t user       : 1;
        uint64_t pwt        : 1;
        uint64_t pcd        : 1;
        uint64_t accessed   : 1;
        uint64_t dirty      : 1;
        uint64_t huge       : 1;   /* Must be 1 for huge page */
        uint64_t global     : 1;
        uint64_t sw_bits    : 3;
        uint64_t pat        : 1;   /* Page Attribute Table */
        uint64_t reserved1  : 8;   /* 2MB: bits 13-20 must be 0 */
        uint64_t phys_addr  : 31;  /* 2MB: bits 21-51 */
        uint64_t sw_bits2   : 11;
        uint64_t nx         : 1;
    } huge_2mb;
    
    /* 原始 64 位值 */
    uint64_t raw;
} pte_t;

/* ── 页表操作宏 ───────────────────────────────────────────────────── */

/* 从 PTE 提取物理地址（页帧号 → 物理地址） */
#define PTE_TO_PHYS(pte)   ((pte).bits.phys_addr << 12)
#define PTE_TO_PFN(pte)    ((pte).bits.phys_addr)

/* 从物理地址创建 PTE */
#define PHYS_TO_PTE(paddr, flags)  (((paddr) & PTE_ADDR_MASK) | (flags))

/* 检查 PTE 是否有效 */
#define PTE_IS_PRESENT(pte)  ((pte).bits.present)
#define PTE_IS_WRITABLE(pte) ((pte).bits.writable)
#define PTE_IS_USER(pte)     ((pte).bits.user)
#define PTE_IS_HUGE(pte)     ((pte).bits.huge)

/* ── PTE 工具函数（纯位运算，无外部依赖）──────────────────────── */

static inline uint64_t x86_pte_to_pa(uint64_t pte)
{
    return pte & PTE_ADDR_MASK;
}

static inline uint64_t x86_make_table_pte(uint64_t pa)
{
    return (pa & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
}

static inline bool x86_pte_is_leaf(uint64_t pte, int level)
{
    if (level > 0 && (pte & PTE_HUGE))
        return true;
    if (level == 0)
        return (pte & PTE_PRESENT) != 0;
    return false;
}

/* ── CR3 寄存器操作 ───────────────────────────────────────────────── */

/* 读取 CR3（当前页表基址） */
static inline uint64_t read_cr3(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

/* 写入 CR3（切换页表） */
static inline void write_cr3(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(value) : "memory");
}

/* 刷新 TLB */
static inline void flush_tlb(void)
{
    uint64_t cr3 = read_cr3();
    write_cr3(cr3);
}

/* 刷新单个虚拟地址的 TLB */
static inline void flush_tlb_single(uint64_t vaddr)
{
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/* ── 物理/虚拟地址转换 ───────────────────────────────────────────── */

/* 
 * 注意：phys_to_virt 和 virt_to_phys 已在 include/mm_vm.h 中统一定义
 * 此处不再重复定义以避免警告
 */

/* 内核虚拟地址偏移（用于参考） */
#define KERNEL_VMA_OFFSET  0xffff800000000000ULL

/* 检查是否是内核虚拟地址 */
#define is_kernel_vaddr(vaddr)  ((uint64_t)(vaddr) >= KERNEL_VMA_OFFSET)

/* ── 页对齐宏 ───────────────────────────────────────────────────── */

/* 
 * 注意：PAGE_SIZE、PAGE_SHIFT、PAGE_MASK 已在 include/mm_vm.h 中统一定义
 * 此处不再重复定义以避免警告
 */

/* 向下对齐到页边界 */
#define PAGE_ALIGN_DOWN(addr)  ((uint64_t)(addr) & PAGE_MASK)

/* 向上对齐到页边界 */
#define PAGE_ALIGN_UP(addr)    (((uint64_t)(addr) + PAGE_SIZE - 1) & PAGE_MASK)

/* 检查是否页对齐 */
#define IS_PAGE_ALIGNED(addr)  (((uint64_t)(addr) & ~PAGE_MASK) == 0)

/* ── 内核高半区 PML4 范围 ───────────────────────────────────────── */

/*
 * 内核高半区 PML4 起始索引，由 KERNEL_VMA_OFFSET 严格派生：
 *   KERNEL_VMA_OFFSET = 0xffff800000000000
 *   PML4_idx = (KERNEL_VMA_OFFSET >> 39) & 0x1ff = 0x100 = 256
 *
 * 用户页表必须包含 PML4[X86_PML4_KERNEL_START..511] 的内核映射，
 * 否则 SYSCALL / 中断时内核高半区不可达 → 三重错误 → 重启。
 */
#define X86_PML4_KERNEL_START  256U      /* (KERNEL_VMA_OFFSET >> 39) & 0x1ff */
#define X86_PML4_ENTRIES       512U

/**
 * x86_copy_kernel_mappings - 将内核高半区 PML4[256..511] 复制到用户页表
 * @user_pml4:   目标用户 PML4（内核虚拟地址，uint64_t[512]）
 * @kernel_pml4: 源内核 PML4（内核虚拟地址，uint64_t[512]）
 */
static inline void x86_copy_kernel_mappings(uint64_t *user_pml4,
                                            const uint64_t *kernel_pml4)
{
    for (unsigned int i = X86_PML4_KERNEL_START; i < X86_PML4_ENTRIES; i++)
        user_pml4[i] = kernel_pml4[i];
}

/* ── 函数声明 ───────────────────────────────────────────────────── */

/* MMU 初始化（在 mmu.S 中实现） */
extern void mmu_init(uint64_t pml4_phys);
extern void mmu_disable(void);

#endif /* X86_64_MMU_H */
