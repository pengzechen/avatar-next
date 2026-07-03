
#include "types.h"
#include "klog.h"
#include "assert.h"
#include "pmm.h"
#include "mm_vm.h"
#include "aarch64/mmu.h"
#include "string.h"


/* 兼容性定义（替换参考项目中的符号） */
#ifndef HEAP_OFFSET
#define HEAP_OFFSET  0
#endif

/* __heap_flag 的默认值（使用 __kernel_end 作为替代） */
extern char __kernel_end[];
extern char  __heap_flag[];

/* 设备内存范围定义（AArch64 QEMU virt） */
#define DEVICE_MEM_START 0x8000000
#define DEVICE_MEM_END   0xa000000

/* 页表常量 */
#define PAGE_TABLE_MAX_ENTRIES_L0  1
#define PAGE_TABLE_MAX_ENTRIES_L1  512
#define PAGE_TABLE_MAX_ENTRIES_L2  512
#define PAGE_TABLE_MAX_ENTRIES_L3  512

/* 兼容性宏 */
#define DOWN2(x, size)  ((x) & ~((size) - 1))  /* 向下对齐 */

/* 内联汇编函数 - 读取 TTBR0_EL1 寄存器 */
static inline uint64_t read_ttbr0_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(value));
    return value;
}

// ============= 用户程序内存分配释放 =================

pte_t *
find_pte(pte_t *page_dir, // 虚拟地址
         uint64_t vaddr,
         int32_t alloc) // 返回虚拟地址
{
    // KLOG_INFO("find_pte called for vaddr: 0x%llx\n", vaddr);

    // 获取PGD索引
    pte_t *pgd = &page_dir[GET_PGD_INDEX(vaddr)];
    // KLOG_INFO("    PGD Index: %d, PGD entry: 0x%llx\n", GET_PGD_INDEX(vaddr), pgd->pte);

    // 分配 PUD
    if (!pgd->table.is_valid)
    {
        if (!alloc)
            return NULL;
        // KLOG_INFO("    PGD entry is not valid, allocating PUD\n");
        uint64_t pud_phys = pmm_alloc_pages(g_pmm, 1);
        if (!pud_phys)
            return NULL;

        pgd->pte = (pud_phys >> 12) << 12 | 0x3; // valid + table
        // KLOG_INFO("    Allocated PUD at: 0x%llx, setting PGD entry to: 0x%llx\n", pud_phys, pgd->pte);
        memset(phys_to_virt(pud_phys), 0, 0x1000);
    }

    // 获取PUD表项
    pte_t *pud = (pte_t *)phys_to_virt((uint64_t)((pgd->table.next_table_addr) << 12ULL));
    // KLOG_INFO("    PUD entry: 0x%llx\n", pud->pte);

    pte_t *pud_entry = &pud[GET_PUD_INDEX(vaddr)];
    // KLOG_INFO("    PUD Index: %d, PUD entry: 0x%llx\n", GET_PUD_INDEX(vaddr), pud_entry->pte);

    // 分配 PMD
    if (!pud_entry->table.is_valid)
    {
        if (!alloc)
            return NULL;
        // KLOG_INFO("    PUD entry is not valid, allocating PMD\n");
        uint64_t pmd_phys = pmm_alloc_pages(g_pmm, 1);
        if (!pmd_phys)
            return NULL;

        pud_entry->pte = (pmd_phys >> 12) << 12 | 0x3;
        // KLOG_INFO("    Allocated PMD at: 0x%llx, setting PUD entry to: 0x%llx\n", pmd_phys, pud_entry->pte);
        memset(phys_to_virt(pmd_phys), 0, 0x1000);
    }

    // 获取PMD表项
    pte_t *pmd = (pte_t *)phys_to_virt((uint64_t)((pud_entry->table.next_table_addr) << 12ULL));
    // KLOG_INFO("    PMD entry: 0x%llx\n", pmd->pte);

    pte_t *pmd_entry = &pmd[GET_PMD_INDEX(vaddr)];
    // KLOG_INFO("    PMD Index: %d, PMD entry: 0x%llx\n", GET_PMD_INDEX(vaddr), pmd_entry->pte);

    // 分配 Page Table
    if (!pmd_entry->table.is_valid)
    {
        if (!alloc)
            return NULL;
        // KLOG_INFO("    PMD entry is not valid, allocating Page Table\n");
        uint64_t pt_phys = pmm_alloc_pages(g_pmm, 1);
        if (!pt_phys)
            return NULL;

        pmd_entry->pte = (pt_phys >> 12) << 12 | 0x3;
        // KLOG_INFO("    Allocated Page Table at: 0x%llx, setting PMD entry to: 0x%llx\n", pt_phys, pmd_entry->pte);
        memset(phys_to_virt(pt_phys), 0, 0x1000);
    }

    // 获取PTE表项
    pte_t *pte_base =
        (pte_t *)phys_to_virt((uint64_t)((pmd_entry->table.next_table_addr) << 12ULL));
    // KLOG_INFO("    PTE Index: %d, PTE entry: 0x%llx\n", GET_PTE_INDEX(vaddr), pte_base[GET_PTE_INDEX(vaddr)].pte);

    return &pte_base[GET_PTE_INDEX(vaddr)];
}

