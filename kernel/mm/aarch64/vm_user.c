/*
 * kernel/mm/aarch64/vm_user.c - 用户进程虚拟内存管理
 *
 * 为用户进程创建独立的地址空间
 */

#include "types.h"
#include "klog.h"
#include "assert.h"
#include "pmm.h"
#include "mmu.h"
#include "string.h"
#include "vmm.h"

/* 外部声明 */
extern int32_t memory_create_map(void *page_dir, uint64_t vaddr, uint64_t paddr, int32_t count, uint64_t perm);

/* 全局 PMM 指针 */
extern pmm_t *g_pmm;

/* ── 辅助函数 ───────────────────────────────────────────────────── */

/**
 * vm_copy_user_code - 将用户代码从内核空间拷贝到用户空间
 * @pgd: 用户页表基址（虚拟地址）
 * @user_code_vaddr: 用户代码在内核空间的虚拟地址
 * @user_code_size: 用户代码大小
 * @user_vaddr: 用户代码在用户空间的虚拟地址
 *
 * 返回：0 成功，-1 失败
 */
static int32_t
vm_copy_user_code(pte_t *pgd, uint64_t user_code_vaddr, uint64_t user_code_size, uint64_t user_vaddr)
{
    /* user_code_vaddr 是内核虚拟地址，需要转换为物理地址供 copydata_to_uvm 使用 */
    uint64_t user_code_paddr = virt_to_phys(user_code_vaddr);

    KLOG_INFO("[vm_user] Copying user code:\n");
    KLOG_INFO("[vm_user]   src (kernel): 0x%llx (phys: 0x%llx)\n", user_code_vaddr, user_code_paddr);
    KLOG_INFO("[vm_user]   dst (user):   0x%llx\n", user_vaddr);
    KLOG_INFO("[vm_user]   size: 0x%llx (%llu KB)\n", user_code_size, user_code_size / 1024);

    /* 使用 copydata_to_uvm 拷贝数据 */
    copydata_to_uvm(pgd, user_vaddr, user_code_paddr, user_code_size);

    KLOG_INFO("[vm_user] User code copied successfully\n");
    return 0;
}

/* ── 创建用户进程页表 ──────────────────────────────────────────── */

/**
 * vm_create_user_process - 为用户进程创建独立的地址空间
 * @user_code_vaddr: 用户代码段在内核空间的虚拟地址
 * @user_code_size: 用户代码段大小（字节）
 * @user_stack_top: 用户栈顶虚拟地址
 * @user_stack_size: 用户栈大小（字节）
 *
 * 返回：页表基址（物理地址），失败返回 0
 *
 * 功能：
 *   1. 创建用户页表
 *   2. 映射用户代码段到 0x10000
 *   3. 映射用户栈到 user_stack_top
 *   4. 将用户代码从内核空间拷贝到用户空间
 *
 * 地址空间布局：
 *   0x00010000 - 0x0001FFFF: 用户代码段（只读，可执行）
 *   0x70000000 - 0x700FFFFF: 用户栈（读写，不可执行）
 *   0x40000000 - 0x4FFFFFFF: 内核空间（共享，通过 TTBR1_EL1）
 */
