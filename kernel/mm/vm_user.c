/*
 * kernel/mm/vm_user.c - 用户进程虚拟内存管理
 *
 * 架构无关实现，依赖 mm_vm.h 提供的抽象接口。
 * 各架构在其 include/<arch>/mm_vm.h 中提供具体实现或 stub。
 */

#include "vm_user.h"
#include "klog.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "types.h"
#include "user_layout.h"

/* ── 创建用户进程页表 ──────────────────────────────────────────── */

uint64_t vm_create_user_process(uint64_t user_code_vaddr,
                                uint64_t user_code_size,
                                uint64_t user_stack_top,
                                uint64_t user_stack_size)
{
    /* 分配并清零页目录（PGD） */
    uint64_t pgd_phys = pmm_alloc_pages(&g_pmm, 1);
    if (pgd_phys == 0) {
        KLOG_ERROR("[vm_user] Failed to allocate PGD\n");
        return 0;
    }
    memset(phys_to_virt(pgd_phys), 0, PAGE_SIZE);
    void *pgd = phys_to_virt(pgd_phys);

    /*
     * 架构要求的内核映射注入（RISC-V/x86 复制内核高半区，AArch64 为空操作）。
     * 具体实现在 kernel/mm/<arch>/vmm.c，根目录不出现架构条件编译。
     */
    mm_vm_init_user_pgd(pgd, pgd_phys);

    KLOG_DEBUG("[vm_user] Creating user process page table:\n");
    KLOG_DEBUG("[vm_user]   code (kernel): 0x%llx - 0x%llx (size=0x%llx)\n",
               user_code_vaddr, user_code_vaddr + user_code_size,
               user_code_size);
    KLOG_DEBUG("[vm_user]   stack: 0x%llx - 0x%llx (size=0x%llx)\n",
               user_stack_top - user_stack_size, user_stack_top,
               user_stack_size);

    /* ── 映射用户代码段 ─────────────────────────────────────────── */
    uint64_t code_vaddr = ALIGN_UP(USER_CODE_BASE, PAGE_SIZE);
    uint64_t code_end = ALIGN_UP(USER_CODE_BASE + user_code_size, PAGE_SIZE);

    KLOG_DEBUG("[vm_user] Mapping user code: vaddr=0x%llx - 0x%llx\n",
               code_vaddr, code_end);

    for (uint64_t vaddr = code_vaddr; vaddr < code_end; vaddr += PAGE_SIZE) {
        uint64_t new_paddr = pmm_alloc_pages(&g_pmm, 1);
        if (new_paddr == 0) {
            KLOG_ERROR("[vm_user] Failed to allocate code page\n");
            goto error;
        }
        if (mm_vm_map_pages(pgd, vaddr, new_paddr, 1, 0) != 0) {
            KLOG_ERROR("[vm_user] Failed to map user code at 0x%llx\n", vaddr);
            pmm_free_pages(&g_pmm, new_paddr, 1);
            goto error;
        }
        KLOG_DEBUG("[vm_user]   code mapped: vaddr=0x%llx -> paddr=0x%llx\n",
                   vaddr, new_paddr);
    }

    /* ── 映射用户栈 ─────────────────────────────────────────────── */
    {
        uint64_t stack_bottom =
            ALIGN_DOWN(user_stack_top - user_stack_size, PAGE_SIZE);
        KLOG_DEBUG("[vm_user] Mapping user stack: vaddr=0x%llx - 0x%llx\n",
                   stack_bottom, user_stack_top);
        for (uint64_t vaddr = stack_bottom; vaddr < user_stack_top;
             vaddr += PAGE_SIZE) {
            uint64_t stack_page = pmm_alloc_pages(&g_pmm, 1);
            if (stack_page == 0) {
                KLOG_ERROR("[vm_user] Failed to allocate stack page\n");
                goto error;
            }
            if (mm_vm_map_pages(pgd, vaddr, stack_page, 1, 0) != 0) {
                KLOG_ERROR("[vm_user] Failed to map user stack at 0x%llx\n",
                           vaddr);
                pmm_free_pages(&g_pmm, stack_page, 1);
                goto error;
            }
        }
    }

    /* ── 拷贝用户代码到用户地址空间 ───────────────────────────────── */
    {
        uint64_t src_paddr = virt_to_phys(user_code_vaddr);
        KLOG_DEBUG("[vm_user] Copying user code: kern_paddr=0x%llx -> "
                   "user_vaddr=0x10000\n",
                   src_paddr);
        mm_vm_copy_to_uva(pgd, USER_CODE_BASE, src_paddr, user_code_size);

        /* 验证：读取前 16 字节 */
        uint64_t test_pa = mm_vm_get_paddr(pgd, USER_CODE_BASE);
        if (test_pa != 0) {
            uint8_t *code = (uint8_t *)phys_to_virt(test_pa);
            KLOG_DEBUG(
                "[vm_user] User code at 0x%llx (PA=0x%llx): %02x %02x %02x "
                "%02x %02x %02x %02x %02x\n",
                USER_CODE_BASE, test_pa, code[0], code[1], code[2], code[3],
                code[4], code[5], code[6], code[7]);
        }
    }

    KLOG_DEBUG("[vm_user] User process page table created: PGD=0x%llx\n",
               pgd_phys);
    return pgd_phys;

error:
    KLOG_ERROR("[vm_user] Failed to create user page table\n");
    /*
     * 失败时可能已经映射了代码/栈的一部分，甚至已经分配了中间页表。
     * 只 free PGD 会把这些页全部泄漏；走完整的销毁路径。
     */
    vm_destroy_user_process(pgd_phys);
    return 0;
}

void vm_destroy_user_process(uint64_t pgd_phys)
{
    if (pgd_phys == 0)
        return;
    KLOG_DEBUG("[vm_user] Destroying user process page table: PGD=0x%llx\n",
               pgd_phys);

    /* 整棵页表、leaf 页与共享引用都由架构钩子回收（含 PGD 自身）。 */
    mm_vm_destroy_user_space(pgd_phys);
}

uint64_t vm_unmap_user_range(uint64_t pgd_phys, uint64_t vaddr, uint64_t size)
{
    if (pgd_phys == 0 || size == 0)
        return 0;

    uint64_t start = ALIGN_DOWN(vaddr, PAGE_SIZE);
    uint64_t end = ALIGN_UP(vaddr + size, PAGE_SIZE);
    if (end < start)
        return 0;

    uint64_t freed = 0;
    void *pgd = phys_to_virt(pgd_phys);

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        if (mm_vm_unmap_page(pgd, va))
            freed++;
    }

    if (freed)
        mm_vm_flush_tlb_user();

    return freed;
}