int32_t
memory_create_map(void *page_dir, uint64_t vaddr, uint64_t paddr, int32_t count, uint64_t perm)
{
    extern char __kernel_start[];
    extern char __kernel_end[];

    uint64_t start = (uint64_t)(void *)__kernel_start;
    uint64_t end = (uint64_t)(void *)__heap_flag + HEAP_OFFSET;
    // 如果 heap_start 不是页对齐的，将其向上对齐
    end = ALIGN_UP(end, PAGE_SIZE);
    // 这里start和end计算出来的都是物理地址
    if (start > KERNEL_VMA)
        start -= KERNEL_VMA;
    if (end > KERNEL_VMA)
        end -= KERNEL_VMA;
    // KLOG_DEBUG("=>Starting memory_create_map for vaddr 0x%llx, paddr 0x%llx, count %d\n",
    //        vaddr, paddr, count);

    for (int32_t i = 0; i < count; i++)
    {
        // 获取对应的 PTE
        pte_t *pte_entry = find_pte((pte_t *)page_dir, vaddr, 1);
        if (pte_entry == NULL)
        {
            KLOG_INFO("memory_create_map: Failed to find or allocate PTE for vaddr 0x%llx\n", vaddr);
            return -1;
        }

        if (pte_entry->l3_page.is_valid)
        {
            KLOG_INFO("memory_create_map: vaddr 0x%llx is already mapped to pfn 0x%llx\n",
                   vaddr,
                   pte_entry->l3_page.pfn);
            return -1;
        }

        // 打印分配信息
        // KLOG_INFO("Mapping vaddr 0x%llx to paddr 0x%llx\n", vaddr, paddr);    0(ng) 1(af) 11(sh) 00(ap) 0(ns) 000(attr) 01(table valid)  0x701

        // 设置 PTE 为有效并设置物理地址
        pte_entry->l3_page.is_valid = 1;
        pte_entry->l3_page.is_table = 1;
        pte_entry->l3_page.pfn = (paddr >> 12) & 0xFFFFFFFFF; // 36 bits PFN Page Frame Number

        // 设置权限
        if (perm == 0)
        {
            pte_entry->l3_page.AF = 1;
            pte_entry->l3_page.SH = 3; // Inner shareable
            pte_entry->l3_page.AP = 1;
            pte_entry->l3_page.UXN = 0;
            pte_entry->l3_page.PXN = 1;
            pte_entry->l3_page.attr_index = 1; // Normal memory
        }
        else if (perm == 1)
        {
            pte_entry->l3_page.AF = 1;
            pte_entry->l3_page.SH = 3; // Inner shareable
            pte_entry->l3_page.AP = 0;
            pte_entry->l3_page.UXN = 0;
            pte_entry->l3_page.PXN = 0;
            pte_entry->l3_page.attr_index = 1; // Normal memory
        }
        else if (perm == 2)
        {
            pte_entry->l3_page.AF = 1;
            pte_entry->l3_page.SH = 3; // Inner shareable
            pte_entry->l3_page.AP = 0;
            pte_entry->l3_page.UXN = 0;
            pte_entry->l3_page.PXN = 0;
            pte_entry->l3_page.attr_index = 0; // device memory
        }

        // 输出映射后的权限和地址信息
        // KLOG_INFO("Mapped PTE entry: is_valid=%d, pfn=0x%llx, AF=%d, SH=%d, AP=%d, UXN=%d, PXN=%d, attr_index=%d\n",
        //        pte_entry->l3_page.is_valid, pte_entry->l3_page.pfn, pte_entry->l3_page.AF, pte_entry->l3_page.SH,
        //        pte_entry->l3_page.AP, pte_entry->l3_page.UXN, pte_entry->l3_page.PXN, pte_entry->l3_page.attr_index);

        // 更新虚拟地址和物理地址
        vaddr += PAGE_SIZE;
        paddr += PAGE_SIZE;
    }

    return 0;
}

