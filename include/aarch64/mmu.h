#ifndef MMU_H
#define MMU_H

#include "types.h"
#include "arch.h"

#include "mair.h"

/* ── 页表项标志位 ───────────────────────────────────────────────── */

#define PTE_VALID   (1ULL << 0)   /* 描述符有效 */
#define PTE_TABLE   (1ULL << 1)   /* 指向下一级页表（非块） */
#define PTE_BLOCK   (0ULL << 1)   /* 块描述符 */

/* 访问权限 */
#define PTE_AP_EL0  (1ULL << 6)   /* EL0 可访问 */
#define PTE_AP_RO   (1ULL << 7)   /* 只读 */
#define PTE_AP_RW   (0ULL << 7)   /* 读写 */

/* 共享性 */
#define PTE_SH_INNER   (3ULL << 8) /* Inner Shareable */

/* 其他标志 */
#define PTE_AF      (1ULL << 10)  /* Access Flag */
#define PTE_NG      (1ULL << 11)  /* Not Global */
#define PTE_PXN     (1ULL << 53)  /* Privileged Execute Never */
#define PTE_UXN     (1ULL << 54)  /* Unprivileged Execute Never */
#define PTE_NOFREE  (1ULL << 55)  /* SW: shared page, don't free on unmap */

/* 共享性定义 */
#define PTE_SH  (0b11 << 8)  /* Inner Shareable（SMP 系统） */

/* 默认页表项类型 */
#define MM_TYPE_BLOCK  0b01
#define MM_TYPE_TABLE  0b11

/* 预定义页表项 */
#define PTE_NORMAL_MEMORY \
    (MM_TYPE_BLOCK | PTE_AIDX_MEMORY | PTE_SH | PTE_AF | PTE_AP_RW)

#define PTE_DEVICE_MEMORY \
    (MM_TYPE_BLOCK | PTE_AIDX_DEVICE_nGnRn | PTE_SH | PTE_AF | PTE_AP_RW)


/* ── SCTLR_EL1（系统控制寄存器） ────────────────────────────────── */

#define SCTLR_EL1_M  (1 << 0)  /* MMU 使能 */
#define SCTLR_EL1_A  (1 << 1)  /* 对齐检查 */
#define SCTLR_EL1_C  (1 << 2)  /* 数据缓存使能 */
#define SCTLR_EL1_I  (1 << 12) /* 指令缓存使能 */
#define SCTLR_EL1_nAA (1 << 26) /* 不检查对齐 */

/* ── 页表索引宏 ───────────────────────────────────────────────────── */

/* 48 位虚拟地址：4 级页表 */
/* [47:39] PGD (L0) - 9 bits, 512 entries */
/* [38:30] PUD (L1) - 9 bits, 512 entries */
/* [29:21] PMD (L2) - 9 bits, 512 entries */
/* [20:12] PTE (L3) - 9 bits, 512 entries */
/* [11:0]  offset  - 12 bits */

#define GET_PGD_INDEX(vaddr) ((((uint64_t)(vaddr)) >> 39) & 0x1FF)
#define GET_PUD_INDEX(vaddr) ((((uint64_t)(vaddr)) >> 30) & 0x1FF)
#define GET_PMD_INDEX(vaddr) ((((uint64_t)(vaddr)) >> 21) & 0x1FF)
#define GET_PTE_INDEX(vaddr) ((((uint64_t)(vaddr)) >> 12) & 0x1FF)

/* ── 页表项结构 ───────────────────────────────────────────────────── */


typedef struct __packed
{
    /* 这些字段在所有类型的条目中都使用。 */
    unsigned long valid : 1; /* 有效映射 */
    unsigned long table : 1; /* 在4k映射条目中也等于1 */

    /* 这十个位仅在块条目中使用，在表条目中被忽略。 */
    unsigned long mattr : 4; /* 内存属性 */
    unsigned long read : 1;  /* 读访问 */
    unsigned long write : 1; /* 写访问 */
    unsigned long sh : 2;    /* 共享性 */
    unsigned long af : 1;    /* 访问标志 */
    unsigned long sbz4 : 1;  /* 必须为零 */

    /* 基地址必须对块条目进行适当对齐 */
    unsigned long base : 36; /* 块或下一级表的基地址 */
    unsigned long sbz3 : 4;  /* 必须为零 */

    /* 这七个位仅在块条目中使用，在表条目中被忽略。 */
    unsigned long contig : 1; /* 在16个连续条目中的块 */
    unsigned long sbz2 : 1;   /* 必须为零 */
    unsigned long xn : 1;     /* 不可执行 */
    unsigned long type : 4;   /* 硬件忽略。用于存储p2m类型 */

    unsigned long sbz1 : 5; /* 必须为零 */
} lpae_p2m_t;

typedef union {
    uint64_t   bits;
    lpae_p2m_t p2m;
} lpae_t;

typedef union {
    struct
    {
        unsigned long is_valid : 1, is_table : 1, ignored1 : 10, next_table_addr : 36, reserved : 4,
            ignored2 : 7,
            PXNTable : 1,  // Privileged Execute-never for next level
            XNTable : 1,   // Execute-never for next level
            APTable : 2,   // Access permissions for next level
            NSTable : 1;
    } table;
    struct
    {
        unsigned long is_valid : 1, is_table : 1,
            attr_index : 3,  // Memory attributes index
            NS : 1,          // Non-secure
            AP : 2,          // Data access permissions
            SH : 2,          // Shareability
            AF : 1,          // Accesss flag
            nG : 1,          // Not global bit
            reserved1 : 4, nT : 1, reserved2 : 13, pfn : 18, reserved3 : 2, GP : 1, reserved4 : 1,
            DBM : 1,  // Dirty bit modifier
            Contiguous : 1,
            PXN : 1,  // Privileged execute-never
            UXN : 1,  // Execute never
            soft_reserved : 4,
            PBHA : 4;  // Page based hardware attributes
    } l1_block;
    struct
    {
        unsigned long is_valid : 1, is_table : 1,
            attr_index : 3,  // Memory attributes index
            NS : 1,          // Non-secure
            AP : 2,          // Data access permissions
            SH : 2,          // Shareability
            AF : 1,          // Accesss flag
            nG : 1,          // Not global bit
            reserved1 : 4, nT : 1, reserved2 : 4, pfn : 27, reserved3 : 2, GP : 1, reserved4 : 1,
            DBM : 1,  // Dirty bit modifier
            Contiguous : 1,
            PXN : 1,  // Privileged execute-never
            UXN : 1,  // Execute never
            soft_reserved : 4,
            PBHA : 4;  // Page based hardware attributes
    } l2_block;
    struct
    {
        unsigned long is_valid : 1, is_table : 1,
            attr_index : 3,  // Memory attributes index
            NS : 1,          // Non-secure
            AP : 2,          // Data access permissions
            SH : 2,          // Shareability
            AF : 1,          // Accesss flag
            nG : 1,          // Not global bit
            pfn : 36, reserved : 3,
            DBM : 1,  // Dirty bit modifier
            Contiguous : 1,
            PXN : 1,  // Privileged execute-never
            UXN : 1,  // Execute never
            soft_reserved : 4,
            PBHA : 4,  // Page based hardware attributes
            ignored : 1;
    } l3_page;
    uint64_t pte;
} pte_t;

/* ── 函数声明 ───────────────────────────────────────────────────── */

/* MMU 初始化（在 mmu.S 中实现） */
extern void mmu_init(uint64_t ttbr0, uint64_t ttbr1);

#endif /* MMU_H */
