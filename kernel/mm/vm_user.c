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
#if ARCH_RISCV64
#include "riscv64/satp_utils.h"
#endif
#if ARCH_X86_64
#include "x86_64/mmu.h"
#endif

#if ARCH_AARCH64
extern void destroy_uvm_4level(void *page_dir);
#endif

#if ARCH_RISCV64
static void rv_destroy_table(uint64_t *table, int level, bool free_leaf_pages) {
  for (uint32_t i = 0; i < 512U; i++) {
    uint64_t pte = table[i];
    if ((pte & RV_PTE_V) == 0)
      continue;

    if (rv_pte_is_leaf(pte)) {
      if (free_leaf_pages && ((pte & RV_PTE_NOFREE) == 0))
        pmm_free_pages(g_pmm, rv_pte_to_pa(pte), 1);
      table[i] = 0;
      continue;
    }

    uint64_t child_pa = rv_pte_to_pa(pte);
    rv_destroy_table((uint64_t *)phys_to_virt(child_pa), level - 1, true);
    pmm_free_pages(g_pmm, child_pa, 1);
    table[i] = 0;
  }
}
#endif

#if ARCH_X86_64
static void x86_destroy_table(uint64_t *table, int level) {
  for (uint32_t i = 0; i < 512U; i++) {
    uint64_t pte = table[i];
    if ((pte & PTE_PRESENT) == 0)
      continue;

    if (level == 1 || (level > 1 && (pte & PTE_HUGE))) {
      if ((pte & PTE_NOFREE) == 0)
        pmm_free_pages(g_pmm, x86_pte_to_pa(pte), 1);
      table[i] = 0;
      continue;
    }

    uint64_t child_pa = x86_pte_to_pa(pte);
    x86_destroy_table((uint64_t *)phys_to_virt(child_pa), level - 1);
    pmm_free_pages(g_pmm, child_pa, 1);
    table[i] = 0;
  }
}
#endif

/* ── 创建用户进程页表 ──────────────────────────────────────────── */