pte_t *
current_page_dir() // 返回物理地址
{
    return (pte_t *)read_ttbr0_el1();
}

uint64_t
memory_get_paddr(void *page_dir, uint64_t vaddr) // 返回物理地址
{
    pte_t *pte = find_pte((pte_t *)page_dir, vaddr, 0);

    if (pte == (pte_t *)0)
    {
        return 0;
    }

    return (pte->l3_page.pfn << 12) + (vaddr & (PAGE_SIZE - 1));
}

uint64_t
memory_get_pte_raw(void *page_dir, uint64_t vaddr)
{
    pte_t *pte = find_pte((pte_t *)page_dir, vaddr, 0);
    if (!pte) return 0;
    return pte->pte;
}

void
memory_set_pte_nofree(void *page_dir, uint64_t vaddr)
{
    pte_t *pte = find_pte((pte_t *)page_dir, vaddr, 0);
    if (pte) pte->pte |= PTE_NOFREE;
}

uint64_t
memory_alloc_page(void *page_dir, // 虚拟地址
                  uint64_t vaddr,
                  uint64_t size,
                  int32_t perm)
{
    uint64_t curr_vaddr = vaddr;
    int32_t page_count = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    vaddr = DOWN2(vaddr, PAGE_SIZE);

    // 逐页分配内存，然后建立映射关系
    for (int32_t i = 0; i < page_count; i++)
    {
        // 分配需要的内存
        uint64_t paddr = pmm_alloc_pages(g_pmm, 1);
        if (paddr == 0)
        {
            KLOG_INFO("mem alloc failed. no memory");
            return -1;
        }

        // 建立分配的内存与指定地址的关联
        int32_t err = memory_create_map((pte_t *)page_dir, curr_vaddr, paddr, 1, perm);
        if (err < 0)
        {
            KLOG_INFO("create memory map failed. err = %d", err);
            pmm_free_pages(g_pmm, vaddr, i + 1);
            return -1;
        }

        curr_vaddr += PAGE_SIZE;
    }

    return 0;
}

void memory_free_page(void *page_dir, uint64_t addr)
{
    pte_t *pte = find_pte((pte_t *)page_dir, addr, 0);
    if (!pte || ((pte->pte & PTE_VALID) == 0) || ((pte->pte & PTE_TABLE) == 0))
        return;

    if ((pte->pte & PTE_NOFREE) == 0)
        pmm_free_pages(g_pmm, (pte->l3_page.pfn << 12), 1);

    pte->pte = 0;
}

pte_t *
create_uvm(void)
{
    extern char __kernel_start[];
    extern char __kernel_end[];

    pte_t *page_dir = (pte_t *)phys_to_virt(pmm_alloc_pages(g_pmm, 1));
    if (page_dir == 0)
    {
        return 0;
    }
    memset((void *)page_dir, 0, PAGE_SIZE);

    uint64_t start = (uint64_t)(void *)__kernel_start;
    uint64_t end = (uint64_t)(void *)__heap_flag + 0x900000ULL;
    // 如果 heap_start 不是页对齐的，将其向上对齐
    end = ALIGN_UP(end, PAGE_SIZE);
    // 这里start和end计算出来的都是物理地址

    KLOG_INFO("map kernel start: 0x%llx, end: 0x%llx\n", start, end);
    if (start > KERNEL_VMA)
        start -= KERNEL_VMA;
    if (end > KERNEL_VMA)
        end -= KERNEL_VMA;

    // TODO: 这个地方需要让el0进程共享内核空间
    // TODO: 这个地方原理上并不需要映射，因为内核应该使用 FFFF_0000_0000_0000 之后的地址
    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE)
    {
        memory_create_map(page_dir, addr, addr, 1, 1); // 内核空间先恒等映射
    }
    KLOG_INFO("map device memory start: 0x%llx, end: 0x%llx\n", DEVICE_MEM_START, DEVICE_MEM_END);

    for (uint64_t addr = DEVICE_MEM_START; addr < DEVICE_MEM_END; addr += PAGE_SIZE)
    {
        memory_create_map(page_dir, addr, addr, 1, 2); // 设备内存恒等映射
    }
    return page_dir;
}

