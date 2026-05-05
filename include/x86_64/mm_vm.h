#ifndef X86_64_MM_VM_H
#define X86_64_MM_VM_H

#include "types.h"
#include "mmu.h"
#include "pmm.h"
#include "string.h"

/* ══════════════════════════════════════════════════════════════════
 * x86_64 页表管理实现
 * ══════════════════════════════════════════════════════════════════
 *
 * 实现 4 级页表遍历和映射：
 *   PML4 → PDPT → PD → PT → 4KB page
 */

/* ── 辅助函数：物理地址转内核虚拟地址 ───────────────────────────── */
static inline void *x86_pa_to_kva(uint64_t pa)
{
    return phys_to_virt(pa);
}

/* ── 辅助函数：从 PTE 提取物理地址 ──────────────────────────────── */
static inline uint64_t x86_pte_to_pa(uint64_t pte)
{
    return pte & PTE_ADDR_MASK;
}

/* ── 辅助函数：创建表项 PTE（指向下一级页表）────────────────────── */
static inline uint64_t x86_make_table_pte(uint64_t pa)
{
    /* 表项需要：Present + Writable + User（允许用户访问下级） */
    return (pa & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
}

/* ── 辅助函数：检查是否是叶子页表项 ─────────────────────────────── */
static inline bool x86_pte_is_leaf(uint64_t pte, int level)
{
    /* PD/PDPT 级别：检查 PS 位（Huge Page） */
    if (level > 0 && (pte & PTE_HUGE))
        return true;
    /* PT 级别：始终是叶子 */
    if (level == 0)
        return (pte & PTE_PRESENT) != 0;
    return false;
}

/* ── 辅助函数：获取下一级页表地址 ───────────────────────────────── */
static inline uint64_t *x86_next_level(uint64_t pte)
{
    return (uint64_t *)x86_pa_to_kva(x86_pte_to_pa(pte));
}

/* ── 核心函数：页表遍历到 PT 级别 ────────────────────────────────── */
/**
 * x86_walk_pt - 遍历页表到最后一级（PT），返回 PTE 地址
 * @page_dir: PML4 基址（虚拟地址）
 * @vaddr: 要查询的虚拟地址
 * @alloc: 是否在中间页表缺失时自动分配
 *
 * 返回：PT 中对应的 PTE 地址（虚拟），失败返回 NULL
 */
static inline uint64_t *x86_walk_pt(void *page_dir, uint64_t vaddr, bool alloc)
{
    uint64_t *pml4 = (uint64_t *)page_dir;
    uint64_t idx_pml4 = GET_PML4_INDEX(vaddr);
    uint64_t idx_pdpt = GET_PDPT_INDEX(vaddr);
    uint64_t idx_pd   = GET_PD_INDEX(vaddr);
    uint64_t idx_pt   = GET_PT_INDEX(vaddr);

    /* Level 4: PML4 → PDPT */
    if ((pml4[idx_pml4] & PTE_PRESENT) == 0) {
        if (!alloc) return NULL;
        uint64_t pa = pmm_alloc_pages(g_pmm, 1);
        if (pa == 0) return NULL;
        memset(x86_pa_to_kva(pa), 0, PAGE_SIZE);
        pml4[idx_pml4] = x86_make_table_pte(pa);
    }
    if (x86_pte_is_leaf(pml4[idx_pml4], 3)) return NULL;  /* 不支持 1GB 大页 */

    /* Level 3: PDPT → PD */
    uint64_t *pdpt = x86_next_level(pml4[idx_pml4]);
    if ((pdpt[idx_pdpt] & PTE_PRESENT) == 0) {
        if (!alloc) return NULL;
        uint64_t pa = pmm_alloc_pages(g_pmm, 1);
        if (pa == 0) return NULL;
        memset(x86_pa_to_kva(pa), 0, PAGE_SIZE);
        pdpt[idx_pdpt] = x86_make_table_pte(pa);
    }
    if (x86_pte_is_leaf(pdpt[idx_pdpt], 2)) return NULL;  /* 不支持 1GB 大页 */

    /* Level 2: PD → PT */
    uint64_t *pd = x86_next_level(pdpt[idx_pdpt]);
    if ((pd[idx_pd] & PTE_PRESENT) == 0) {
        if (!alloc) return NULL;
        uint64_t pa = pmm_alloc_pages(g_pmm, 1);
        if (pa == 0) return NULL;
        memset(x86_pa_to_kva(pa), 0, PAGE_SIZE);
        pd[idx_pd] = x86_make_table_pte(pa);
    }
    if (x86_pte_is_leaf(pd[idx_pd], 1)) return NULL;  /* 不支持 2MB 大页 */

    /* Level 1: PT → Page */
    uint64_t *pt = x86_next_level(pd[idx_pd]);
    return &pt[idx_pt];
}

/* ── 辅助函数：权限转换为 PTE 标志 ────────────────────────────────── */
/**
 * x86_perm_to_flags - 将抽象权限转换为 x86_64 PTE 标志
 * @perm: 权限类型
 *   0: 用户页（RWX + USER）
 *   1: 内核页（RW + GLOBAL + NX）
 *   2: 设备页（RW + CACHE_DISABLE + NX）
 *
 * 返回：PTE 标志位组合
 */
static inline uint64_t x86_perm_to_flags(uint64_t perm)
{
    if (perm == 1) {
        /* 内核数据页：Present + Writable + Global + NX */
        return PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_NX | 
               PTE_ACCESSED | PTE_DIRTY;
    }
    if (perm == 2) {
        /* 设备页：Present + Writable + Cache Disable + NX */
        return PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX | 
               PTE_ACCESSED | PTE_DIRTY;
    }
    /* 默认用户页：Present + Writable + Executable + User */
    return PTE_PRESENT | PTE_WRITABLE | PTE_USER | 
           PTE_ACCESSED | PTE_DIRTY;
}

/* ══════════════════════════════════════════════════════════════════
 * 导出接口实现
 * ══════════════════════════════════════════════════════════════════ */

/**
 * mm_vm_map_pages - 映射虚拟地址到物理地址
 * @page_dir: PML4 页表基址（虚拟地址）
 * @vaddr: 起始虚拟地址
 * @paddr: 起始物理地址
 * @count: 页数
 * @perm: 权限（0=用户，1=内核，2=设备）
 *
 * 返回：0 成功，-1 失败
 */
static inline int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr,
                                      uint64_t paddr, int32_t count,
                                      uint64_t perm)
{
    uint64_t va = vaddr;
    uint64_t pa = paddr;
    uint64_t flags = x86_perm_to_flags(perm);

    for (int32_t i = 0; i < count; i++) {
        uint64_t *pte = x86_walk_pt(page_dir, va, true);
        if (pte == NULL) {
            return -1;
        }
        
        /* 检查是否已映射 */
        if ((*pte & PTE_PRESENT) != 0) {
            return -1;
        }
        
        /* 建立映射 */
        *pte = (pa & PTE_ADDR_MASK) | flags;
        
        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }

    return 0;
}