uint64_t vm_create_user_process(uint64_t user_code_vaddr,
                                uint64_t user_code_size,
                                uint64_t user_stack_top,
                                uint64_t user_stack_size) {
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
   *   - L1[0x102]: KERNEL_VMA + 0x80000000..0xbfffffff (RAM
   * 高别名，含内核代码/数据)
   */
  uint64_t *kernel_l1 = (uint64_t *)phys_to_virt(satp_read_pgd_phys());
  uint64_t *user_l1 = (uint64_t *)pgd;
  riscv64_copy_kernel_mappings(user_l1, kernel_l1);
  KLOG_DEBUG("[vm_user] RISC-V kernel mappings: l1[0x%x]=0x%llx "
             "l1[0x%x]=0x%llx l1[0x%x]=0x%llx\n",
             RISCV64_KERNEL_L1_MMIO0_IDX, user_l1[RISCV64_KERNEL_L1_MMIO0_IDX],
             RISCV64_KERNEL_L1_MMIO1_IDX, user_l1[RISCV64_KERNEL_L1_MMIO1_IDX],
             RISCV64_KERNEL_L1_RAM_IDX, user_l1[RISCV64_KERNEL_L1_RAM_IDX]);
#endif

  KLOG_DEBUG("[vm_user] Creating user process page table:\n");
  KLOG_DEBUG("[vm_user]   code (kernel): 0x%llx - 0x%llx (size=0x%llx)\n",
             user_code_vaddr, user_code_vaddr + user_code_size, user_code_size);
  KLOG_DEBUG("[vm_user]   stack: 0x%llx - 0x%llx (size=0x%llx)\n",
             user_stack_top - user_stack_size, user_stack_top, user_stack_size);

  /* ── 映射用户代码段 ─────────────────────────────────────────── */
  uint64_t code_vaddr = ALIGN_UP(USER_CODE_BASE, PAGE_SIZE);
  uint64_t code_end = ALIGN_UP(USER_CODE_BASE + user_code_size, PAGE_SIZE);

  KLOG_DEBUG("[vm_user] Mapping user code: vaddr=0x%llx - 0x%llx\n", code_vaddr,
             code_end);

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
    KLOG_DEBUG("[vm_user]   code mapped: vaddr=0x%llx -> paddr=0x%llx\n", vaddr,
               new_paddr);
  }

  /* ── 映射用户栈 ─────────────────────────────────────────────── */
  {
    uint64_t stack_bottom =
        ALIGN_DOWN(user_stack_top - user_stack_size, PAGE_SIZE);
    KLOG_DEBUG("[vm_user] Mapping user stack: vaddr=0x%llx - 0x%llx\n",
               stack_bottom, user_stack_top);
    for (uint64_t vaddr = stack_bottom; vaddr < user_stack_top;
         vaddr += PAGE_SIZE) {
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
    KLOG_DEBUG("[vm_user] Copying user code: kern_paddr=0x%llx -> "
               "user_vaddr=0x10000\n",
               src_paddr);
    mm_vm_copy_to_uva(pgd, USER_CODE_BASE, src_paddr, user_code_size);

    /* 验证：读取前 16 字节 */
    uint64_t test_pa = mm_vm_get_paddr(pgd, USER_CODE_BASE);
    if (test_pa != 0) {
      uint8_t *code = (uint8_t *)phys_to_virt(test_pa);
      KLOG_DEBUG("[vm_user] User code at 0x%llx (PA=0x%llx): %02x %02x %02x "
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
  pmm_free_pages(g_pmm, pgd_phys, 1);
  return 0;
}

void vm_destroy_user_process(uint64_t pgd_phys) {
  if (pgd_phys == 0)
    return;
  KLOG_DEBUG("[vm_user] Destroying user process page table: PGD=0x%llx\n",
             pgd_phys);

#if ARCH_AARCH64
  destroy_uvm_4level(phys_to_virt(pgd_phys));
  return;
#elif ARCH_RISCV64
  uint64_t *root = (uint64_t *)phys_to_virt(pgd_phys);

  /* 只释放用户半区。高半区 L1[0x100..] 是从内核页表复制的共享映射。 */
  for (uint32_t i = 0; i < RISCV64_KERNEL_L1_MMIO0_IDX; i++) {
    uint64_t pte = root[i];
    if ((pte & RV_PTE_V) == 0)
      continue;
    if (rv_pte_is_leaf(pte)) {
      pmm_free_pages(g_pmm, rv_pte_to_pa(pte), 1);
      root[i] = 0;
      continue;
    }
    uint64_t child_pa = rv_pte_to_pa(pte);
    rv_destroy_table((uint64_t *)phys_to_virt(child_pa), 1, true);
    pmm_free_pages(g_pmm, child_pa, 1);
    root[i] = 0;
  }
#elif ARCH_X86_64
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pgd_phys);
  for (uint32_t i = 0; i < X86_PML4_KERNEL_START; i++) {
    uint64_t pte = pml4[i];
    if ((pte & PTE_PRESENT) == 0)
      continue;
    uint64_t child_pa = x86_pte_to_pa(pte);
    x86_destroy_table((uint64_t *)phys_to_virt(child_pa), 3);
    pmm_free_pages(g_pmm, child_pa, 1);
    pml4[i] = 0;
  }
#endif

  pmm_free_pages(g_pmm, pgd_phys, 1);
}

uint64_t vm_unmap_user_range(uint64_t pgd_phys, uint64_t vaddr, uint64_t size) {
  if (pgd_phys == 0 || size == 0)
    return 0;

  uint64_t start = ALIGN_DOWN(vaddr, PAGE_SIZE);
  uint64_t end = ALIGN_UP(vaddr + size, PAGE_SIZE);
  if (end < start)
    return 0;

  uint64_t freed = 0;
  void *pgd = phys_to_virt(pgd_phys);

  for (uint64_t va = start; va < end; va += PAGE_SIZE) {
#if ARCH_AARCH64
    if (mm_vm_get_paddr(pgd, va) != 0) {
      memory_free_page(pgd, va);
      freed++;
    }
#elif ARCH_RISCV64
    uint64_t *pte = rv_walk_l0_pte(pgd, va, false);
    if (pte && ((*pte & RV_PTE_V) != 0) && rv_pte_is_leaf(*pte)) {
      if ((*pte & RV_PTE_NOFREE) == 0)
        pmm_free_pages(g_pmm, rv_pte_to_pa(*pte), 1);
      *pte = 0;
      freed++;
    }
#elif ARCH_X86_64
    uint64_t *pte = x86_walk_pt(pgd, va, false);
    if (pte && ((*pte & PTE_PRESENT) != 0)) {
      if ((*pte & PTE_NOFREE) == 0)
        pmm_free_pages(g_pmm, x86_pte_to_pa(*pte), 1);
      *pte = 0;
      flush_tlb_single(va);
      freed++;
    }
#else
    (void)va;
#endif
  }

#if ARCH_RISCV64
  __asm__ volatile("sfence.vma" ::: "memory");
#elif ARCH_AARCH64
  __asm__ volatile("dsb ishst\n"
                   "tlbi vmalle1is\n"
                   "dsb ish\n"
                   "isb" ::
                       : "memory");
#endif

  return freed;
}