void _destroy_page_table_vm(pte_t *table, int32_t level)
{
    extern char __kernel_start[];

    if (level >= 4)
        return;

    static const int32_t max_entries[] = {PAGE_TABLE_MAX_ENTRIES_L0,
                                          PAGE_TABLE_MAX_ENTRIES_L1,
                                          PAGE_TABLE_MAX_ENTRIES_L2,
                                          PAGE_TABLE_MAX_ENTRIES_L3};
    int32_t entry_count = max_entries[level];

    for (int32_t i = 0; i < entry_count; i++)
    {
        pte_t *entry = &table[i];

        if ((entry->pte & PTE_VALID) == 0)
            continue;

        if (level == 3)
        {
            if ((entry->pte & PTE_TABLE) == 0)
                continue;

            uint64_t page_phys = entry->l3_page.pfn << 12;

            uint64_t start = (uint64_t)(void *)__kernel_start;
            uint64_t end = (uint64_t)(void *)__heap_flag + 0x900000ULL;
            end = ALIGN_UP(end, PAGE_SIZE);

            if (start > KERNEL_VMA)
                start -= KERNEL_VMA;
            if (end > KERNEL_VMA)
                end -= KERNEL_VMA;

            if (!((page_phys >= start && page_phys < end) ||
                  (page_phys >= DEVICE_MEM_START && page_phys < DEVICE_MEM_END)) &&
                !(entry->pte & PTE_NOFREE)) {
                pmm_free_pages(g_pmm, page_phys, 1);
            }

            entry->pte = 0;
            continue;
        }

        if ((entry->pte & PTE_TABLE) == 0)
            continue;

        uint64_t next_table_phys = entry->table.next_table_addr << 12;
        _destroy_page_table_vm((pte_t *)phys_to_virt(next_table_phys), level + 1);
        pmm_free_pages(g_pmm, next_table_phys, 1);
        entry->pte = 0;
    }
}

void _destroy_page_table(pte_t *table, int32_t level)
{
    // 输出当前正在处理的层级
    // KLOG_INFO("Destroying page table at level %d\n", level);

    if (level >= 3)
        return;

    // 各级页表的最大项数（按实际情况调整）
    static const int32_t max_entries[] = {PAGE_TABLE_MAX_ENTRIES_L0,
                                          8,
                                          PAGE_TABLE_MAX_ENTRIES_L2,
                                          PAGE_TABLE_MAX_ENTRIES_L3};
    int32_t entry_count = max_entries[level];

    // 遍历当前层级的所有页表项
    for (int32_t i = 0; i < entry_count; i++)
    {
        pte_t *entry = &table[i];

        if (!entry->table.is_valid)
            continue;

        uint64_t next_table_phys = entry->table.next_table_addr << 12;
        void *next_table = phys_to_virt(next_table_phys);

        // 输出当前页表项的信息
        // KLOG_INFO("Level %d, Entry %d: is_valid = %d, is_table = %d, Next Table Address = 0x%llx\n",
        //        level, i, entry->table.is_valid, entry->l3_page.is_table, next_table_phys);

        // 递归释放下一层页表
        _destroy_page_table((pte_t *)next_table, level + 1);

        // 释放当前这一级的页表页
        if (entry->l3_page.is_table == 1)
        {
            // KLOG_INFO("Level %d, Freeing page table at entry %d: 0x%llx\n", level, i, next_table_phys);
            pmm_free_pages(g_pmm, next_table_phys, 1);
        }
    }
}

