/*
 * kernel/mm/vm_user.c - 用户进程虚拟内存管理
 *
 * 架构无关实现，依赖 mm_vm.h 提供的抽象接口。
 * 各架构在其 include/<arch>/mm_vm.h 中提供具体实现或 stub。
 */

#include "types.h"
#include "klog.h"
#include "pmm.h"
#include "mm_vm.h"
#include "string.h"
#include "mm_vm.h"
#include "vm_user.h"

/* ── 创建用户进程页表 ──────────────────────────────────────────── */

uint64_t
vm_create_user_process(uint64_t user_code_vaddr, uint64_t user_code_size,
                       uint64_t user_stack_top, uint64_t user_stack_size)
{
    /* 分配并清零页目录（PGD） */
    uint64_t pgd_phys = pmm_alloc_pages(g_pmm, 1);
    if (pgd_phys == 0) {
        KLOG_ERROR("[vm_user] Failed to allocate PGD\n");
        return 0;
    }
    memset(phys_to_virt(pgd_phys), 0, PAGE_SIZE);
    void *pgd = phys_to_virt(pgd_phys);

#if ARCH_RISCV64
    /*
     * RISC-V: 用户页表必须包含内核高半区映射，否则从 U 态陷入（ecall/中断/异常）
     * 时 stvec（高地址）不可达，会在陷入路径中失联。
     *
     * RISC-V Sv39 虚拟地址布局：
     *   - 内核空间起始地址 KERNEL_VMA = 0xffffffc000000000
     *   - VA[38:30] = L1 index = (0xffffffc000000000 >> 30) & 0x1ff = 0x100 = 256
     *   
     * 从当前内核根页表复制高半区映射（1GB 大页叶子）：
     *   - L1[0x100]: KERNEL_VMA + 0x00000000..0x3fffffff (MMIO 高别名)
     *   - L1[0x102]: KERNEL_VMA + 0x80000000..0xbfffffff (RAM 高别名，含内核代码/数据)
     */
    uint64_t satp_now;
    __asm__ volatile("csrr %0, satp" : "=r"(satp_now));
    uint64_t kernel_pgd_phys = (satp_now & 0x0fffffffffffULL) << 12;
    uint64_t *kernel_l1 = (uint64_t *)phys_to_virt(kernel_pgd_phys);
    uint64_t *user_l1   = (uint64_t *)pgd;

    /* 复制内核高半区L1页表项：L1[0x100]和L1[0x102] */
    user_l1[0x100] = kernel_l1[0x100];
    user_l1[0x102] = kernel_l1[0x102];

    KLOG_INFO("[vm_user] RISC-V kernel mappings copied: l1[0x100]=0x%llx l1[0x102]=0x%llx\n",
              user_l1[0x100], user_l1[0x102]);
#endif

    KLOG_INFO("[vm_user] Creating user process page table:\n");
    KLOG_INFO("[vm_user]   code (kernel): 0x%llx - 0x%llx (size=0x%llx)\n",
              user_code_vaddr, user_code_vaddr + user_code_size, user_code_size);
    KLOG_INFO("[vm_user]   stack: 0x%llx - 0x%llx (size=0x%llx)\n",
              user_stack_top - user_stack_size, user_stack_top, user_stack_size);

    /* ── 映射用户代码段 ─────────────────────────────────────────── */
    uint64_t code_vaddr = ALIGN_UP(0x10000ULL, PAGE_SIZE);
    uint64_t code_end   = ALIGN_UP(0x10000ULL + user_code_size, PAGE_SIZE);

    KLOG_INFO("[vm_user] Mapping user code: vaddr=0x%llx - 0x%llx\n",
              code_vaddr, code_end);

    for (uint64_t vaddr = code_vaddr; vaddr < code_end; vaddr += PAGE_SIZE) {
        uint64_t new_paddr = pmm_alloc_pages(g_pmm, 1);
        if (new_paddr == 0) {
            KLOG_ERROR("[vm_user] Failed to allocate code page\n");
            goto error;
        }
        if (mm_vm_map_pages(pgd, vaddr, new_paddr, 1, 0) != 0) {
            KLOG_ERROR("[vm_user] Failed to map user code at 0x%llx\n", vaddr);
            pmm_free_pages(g_pmm, new_paddr, 1);
            goto error;
        }
        KLOG_DEBUG("[vm_user]   code mapped: vaddr=0x%llx -> paddr=0x%llx\n", vaddr, new_paddr);
    }

    /* ── 映射用户栈 ─────────────────────────────────────────────── */
    {
        uint64_t stack_bottom = ALIGN_DOWN(user_stack_top - user_stack_size, PAGE_SIZE);
        KLOG_INFO("[vm_user] Mapping user stack: vaddr=0x%llx - 0x%llx\n",
                  stack_bottom, user_stack_top);
        for (uint64_t vaddr = stack_bottom; vaddr < user_stack_top; vaddr += PAGE_SIZE) {
            uint64_t stack_page = pmm_alloc_pages(g_pmm, 1);
            if (stack_page == 0) {
                KLOG_ERROR("[vm_user] Failed to allocate stack page\n");
                goto error;
            }
            if (mm_vm_map_pages(pgd, vaddr, stack_page, 1, 0) != 0) {
                KLOG_ERROR("[vm_user] Failed to map user stack at 0x%llx\n", vaddr);
                pmm_free_pages(g_pmm, stack_page, 1);
                goto error;
            }
        }
    }

    /* ── 拷贝用户代码到用户地址空间 ───────────────────────────────── */
    {
        uint64_t src_paddr = virt_to_phys(user_code_vaddr);
        KLOG_INFO("[vm_user] Copying user code: kern_paddr=0x%llx -> user_vaddr=0x10000\n",
                  src_paddr);
        mm_vm_copy_to_uva(pgd, 0x10000ULL, src_paddr, user_code_size);
    }

    KLOG_INFO("[vm_user] User process page table created: PGD=0x%llx\n", pgd_phys);
    return pgd_phys;

error:
    KLOG_ERROR("[vm_user] Failed to create user page table\n");
    pmm_free_pages(g_pmm, pgd_phys, 1);
    return 0;
}

/* ── 销毁用户进程页表 ──────────────────────────────────────────── */

void
vm_destroy_user_process(uint64_t pgd_phys)
{
    if (pgd_phys == 0)
        return;
    KLOG_INFO("[vm_user] Destroying user process page table: PGD=0x%llx\n", pgd_phys);
    /* TODO: 递归释放所有页表和映射的物理页 */
    pmm_free_pages(g_pmm, pgd_phys, 1);
}

