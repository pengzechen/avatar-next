/*
 * kernel/mm/riscv64/vmm.c — RISC-V Sv39 页表操作实现
 *
 * 提供 Sv39 三级页表（L2→L1→L0）的遍历、映射与用户地址空间生命周期操作。
 */

#include "types.h"
#include "assert.h"
#include "klog.h"
#include "pmm.h"
#include "mm_vm.h"
#include "string.h"
#include "shared_page.h"
#include "riscv64/satp_utils.h"

/* ── 页表遍历 ───────────────────────────────────────────────────── */

uint64_t *rv_walk_l0_pte(void *page_dir, uint64_t vaddr, bool alloc)
{
    uint64_t *l2 = (uint64_t *)page_dir;
    uint64_t idx2 = RV_PTE_INDEX(vaddr, 2);
    uint64_t idx1 = RV_PTE_INDEX(vaddr, 1);
    uint64_t idx0 = RV_PTE_INDEX(vaddr, 0);

    if ((l2[idx2] & RV_PTE_V) == 0) {
        if (!alloc)
            return NULL;
        uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
        if (pa == 0)
            return NULL;
        memset(rv_pa_to_kva(pa), 0, PAGE_SIZE);
        l2[idx2] = rv_make_table_pte(pa);
    }
    if (rv_pte_is_leaf(l2[idx2]))
        return NULL;

    uint64_t *l1 = rv_next_level(l2[idx2]);
    if ((l1[idx1] & RV_PTE_V) == 0) {
        if (!alloc)
            return NULL;
        uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
        if (pa == 0)
            return NULL;
        memset(rv_pa_to_kva(pa), 0, PAGE_SIZE);
        l1[idx1] = rv_make_table_pte(pa);
    }
    if (rv_pte_is_leaf(l1[idx1]))
        return NULL;

    uint64_t *l0 = rv_next_level(l1[idx1]);
    return &l0[idx0];
}

/* ── 权限转换 ───────────────────────────────────────────────────── */

uint64_t rv_perm_to_flags(vm_perm_t perm)
{
    if (perm == VM_PERM_KERNEL) {
        return RV_PTE_R | RV_PTE_W | RV_PTE_X | RV_PTE_A | RV_PTE_D |
               RV_PTE_ATTR_NORMAL;
    }
    if (perm == VM_PERM_KERNEL_MMIO) {
        return RV_PTE_R | RV_PTE_W | RV_PTE_A | RV_PTE_D | RV_PTE_ATTR_IOREMAP;
    }
    return RV_PTE_R | RV_PTE_W | RV_PTE_X | RV_PTE_U | RV_PTE_A | RV_PTE_D |
           RV_PTE_ATTR_NORMAL;
}

/* ── PTE 查询 ───────────────────────────────────────────────────── */

uint64_t mm_vm_get_pte(void *page_dir, uint64_t vaddr)
{
    uint64_t *pte = rv_walk_l0_pte(page_dir, vaddr, false);
    if (pte == NULL)
        return 0;
    return *pte;
}

/* ── 页表映射 ───────────────────────────────────────────────────── */

int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr, uint64_t paddr,
                        int32_t count, vm_perm_t perm)
{
    uint64_t pa = paddr;
    uint64_t flags = rv_perm_to_flags(perm);

    for (int32_t i = 0; i < count; i++) {
        uint64_t *pte = rv_walk_l0_pte(page_dir, vaddr, true);
        if (pte == NULL)
            return -1;
        if ((*pte & RV_PTE_V) != 0)
            return -1;
        *pte = RV_PTE_FROM_PA(pa, flags | RV_PTE_V);
        vaddr += PAGE_SIZE;
        pa += PAGE_SIZE;
    }

    return 0;
}

/* ── 虚拟地址转物理地址 ─────────────────────────────────────────── */

uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
{
    uint64_t *pte = rv_walk_l0_pte(page_dir, vaddr, false);
    if (pte == NULL || ((*pte & RV_PTE_V) == 0) || !rv_pte_is_leaf(*pte))
        return 0;
    return rv_pte_to_pa(*pte) + (vaddr & (PAGE_SIZE - 1));
}

/* ── 数据拷贝到用户虚拟地址 ─────────────────────────────────────── */

void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr, uint64_t src_paddr,
                       uint64_t size)
{
    uint64_t src = src_paddr;
    uint64_t left = size;

    while (left > 0) {
        uint64_t dst_pa = mm_vm_get_paddr(pgd, user_vaddr);
        if (dst_pa == 0)
            return;

        uint64_t page_off = user_vaddr & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - page_off;
        if (chunk > left)
            chunk = left;

        memcpy(rv_pa_to_kva(dst_pa), rv_pa_to_kva(src), chunk);

        src += chunk;
        user_vaddr += chunk;
        left -= chunk;
    }
}

/* ── 用户地址空间生命周期（架构无关层经 mm_vm.h 调用）────────────── */