bool _copy_page_table(pte_t *src_table, pte_t *dst_table, int32_t level)
{
    extern char __kernel_start[];
    extern char __kernel_end[];

    for (int32_t i = 0; i < 512; i++)
    {
        pte_t *src_entry = &src_table[i];
        if (!src_entry->table.is_valid)
            continue;

        if (level == 3)
        {
            // 第4级页表：实际映射的物理页
            uint64_t src_phys = src_entry->l3_page.pfn << 12;

            uint64_t start = (uint64_t)(void *)__kernel_start;
            uint64_t end = (uint64_t)(void *)__heap_flag + 0x900000ULL;
            end = ALIGN_UP(end, PAGE_SIZE);

            if (start > KERNEL_VMA)
                start -= KERNEL_VMA;
            if (end > KERNEL_VMA)
                end -= KERNEL_VMA;

            if ((src_phys >= start && src_phys <= end) ||
                (src_phys >= DEVICE_MEM_START && src_phys <= DEVICE_MEM_END) ||
                (src_entry->pte & PTE_NOFREE))
            {
                // 共享页或内核/设备页：复制 PTE 本身（共享物理页）
                dst_table[i].pte = src_entry->pte;
                continue;
            }

            uint64_t dst_phys = pmm_alloc_pages(g_pmm, 1);
            if (!dst_phys)
                return false;

            // 拷贝页内容
            memcpy(phys_to_virt(dst_phys), phys_to_virt(src_phys), PAGE_SIZE);

            // 设置目标页表项
            dst_table[i].pte = (dst_phys >> 12 << 12) | (src_entry->pte & 0xFFF);
        }
        else
        {
            // 中间层级：创建目标子表并递归拷贝
            uint64_t src_next_phys = src_entry->table.next_table_addr << 12;
            pte_t *src_next = (pte_t *)phys_to_virt(src_next_phys);

            uint64_t dst_next_phys = pmm_alloc_pages(g_pmm, 1);
            if (!dst_next_phys)
                return false;

            pte_t *dst_next = (pte_t *)phys_to_virt(dst_next_phys);
            memset(dst_next, 0, PAGE_SIZE);

            // 设置当前页表项指向新分配的页表
            dst_table[i].pte = (dst_next_phys >> 12 << 12) | (src_entry->pte & 0xFFF);
            // KLOG_INFO("copy structure, src_next_phys: 0x%llx, dest phys: 0x%llx, level: %d\n", src_next_phys, dst_next_phys, level);
            if (!_copy_page_table(src_next, dst_next, level + 1))
                return false;
        }
    }
    return true;
}

void destroy_uvm_4level(pte_t *page_dir)
{
    // level: 0 = PGD, 1 = PUD, 2 = PMD, 3 = PTE
    _destroy_page_table_vm(page_dir, 0);
    pmm_free_pages(g_pmm, virt_to_phys(page_dir), 1); // 最后释放 PGD 自身
}

void destory_4level(pte_t *page_dir)
{
    // level: 0 = PGD, 1 = PUD, 2 = PMD, 3 = PTE
    _destroy_page_table(page_dir, 0);
    pmm_free_pages(g_pmm, virt_to_phys(page_dir), 1); // 最后释放 PGD 自身
}

// 内核将数据拷贝到指定进程空间下
void copydata_to_uvm(void *page_dir, uint64_t vaddr, uint64_t paddr, uint64_t size)
{
    uint64_t offset = 0;
    while (offset < size)
    {
        uint64_t curr_vaddr = vaddr + offset;

        // 获取页表项
        pte_t *pte = find_pte((pte_t *)page_dir, curr_vaddr, 0);
        if (!pte || pte->l3_page.is_valid == 0)
        {
            // 页未映射，直接跳过或报错
            KLOG_INFO("No valid mapping for vaddr 0x%llx\n", curr_vaddr);
            return;
        }

        // 获取物理页帧号（PFN），并计算出物理地址
        uint64_t page_pfn = pte->l3_page.pfn;
        uint64_t page_paddr = page_pfn << 12;

        // 虚拟页起始地址（用于写入）
        uint8_t *dest = (uint8_t *)phys_to_virt(page_paddr);

        // 当前页剩余空间
        uint64_t page_offset = curr_vaddr & (PAGE_SIZE - 1);
        uint64_t page_remain = PAGE_SIZE - page_offset;
        uint64_t copy_len = (size - offset > page_remain) ? page_remain : (size - offset);

        // 源地址
        uint8_t *src = (uint8_t *)phys_to_virt(paddr + offset);

        // 拷贝数据
        memcpy(dest + page_offset, src, copy_len);

        offset += copy_len;
    }
}

// 复制某个进程空间的所有内存到另一个进程空间下
int32_t
memory_copy_uvm_4level(void *dst_pgd, void *src_pgd)
{
    if (!_copy_page_table((pte_t *)src_pgd, (pte_t *)dst_pgd, 0))
    {
        destroy_uvm_4level((pte_t *)dst_pgd);
        return -1;
    }

    return 0;
}