uint64_t
vm_create_user_process(uint64_t user_code_vaddr, uint64_t user_code_size,
                       uint64_t user_stack_top, uint64_t user_stack_size)
{
    /* 分配页目录（PGD，L0） */
    uint64_t pgd_phys = pmm_alloc_pages(g_pmm, 1);
    if (pgd_phys == 0) {
        KLOG_ERROR("[vm_user] Failed to allocate PGD\n");
        return 0;
    }

    pte_t *pgd = (pte_t *)phys_to_virt(pgd_phys);
    memset(pgd, 0, PAGE_SIZE);

    KLOG_INFO("[vm_user] Creating user process page table:\n");
    KLOG_INFO("[vm_user]   code (kernel): 0x%llx - 0x%llx (size=0x%llx)\n",
              user_code_vaddr, user_code_vaddr + user_code_size, user_code_size);
    KLOG_INFO("[vm_user]   stack: 0x%llx - 0x%llx (size=0x%llx)\n",
              user_stack_top - user_stack_size, user_stack_top, user_stack_size);

    /* ── 映射用户代码段（只读，可执行）─────────────────────────── */
    /* user_code_vaddr 是内核虚拟地址，需要转换为物理地址 */
    uint64_t user_code_paddr = virt_to_phys(user_code_vaddr);
    uint64_t code_vaddr = 0x10000;  /* 用户代码段从 0x10000 开始 */
    uint64_t code_end = code_vaddr + user_code_size;

    /* 确保代码段按页对齐 */
    code_vaddr = ALIGN_UP(code_vaddr, PAGE_SIZE);
    code_end = ALIGN_UP(code_end, PAGE_SIZE);

    KLOG_INFO("[vm_user] Allocating and mapping user code:\n");
    KLOG_INFO("[vm_user]   vaddr: 0x%llx - 0x%llx\n", code_vaddr, code_end);
    KLOG_INFO("[vm_user]   source paddr: 0x%llx\n", user_code_paddr);

    /* 为用户代码分配新的物理页并映射 */
    for (uint64_t vaddr = code_vaddr; vaddr < code_end; vaddr += PAGE_SIZE) {
        /* 分配新的物理页 */
        uint64_t new_paddr = pmm_alloc_pages(g_pmm, 1);
        if (new_paddr == 0) {
            KLOG_ERROR("[vm_user] Failed to allocate code page\n");
            goto error;
        }

        /* 权限：用户可读+可执行（EL0）
         * memory_create_map perm=0: AF=1, AP=1(EL0 R/W), UXN=0(EL0可执行), PXN=1 */
        uint64_t perm = 0;

        if (memory_create_map(pgd, vaddr, new_paddr, 1, perm) != 0) {
            KLOG_ERROR("[vm_user] Failed to map user code at 0x%llx\n", vaddr);
            pmm_free_pages(g_pmm, new_paddr, 1);
            goto error;
        }

        KLOG_DEBUG("[vm_user]   Mapped: vaddr=0x%llx -> paddr=0x%llx\n", vaddr, new_paddr);
    }

    /* ── 映射用户栈（读写，不可执行）───────────────────────────────── */
    uint64_t stack_bottom = user_stack_top - user_stack_size;
    stack_bottom = ALIGN_DOWN(stack_bottom, PAGE_SIZE);

    KLOG_INFO("[vm_user] Mapping user stack: vaddr=0x%llx - 0x%llx\n",
              stack_bottom, user_stack_top);

    for (uint64_t vaddr = stack_bottom; vaddr < user_stack_top; vaddr += PAGE_SIZE) {
        /* 分配物理页作为栈空间 */
        uint64_t stack_page = pmm_alloc_pages(g_pmm, 1);
        if (stack_page == 0) {
            KLOG_ERROR("[vm_user] Failed to allocate stack page\n");
            goto error;
        }

        /* 权限：用户可读写（EL0）
         * memory_create_map perm=0: AF=1, AP=1(EL0 R/W), UXN=0 */
        uint64_t perm = 0;

        if (memory_create_map(pgd, vaddr, stack_page, 1, perm) != 0) {
            KLOG_ERROR("[vm_user] Failed to map user stack at 0x%llx\n", vaddr);
            pmm_free_pages(g_pmm, stack_page, 1);
            goto error;
        }
    }

    /* ── 拷贝用户代码到用户空间 ────────────────────────────────── */
    uint64_t user_vaddr = 0x10000;  /* 用户代码在用户空间的虚拟地址 */
    if (vm_copy_user_code(pgd, user_code_vaddr, user_code_size, user_vaddr) != 0) {
        KLOG_ERROR("[vm_user] Failed to copy user code\n");
        goto error;
    }

    KLOG_INFO("[vm_user] User process page table created: PGD=0x%llx\n", pgd_phys);

    return pgd_phys;

error:
    /* TODO: 清理已分配的资源 */
    KLOG_ERROR("[vm_user] Failed to create user page table\n");
    return 0;
}

/**
 * vm_destroy_user_process - 销毁用户进程页表
 * @pgd_phys: 页表基址（物理地址）
 */
void
vm_destroy_user_process(uint64_t pgd_phys)
{
    if (pgd_phys == 0) {
        return;
    }

    KLOG_INFO("[vm_user] Destroying user process page table: PGD=0x%llx\n", pgd_phys);

    /* TODO: 递归释放所有页表和映射的物理页 */
    /* 当前简化实现：只释放 PGD */
    pmm_free_pages(g_pmm, pgd_phys, 1);
}