static void rv_destroy_table(uint64_t *table)
{
    for (uint32_t i = 0; i < RV_PT_ENTRIES; i++) {
        uint64_t pte = table[i];
        if ((pte & RV_PTE_V) == 0)
            continue;

        if (rv_pte_is_leaf(pte)) {
            if ((pte & RV_PTE_KEEP) != 0) {
                /* 设备缓冲：页归 dma_buf/ION 句柄所有，只拆表 */
            } else if ((pte & RV_PTE_NOFREE) == 0) {
                pmm_free_pages(&g_pmm, rv_pte_to_pa(pte), 1);
            } else {
                shared_page_unref(rv_pte_to_pa(pte));
            }
            table[i] = 0;
            continue;
        }

        uint64_t child_pa = rv_pte_to_pa(pte);
        rv_destroy_table(rv_next_level(pte));
        pmm_free_pages(&g_pmm, child_pa, 1);
        table[i] = 0;
    }
}

/*
 * 启动自检：mmu.S 按 RV_KERNEL_MAP_LIST 建立的高半区映射必须每一条都在。
 * 历史上漏掉 L1[0x103] 时内核自己跑得好好的，直到 syscall 期间
 * satp 指向用户页表、恰好在 0xc0000000 以上分配页面才 Store PF ——
 * 那时 pc 在 syscall 里，离根因非常远。这里在启动时就把它变成一条
 * 带文件名/行号的 ASSERT。
 */
void mm_vm_verify_kernel_mappings(void)
{
    uint64_t *kernel_l1 = (uint64_t *)phys_to_virt(satp_read_pgd_phys());

#define RV_CHECK_KERNEL_MAP(pa, attr)                                          \
    assert_always((kernel_l1[RV_KERNEL_HIGH_INDEX(pa)] & RV_PTE_V) != 0 &&     \
                  RV_PTE_IS_LEAF(kernel_l1[RV_KERNEL_HIGH_INDEX(pa)]));
    RV_KERNEL_MAP_LIST(RV_CHECK_KERNEL_MAP)
#undef RV_CHECK_KERNEL_MAP
}

void mm_vm_init_user_pgd(void *pgd, uint64_t pgd_phys)
{
    (void)pgd_phys;

    /*
     * 用户页表必须包含内核高半区映射，否则从 U 态陷入（ecall/中断/异常）
     * 时 stvec（高地址）不可达，会在陷入路径中失联。
     *
     * 复制哪些 L1 项由 RV_KERNEL_MAP_LIST（mem_layout.h）决定，与启动时
     * mmu.S 建立的映射是同一份列表，这里不再逐条列举下标。
     */
    uint64_t *kernel_l1 = (uint64_t *)phys_to_virt(satp_read_pgd_phys());
    uint64_t *user_l1 = (uint64_t *)pgd;
    riscv64_copy_kernel_mappings(user_l1, kernel_l1);
    KLOG_DEBUG("[vm_user] RISC-V kernel mappings copied at L1[0x%x..]\n",
               RV_KERNEL_L1_BASE_INDEX);
}

void mm_vm_destroy_user_space(uint64_t pgd_phys)
{
    if (pgd_phys == 0)
        return;

    uint64_t *root = (uint64_t *)phys_to_virt(pgd_phys);

    /* 只释放用户半区。高半区 L1[FIRST_IDX..] 是从内核页表复制的共享映射。 */
    for (uint32_t i = 0; i < RISCV64_KERNEL_L1_FIRST_IDX; i++) {
        uint64_t pte = root[i];
        if ((pte & RV_PTE_V) == 0)
            continue;
        if (rv_pte_is_leaf(pte)) {
            pmm_free_pages(&g_pmm, rv_pte_to_pa(pte), 1);
            root[i] = 0;
            continue;
        }
        uint64_t child_pa = rv_pte_to_pa(pte);
        rv_destroy_table((uint64_t *)phys_to_virt(child_pa));
        pmm_free_pages(&g_pmm, child_pa, 1);
        root[i] = 0;
    }

    pmm_free_pages(&g_pmm, pgd_phys, 1);
}

int mm_vm_unmap_page(void *pgd, uint64_t vaddr)
{
    uint64_t *pte = rv_walk_l0_pte(pgd, vaddr, false);
    if (!pte || ((*pte & RV_PTE_V) == 0) || !rv_pte_is_leaf(*pte))
        return 0;

    if ((*pte & RV_PTE_KEEP) != 0) {
        /* 设备缓冲：页归 dma_buf/ION 句柄所有，只拆映射 */
    } else if ((*pte & RV_PTE_NOFREE) == 0) {
        pmm_free_pages(&g_pmm, rv_pte_to_pa(*pte), 1);
    } else {
        shared_page_unref(rv_pte_to_pa(*pte));
    }
    *pte = 0;
    return 1;
}

void mm_vm_flush_tlb_user(void)
{
    __asm__ volatile("sfence.vma" ::: "memory");
}