/**
 * mm_vm_get_paddr - 根据虚拟地址获取物理地址
 * @page_dir: PML4 页表基址（虚拟地址）
 * @vaddr: 虚拟地址
 *
 * 返回：物理地址，如果未映射返回 0
 */
static inline uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
{
    uint64_t *pte = x86_walk_pt(page_dir, vaddr, false);
    
    if (pte == NULL || (*pte & PTE_PRESENT) == 0) {
        return 0;
    }
    
    /* 提取物理页帧 + 页内偏移 */
    uint64_t pa = x86_pte_to_pa(*pte);
    uint64_t offset = GET_PAGE_OFFSET(vaddr);
    
    return pa + offset;
}

/**
 * mm_vm_copy_user_space - 复制用户地址空间
 * @dst_pgd: 目标 PML4（虚拟地址）
 * @src_pgd: 源 PML4（虚拟地址）
 *
 * 扫描用户地址空间 [0x1000, 0x80000000)，复制所有已映射页面
 * 返回：0 成功，-1 失败
 */
static inline int32_t mm_vm_copy_user_space(void *dst_pgd, void *src_pgd)
{
    /* 扫描常用用户区域 [0x1000, 0x80000000) */
    for (uint64_t va = 0x1000ULL; va < 0x80000000ULL; va += PAGE_SIZE) {
        uint64_t src_pa = mm_vm_get_paddr(src_pgd, va);
        if (src_pa == 0) {
            continue;  /* 未映射，跳过 */
        }
        
        /* 分配新物理页 */
        uint64_t dst_pa = pmm_alloc_pages(g_pmm, 1);
        if (dst_pa == 0) {
            return -1;
        }
        
        /* 复制页面内容 */
        memcpy(x86_pa_to_kva(dst_pa), 
               x86_pa_to_kva(src_pa & PAGE_MASK), 
               PAGE_SIZE);
        
        /* 在目标页表中建立映射（用户权限） */
        if (mm_vm_map_pages(dst_pgd, va, dst_pa, 1, 0) != 0) {
            pmm_free_pages(g_pmm, dst_pa, 1);
            return -1;
        }
    }
    
    return 0;
}

/**
 * mm_vm_copy_to_uva - 将数据拷贝到用户虚拟地址
 * @pgd: 用户页表 PML4（虚拟地址）
 * @user_vaddr: 目标用户虚拟地址
 * @src_paddr: 源物理地址
 * @size: 拷贝大小（字节）
 *
 * 支持跨页拷贝，逐页查找物理地址并拷贝
 */
static inline void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr,
                                     uint64_t src_paddr, uint64_t size)
{
    uint64_t src = src_paddr;
    uint64_t dst_va = user_vaddr;
    uint64_t left = size;

    while (left > 0) {
        /* 查找目标页的物理地址 */
        uint64_t dst_pa = mm_vm_get_paddr(pgd, dst_va);
        if (dst_pa == 0) {
            return;  /* 未映射，终止 */
        }
        
        /* 计算当前页可拷贝的字节数 */
        uint64_t page_offset = GET_PAGE_OFFSET(dst_va);
        uint64_t copy_size = PAGE_SIZE - page_offset;
        if (copy_size > left) {
            copy_size = left;
        }
        
        /* 拷贝数据 */
        memcpy(x86_pa_to_kva(dst_pa), x86_pa_to_kva(src), copy_size);
        
        /* 更新指针 */
        src += copy_size;
        dst_va += copy_size;
        left -= copy_size;
    }
}

#endif /* X86_64_MM_VM_H */
