/*
 * kernel/loader/elf_loader.c - ELF 程序加载器
 *
 * 支持加载静态链接的 ELF PIE 可执行文件
 */

#include "elf.h"
#include "loader/bin_loader.h"
#include "klog.h"
#include "task/task.h"
#include "task/switch.h"
#include "pmm.h"
#include "mm_vm.h"
#include "string.h"
#if ARCH_X86_64
#include "x86_64/mmu.h"
#endif
#include <ext4.h>
#include <ext4_types.h>

extern pmm_t *g_pmm;

#define USER_STACK_ADDR   0x70000000ULL
#define USER_STACK_SIZE   0x100000     /* 1MB */
#define MAX_FILE_SIZE     (10 * 1024 * 1024) /* 10MB */

/**
 * elf_load_segment - 加载单个 ELF 段到用户空间
 * @pgd: 页表基址
 * @phdr: 程序头
 * @file_data: ELF 文件数据
 *
 * 返回：0 成功，负值失败
 */
static int elf_load_segment(void *pgd, elf64_phdr_t *phdr, uint8_t *file_data)
{
    uint64_t vaddr = phdr->p_vaddr;
    uint64_t filesz = phdr->p_filesz;
    uint64_t memsz = phdr->p_memsz;
    uint64_t offset = phdr->p_offset;
    uint64_t seg_base_paddr;

    /* 对齐虚拟地址到页边界 */
    uint64_t vaddr_start = ALIGN_DOWN(vaddr, PAGE_SIZE);
    uint64_t vaddr_end = ALIGN_UP(vaddr + memsz, PAGE_SIZE);
    uint64_t total_pages = (vaddr_end - vaddr_start) / PAGE_SIZE;
    uint64_t page_idx = 0;

    KLOG_INFO("[elf] Loading segment:\n");
    KLOG_INFO("[elf]   vaddr: 0x%llx - 0x%llx\n", vaddr_start, vaddr_end);
    KLOG_INFO("[elf]   filesz: 0x%llx, memsz: 0x%llx\n", filesz, memsz);

    /* 计算权限 */
    uint64_t perm = 0;  /* 默认：用户可读写可执行 */

    /* 先一次性分配整段所需的连续物理页，避免每页分配导致的大量位图扫描 */
    seg_base_paddr = pmm_alloc_pages(g_pmm, (uint32_t)total_pages);
    if (seg_base_paddr == 0) {
        KLOG_ERROR("[elf] Failed to allocate %llu contiguous pages\n", total_pages);
        return -1;
    }

    /* 一次性映射整段 */
    if (mm_vm_map_pages(pgd, vaddr_start, seg_base_paddr, (int32_t)total_pages, perm) != 0) {
        KLOG_ERROR("[elf] Failed to map segment pages at 0x%llx (count=%llu)\n",
                   vaddr_start, total_pages);
        pmm_free_pages(g_pmm, seg_base_paddr, (uint32_t)total_pages);
        return -2;
    }

    /* 先整段清零，再一次性拷贝文件部分（更稳定也更快） */
    uint64_t seg_bytes = total_pages * PAGE_SIZE;
    memset(phys_to_virt(seg_base_paddr), 0, seg_bytes);

    if (filesz > 0) {
        uint64_t file_off_in_seg = vaddr - vaddr_start;
        uint8_t *dst = (uint8_t *)phys_to_virt(seg_base_paddr + file_off_in_seg);
        uint8_t *src = file_data + offset;
        memcpy(dst, src, filesz);
    }

    /* 保留进度日志风格 */
    for (uint64_t cur_vaddr = vaddr_start; cur_vaddr < vaddr_end; cur_vaddr += PAGE_SIZE) {
        if ((page_idx % 64) == 0) {
            KLOG_INFO("[elf]   progress: page %llu/%llu vaddr=0x%llx\n",
                      page_idx, total_pages, cur_vaddr);
        }
        page_idx++;
    }

    KLOG_INFO("[elf] Segment done: %llu pages mapped\n", total_pages);

    return 0;
}

static int elf_load_segment_with_base(void *pgd, elf64_phdr_t *phdr,
                                      uint8_t *file_data, uint64_t image_base)
{
    elf64_phdr_t adj = *phdr;
    adj.p_vaddr = phdr->p_vaddr + image_base;
    return elf_load_segment(pgd, &adj, file_data);
}

/**
 * elf_push_strings - 将字符串数组写入用户栈页，返回各字符串的用户虚拟地址
 * 从 page_top（即 USER_STACK_ADDR）向下写
 * page_base: 该页的内核虚拟地址（对应用户虚拟地址 page_top - PAGE_SIZE）
 * ptr: 当前写入位置（内核虚拟，初始指向 page_base + PAGE_SIZE）
 * 返回写入后的 ptr（向下移动了）
 */

/**
 * elf_setup_stack - 在用户栈最高页写入 Linux ABI 初始栈
 * @pgd:       用户页表（内核虚拟地址）
 * @stack_top: 用户栈顶（USER_STACK_ADDR）
 * @entry:     ELF 入口点
 * @argv:      来自 execve 的 argv（用户虚拟地址指针数组，可为 NULL）
 * @envp:      来自 execve 的 envp（用户虚拟地址指针数组，可为 NULL）
 * @out_sp:    输出：新的用户栈指针
 *
 * 构建布局（从高地址向低）：
 *   strings (argv[0..n], envp[0..m])
 *   8-byte aligned
 *   auxv: AT_PAGESZ, AT_ENTRY, AT_NULL
 *   envp ptrs + NULL
 *   argv ptrs + NULL
 *   argc
 *   <- new sp
 */
static int
elf_setup_stack(void *pgd, uint64_t stack_top, uint64_t entry,
                const char *pathname,
                char **argv, char **envp, uint64_t *out_sp,
                uint64_t phdr_uaddr, uint16_t phnum, uint16_t phent)
{
    /* 找到最高一个栈页的物理地址 */
    uint64_t top_page_uvaddr = stack_top - PAGE_SIZE;  /* 0x6ffff000 */
    uint64_t top_page_paddr  = mm_vm_get_paddr(pgd, top_page_uvaddr);
    if (top_page_paddr == 0) {
        KLOG_ERROR("[elf] elf_setup_stack: cannot get stack page paddr\n");
        return -1;
    }

    uint8_t *page = (uint8_t *)phys_to_virt(top_page_paddr);
    /* ptr 从页顶（= stack_top）向下 */
    uint8_t *ptr  = page + PAGE_SIZE;

    /* 内核虚拟地址 → 对应的用户虚拟地址 */
#define UADDR(p)  (top_page_uvaddr + (uint64_t)((p) - page))

    /* ── 默认 argv/envp（调用方未提供时使用）──────────────── */
    const char *default_arg0 = pathname ? pathname : "/init";
    const char *default_argv[] = { NULL, NULL };
    static const char *default_envp[] = {
        "PATH=/bin:/usr/bin:/", "HOME=/", "TERM=vt100", NULL
    };

    default_argv[0] = default_arg0;

    const char **av = (const char **)( argv ? (void *)argv : (void *)default_argv );
    const char **ev = (const char **)( envp ? (void *)envp : (void *)default_envp );

    /* ── 统计个数 ───────────────────────────────────────────── */
    int argc = 0;
    while (av[argc]) argc++;
    int envc = 0;
    while (ev[envc]) envc++;

    /* ── 写入字符串（从高地址向下），记录每个字符串的用户地址 */
    /* 最多 32 个 argv + envp，用局部数组存指针 */
#define MAX_ARGS 32
    uint64_t av_uaddr[MAX_ARGS];
    uint64_t ev_uaddr[MAX_ARGS];

    if (argc > MAX_ARGS - 1) argc = MAX_ARGS - 1;
    if (envc > MAX_ARGS - 1) envc = MAX_ARGS - 1;

    /* envp 字符串（逆序写，最后 ptr 值最小） */
    for (int i = envc - 1; i >= 0; i--) {
        uint64_t len = strlen(ev[i]) + 1;
        ptr -= len;
        memcpy(ptr, ev[i], len);
        ev_uaddr[i] = UADDR(ptr);
    }
    /* argv 字符串 */
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t len = strlen(av[i]) + 1;
        ptr -= len;
        memcpy(ptr, av[i], len);
        av_uaddr[i] = UADDR(ptr);
    }

    /* 为 auxv::AT_RANDOM 预留 16 字节随机区（glibc/musl 启动必需） */
    ptr -= 16;
    {
        uint64_t seed0 = 0x6d5a56da5a6d1234ULL ^ entry;
        uint64_t seed1 = 0xa55aa55a33cc77eeULL ^ stack_top;
        *(uint64_t *)(ptr + 0) = seed0;
        *(uint64_t *)(ptr + 8) = seed1;
    }
    uint64_t at_random_uaddr = UADDR(ptr);

    /* ── 8 字节对齐 ─────────────────────────────────────────── */
    ptr = (uint8_t *)((uint64_t)ptr & ~7ULL);

    /* ── auxv ───────────────────────────────────────────────── */
#define PUSH64(v) do { ptr -= 8; *(uint64_t *)ptr = (uint64_t)(v); } while(0)
    PUSH64(0);       /* AT_NULL value */
    PUSH64(0);       /* AT_NULL type  */
    PUSH64(at_random_uaddr); /* AT_RANDOM value */
    PUSH64(25);      /* AT_RANDOM type */
    PUSH64(4096);    /* AT_PAGESZ value */
    PUSH64(6);       /* AT_PAGESZ type  */
    PUSH64(entry);   /* AT_ENTRY value */
    PUSH64(9);       /* AT_ENTRY type  */
    if (phdr_uaddr) {
        PUSH64(phnum);   /* AT_PHNUM value */
        PUSH64(5);       /* AT_PHNUM type  */
        PUSH64(phent);   /* AT_PHENT value */
        PUSH64(4);       /* AT_PHENT type  */
        PUSH64(phdr_uaddr); /* AT_PHDR value */
        PUSH64(3);       /* AT_PHDR type  */
    }

    /* ── envp 指针数组（含 NULL 终止）──────────────────────── */
    PUSH64(0);
    for (int i = envc - 1; i >= 0; i--)
        PUSH64(ev_uaddr[i]);

    /* ── argv 指针数组（含 NULL 终止）──────────────────────── */
    PUSH64(0);
    for (int i = argc - 1; i >= 0; i--)
        PUSH64(av_uaddr[i]);

    /* ── argc ───────────────────────────────────────────────── */
    PUSH64(argc);

    *out_sp = UADDR(ptr);

    KLOG_INFO("[elf] Initial stack: sp=0x%llx, argc=%d, argv[0]=%s\n",
              *out_sp, argc, av[0]);
#if ARCH_RISCV64
    {
        uint64_t *stack_words = (uint64_t *)ptr;
        for (int i = 0; i < 24; i++) {
            KLOG_INFO("[elf]   stack[%02d] @0x%llx = 0x%llx\n",
                      i,
                      (uint64_t)(*out_sp + (uint64_t)i * 8),
                      stack_words[i]);
        }
    }
#endif

#undef UADDR
#undef PUSH64
#undef MAX_ARGS
    return 0;
}

/**
 * elf_load - 从内存加载 ELF 可执行文件
 * @file_data: ELF 文件数据（内核虚拟地址）
 * @file_size: 文件大小
 * @pathname: 程序路径名（用于任务命名）
 * @argv: 来自 execve 的 argv（可为 NULL）
 * @envp: 来自 execve 的 envp（可为 NULL）
 *
 * 返回：0 表示成功，负值表示错误
 */
static int elf_load(uint8_t *file_data, uint64_t file_size, const char *pathname,
                    char **argv, char **envp)
{
    elf64_ehdr_t *ehdr;
    elf64_phdr_t *phdr;
    task_t *new_task;
    uint64_t pgd_phys;
    void *pgd;
    uint64_t entry_point;
    uint64_t image_base = 0;
    uint64_t min_vaddr = (uint64_t)-1;
    uint64_t max_vaddr = 0;

    if (file_size < sizeof(elf64_ehdr_t)) {
        KLOG_ERROR("[elf] File too small\n");
        return -1;
    }

    ehdr = (elf64_ehdr_t *)file_data;

    /* 验证 ELF 头 */
    if (!elf_check_magic(ehdr)) {
        KLOG_ERROR("[elf] Invalid ELF magic\n");
        return -2;
    }

    if (!elf_check_class(ehdr)) {
        KLOG_ERROR("[elf] Not 64-bit ELF\n");
        return -3;
    }

    if (!elf_check_data(ehdr)) {
        KLOG_ERROR("[elf] Not little-endian\n");
        return -4;
    }

    if (!elf_check_machine(ehdr)) {
        KLOG_ERROR("[elf] Wrong machine type: %u\n", ehdr->e_machine);
        return -5;
    }

    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        KLOG_ERROR("[elf] Not executable (type=%u)\n", ehdr->e_type);
        return -6;
    }

    KLOG_INFO("[elf] Valid ELF: entry=0x%llx, phnum=%u\n",
              ehdr->e_entry, ehdr->e_phnum);

    entry_point = ehdr->e_entry;

    /* 验证程序头 */
    if (ehdr->e_phnum == 0 || ehdr->e_phentsize != sizeof(elf64_phdr_t)) {
        KLOG_ERROR("[elf] Invalid program header\n");
        return -7;
    }

    phdr = (elf64_phdr_t *)(file_data + ehdr->e_phoff);

    /* 创建用户页表 */
    pgd_phys = pmm_alloc_pages(g_pmm, 1);
    if (pgd_phys == 0) {
        KLOG_ERROR("[elf] Failed to allocate PGD\n");
        return -8;
    }

    pgd = phys_to_virt(pgd_phys);
    memset(pgd, 0, PAGE_SIZE);

#if ARCH_X86_64
    /* x86_64: 用户页表必须包含内核高半区映射，否则切换 CR3 后内核不可达 */
    {
        uint64_t kernel_cr3 = read_cr3();
        uint64_t kernel_pml4_phys = kernel_cr3 & 0x000FFFFFFFFFF000ULL;
        uint64_t *kernel_pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
        uint64_t *user_pml4 = (uint64_t *)pgd;

        for (int i = 256; i < 512; i++) {
            user_pml4[i] = kernel_pml4[i];
        }

        KLOG_INFO("[elf] x86_64 copied kernel PML4[256-511] to user PGD=0x%llx\n",
                  pgd_phys);
    }
#endif

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

    KLOG_INFO("[elf] RISC-V user pgd created: user_pa=0x%llx kernel_pa=0x%llx\n",
              pgd_phys, kernel_pgd_phys);
    KLOG_INFO("[elf] RISC-V kernel mappings copied: l1[0x100]=0x%llx l1[0x102]=0x%llx\n",
              user_l1[0x100], user_l1[0x102]);
    
    /* 验证L1[0x102]是否为叶子页表项（R/W/X至少一个为1） */
    uint64_t pte_102 = kernel_l1[0x102];
    int is_leaf = (pte_102 & 0xE) != 0;  /* R(bit1)|W(bit2)|X(bit3) */
    KLOG_INFO("[elf] Kernel L1[0x102] flags: V=%llu R=%llu W=%llu X=%llu U=%llu => %s\n",
              (pte_102 >> 0) & 1, (pte_102 >> 1) & 1, (pte_102 >> 2) & 1,
              (pte_102 >> 3) & 1, (pte_102 >> 4) & 1,
              is_leaf ? "LEAF (1GB page)" : "NON-LEAF (points to L2 table)");
    
    /* 调试：检查关键全局变量是否在映射范围内 */
    extern pmm_t *g_pmm;
    extern pmm_t pmm;
    KLOG_INFO("[elf] Checking globals: &g_pmm=%p &pmm=%p\n", &g_pmm, &pmm);
    uint64_t g_pmm_va = (uint64_t)&g_pmm;
    uint64_t pmm_va = (uint64_t)&pmm;
    KLOG_INFO("[elf]   g_pmm in L1[%llu], pmm in L1[%llu]\n",
              (g_pmm_va >> 30) & 0x1ff, (pmm_va >> 30) & 0x1ff);
    
    /* 调试：检查用户栈对应的 L1 表项是否为0 */
    KLOG_INFO("[elf] After pgd init: User L1[0]=0x%llx L1[1]=0x%llx L1[2]=0x%llx\n",
              user_l1[0], user_l1[1], user_l1[2]);
#endif

    /* ET_DYN 使用 PIE 基址 0x10000 */
    if (ehdr->e_type == ET_DYN) {
        image_base = 0x10000ULL;
        KLOG_INFO("[elf] ET_DYN image base: 0x%llx\n", image_base);
    }

    if (image_base != 0) {
        entry_point += image_base;
    }

    KLOG_INFO("[elf] Loading ELF segments...\n");

    /* 加载所有 PT_LOAD 段 */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            int rc = elf_load_segment_with_base(pgd, &phdr[i], file_data, image_base);
            if (rc < 0) {
                KLOG_ERROR("[elf] Failed to load segment %u\n", i);
                return -9;
            }

            if (phdr[i].p_vaddr + image_base < min_vaddr) {
                min_vaddr = phdr[i].p_vaddr + image_base;
            }
            if (phdr[i].p_vaddr + image_base + phdr[i].p_memsz > max_vaddr) {
                max_vaddr = phdr[i].p_vaddr + image_base + phdr[i].p_memsz;
            }
        }
    }

    KLOG_INFO("[elf] ELF loaded: vaddr 0x%llx - 0x%llx\n", min_vaddr, max_vaddr);
    KLOG_INFO("[elf] Entry point: 0x%llx\n", entry_point);

    /* ── 处理 RELA 重定位（PIE 需要）────────────────────────────── */
    KLOG_INFO("[elf] Processing RELA relocations...\n");
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            /* 找到动态段 */
            elf64_dyn_t *dyn = (elf64_dyn_t *)(file_data + phdr[i].p_offset);
            uint64_t dyn_count = phdr[i].p_filesz / sizeof(elf64_dyn_t);

            uint64_t rela_addr = 0;
            uint64_t rela_size = 0;
            uint64_t rela_ent = 0;

            /* 查找 RELA 重定位表 */
            for (uint64_t j = 0; j < dyn_count; j++) {
                if (dyn[j].d_tag == DT_RELA) {
                    rela_addr = dyn[j].d_un.d_ptr;
                } else if (dyn[j].d_tag == DT_RELASZ) {
                    rela_size = dyn[j].d_un.d_val;
                } else if (dyn[j].d_tag == DT_RELAENT) {
                    rela_ent = dyn[j].d_un.d_val;
                }
            }

            if (rela_addr && rela_size && rela_ent) {
                KLOG_INFO("[elf] Found RELA: addr=0x%llx, size=%llu, ent=%llu\n",
                          rela_addr, rela_size, rela_ent);

                /* 计算加载基址：
                 * - ET_EXEC（非 PIE）：load_bias = 0（使用绝对地址）
                 * - ET_DYN（PIE）：load_bias = 0（如果加载到 min_vaddr）
                 * 注意：对于静态链接的 PIE，所有地址都是相对于 min_vaddr 的
                 */
                uint64_t load_bias = 0;
                if (ehdr->e_type == ET_DYN) {
                    load_bias = image_base;
                }

                KLOG_INFO("[elf] ELF type=%u, min_vaddr=0x%llx, load_bias=0x%llx\n",
                          ehdr->e_type, min_vaddr, load_bias);

#if ARCH_AARCH64
                const uint32_t reloc_relative  = R_AARCH64_RELATIVE;
                const uint32_t reloc_jump_slot = R_AARCH64_JUMP_SLOT;
                const uint32_t reloc_glob_dat  = R_AARCH64_GLOB_DAT;
#elif ARCH_X86_64
                const uint32_t reloc_relative  = R_X86_64_RELATIVE;
                const uint32_t reloc_jump_slot = R_X86_64_JUMP_SLOT;
                const uint32_t reloc_glob_dat  = R_X86_64_GLOB_DAT;
#elif ARCH_RISCV64
                const uint32_t reloc_relative  = R_RISCV_RELATIVE;
                const uint32_t reloc_jump_slot = R_RISCV_JUMP_SLOT;
                const uint32_t reloc_glob_dat  = R_RISCV_GLOB_DAT;
#else
                const uint32_t reloc_relative  = 0xffffffffU;
                const uint32_t reloc_jump_slot = 0xffffffffU;
                const uint32_t reloc_glob_dat  = 0xffffffffU;
#endif

                /* 处理每个 RELA 条目 */
                uint64_t rela_count = rela_size / rela_ent;
                uint64_t unsupported_count = 0;
                for (uint64_t r = 0; r < rela_count; r++) {
                    /* RELA 在文件中的偏移 */
                    uint64_t rela_file_offset = 0;

                    /* 找到包含 RELA 的段 */
                    for (uint16_t s = 0; s < ehdr->e_phnum; s++) {
                        if (phdr[s].p_type == PT_LOAD) {
                            if (rela_addr >= phdr[s].p_vaddr &&
                                rela_addr < phdr[s].p_vaddr + phdr[s].p_filesz) {
                                rela_file_offset = rela_addr - phdr[s].p_vaddr + phdr[s].p_offset;
                                break;
                            }
                        }
                    }

                    if (rela_file_offset == 0) {
                        KLOG_WARN("[elf] Cannot find RELA in file segments\n");
                        continue;
                    }

                    elf64_rela_t *rela = (elf64_rela_t *)(file_data + rela_file_offset + r * rela_ent);
                    uint32_t r_type = ELF64_R_TYPE(rela->r_info);

                    /* 处理多种重定位类型 */
                    if (r_type == reloc_relative) {
                        /* RELATIVE: 基址 + addend */
                        uint64_t target_vaddr = load_bias + rela->r_offset;
                        uint64_t page_vaddr = ALIGN_DOWN(target_vaddr, PAGE_SIZE);
                        uint64_t paddr = mm_vm_get_paddr(pgd, page_vaddr);

                        if (paddr == 0) {
                            KLOG_ERROR("[elf] Cannot get paddr for 0x%llx\n", target_vaddr);
                            continue;
                        }

                        uint64_t new_value = load_bias + rela->r_addend;
                        uint64_t offset_in_page = target_vaddr - page_vaddr;
                        uint64_t *target = (uint64_t *)phys_to_virt(paddr + offset_in_page);

                        KLOG_TRACE("[elf] RELATIVE 0x%llx: 0x%llx -> 0x%llx\n",
                                  target_vaddr, *target, new_value);
                        *target = new_value;
                    } else if (r_type == reloc_jump_slot ||
                               r_type == reloc_glob_dat) {
                        /* JUMP_SLOT/GLOB_DAT: 函数地址
                         * 对于静态链接的 PIE，这些应该指向 load_bias + addend
                         * 但如果 addend 很小（比如 3），可能是编译器生成的占位符
                         */
                        uint64_t target_vaddr = load_bias + rela->r_offset;
                        uint64_t page_vaddr = ALIGN_DOWN(target_vaddr, PAGE_SIZE);
                        uint64_t paddr = mm_vm_get_paddr(pgd, page_vaddr);

                        if (paddr == 0) {
                            KLOG_ERROR("[elf] Cannot get paddr for 0x%llx\n", target_vaddr);
                            continue;
                        }

                        uint64_t new_value = load_bias + rela->r_addend;
                        uint64_t offset_in_page = target_vaddr - page_vaddr;
                        uint64_t *target = (uint64_t *)phys_to_virt(paddr + offset_in_page);

                        /* 如果 new_value 太小（< 0x10000），可能是错误的 */
                        if (new_value < 0x10000) {
                            KLOG_WARN("[elf] Suspicious %s at 0x%llx: addend=0x%llx, new_value=0x%llx\n",
                                      r_type == reloc_jump_slot ? "JUMP_SLOT" : "GLOB_DAT",
                                      target_vaddr, rela->r_addend, new_value);
                        }

                        KLOG_TRACE("[elf] %s 0x%llx: 0x%llx -> 0x%llx\n",
                                  r_type == reloc_jump_slot ? "JUMP_SLOT" : "GLOB_DAT",
                                  target_vaddr, *target, new_value);
                        *target = new_value;
                    } else if (r_type != 0) {
                        if (unsupported_count < 8) {
                            KLOG_WARN("[elf] Unsupported relocation type: %u at offset 0x%llx\n",
                                      r_type, rela->r_offset);
                        }
                        unsupported_count++;
                    }
                }

                if (unsupported_count > 8) {
                    KLOG_WARN("[elf] Unsupported relocations: total=%llu (showing first 8)\n",
                              unsupported_count);
                }

                KLOG_INFO("[elf] Processed %llu RELA relocations\n", rela_count);
            }
            break;
        }
    }

    /* 映射用户栈到 ELF 页表中（批量分配+批量映射） */
    uint64_t stack_bottom = ALIGN_DOWN(USER_STACK_ADDR - USER_STACK_SIZE, PAGE_SIZE);
    uint64_t stack_pages = (USER_STACK_ADDR - stack_bottom) / PAGE_SIZE;
    uint64_t stack_base_paddr = pmm_alloc_pages(g_pmm, (uint32_t)stack_pages);
    if (stack_base_paddr == 0) {
        KLOG_ERROR("[elf] Failed to allocate %llu stack pages\n", stack_pages);
        return -10;
    }
    
    KLOG_INFO("[elf] Stack pages allocated: phys=0x%llx - 0x%llx (%llu pages)\n",
              stack_base_paddr, stack_base_paddr + stack_pages * PAGE_SIZE, stack_pages);

    /* Linux 语义下新映射匿名页应为零页。先清零整段用户栈物理页。 */
    memset(phys_to_virt(stack_base_paddr), 0, stack_pages * PAGE_SIZE);
    
    /* 检查是否分配到了包含 g_pmm 的物理页 */
    {
        extern pmm_t *g_pmm;
        uint64_t g_pmm_check_va = (uint64_t)&g_pmm;
        uint64_t g_pmm_check_pa = g_pmm_check_va - KERNEL_VMA;
        if (g_pmm_check_pa >= stack_base_paddr && g_pmm_check_pa < stack_base_paddr + stack_pages * PAGE_SIZE) {
            KLOG_ERROR("[elf] ⚠️  CRITICAL BUG: Stack uses physical page containing g_pmm!\n");
            KLOG_ERROR("[elf]   g_pmm PA=0x%llx is within stack range [0x%llx, 0x%llx)\n",
                       g_pmm_check_pa, stack_base_paddr, stack_base_paddr + stack_pages * PAGE_SIZE);
        }
    }
    
    if (mm_vm_map_pages(pgd, stack_bottom, stack_base_paddr, (int32_t)stack_pages, 0) != 0) {
        KLOG_ERROR("[elf] Failed to map user stack: vaddr=0x%llx pages=%llu\n",
                   stack_bottom, stack_pages);
        pmm_free_pages(g_pmm, stack_base_paddr, (uint32_t)stack_pages);
        return -11;
    }
    KLOG_INFO("[elf] User stack mapped: 0x%llx - 0x%llx\n", stack_bottom, (uint64_t)USER_STACK_ADDR);

#if ARCH_RISCV64
    /* 调试：检查栈映射后 L1[1] 是否被正确设置 */
    {
        uint64_t *user_l1 = (uint64_t *)pgd;
        KLOG_INFO("[elf] After stack mapping: L1[1]=0x%llx\n", user_l1[1]);
        if (user_l1[1] != 0) {
            /* L1[1] 应该指向一个中间页表，解析PTE */
            uint64_t l1_ppn = (user_l1[1] >> 10) & 0xfffffffffff;
            uint64_t l1_next_table_pa = l1_ppn << 12;
            KLOG_INFO("[elf]   L1[1] points to next-level table at phys=0x%llx\n", l1_next_table_pa);
            
            /* 检查这个物理地址是否恰好是包含 g_pmm 的页 */
            extern pmm_t *g_pmm;
            extern pmm_t pmm;
            uint64_t g_pmm_va = (uint64_t)&g_pmm;
            uint64_t g_pmm_pa = g_pmm_va - KERNEL_VMA;
            uint64_t g_pmm_page = g_pmm_pa & ~0xfff;
            KLOG_INFO("[elf]   g_pmm variable at VA=0x%llx PA=0x%llx (page=0x%llx)\n",
                      g_pmm_va, g_pmm_pa, g_pmm_page);
            if (l1_next_table_pa == g_pmm_page) {
                KLOG_ERROR("[elf] ⚠️  BUG: L1[1] points to page containing g_pmm!\n");
            }
            
            /* 验证栈虚拟地址的实际物理映射 */
            uint64_t test_vaddr = 0x6ffff000;  /* 栈最后一页 */
            uint64_t mapped_paddr = mm_vm_get_paddr(pgd, test_vaddr);
            uint64_t expected_paddr = stack_base_paddr + (test_vaddr - stack_bottom);
            KLOG_INFO("[elf] Stack VA 0x%llx -> PA 0x%llx (expected 0x%llx)\n",
                      test_vaddr, mapped_paddr, expected_paddr);
            if (mapped_paddr != expected_paddr) {
                KLOG_ERROR("[elf] ⚠️  BUG: Stack mapping incorrect!\n");
            }
            if ((mapped_paddr & ~0xfff) == g_pmm_page) {
                KLOG_ERROR("[elf] ⚠️  CRITICAL: Stack page maps to g_pmm page!\n");
            }
        }
    }
#endif


    /* 在用户栈最高页构建 Linux ABI 初始栈（argc/argv/envp/auxv） */
    /* AT_PHDR：程序头的用户空间虚拟地址
     *   ET_DYN (PIE)：image_base + e_phoff（image_base=0x10000）
     *   ET_EXEC（非PIE）：从 PT_PHDR 段取 p_vaddr；若无则用第一个 PT_LOAD
     *     的 p_vaddr + e_phoff
     */
    uint64_t phdr_uaddr;
    if (ehdr->e_type == ET_DYN) {
        phdr_uaddr = image_base + ehdr->e_phoff;
    } else {
        /* 优先查找 PT_PHDR 段 */
        phdr_uaddr = 0;
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == 6 /* PT_PHDR */) {
                phdr_uaddr = phdr[i].p_vaddr;
                break;
            }
        }
        /* 没有 PT_PHDR：找第一个 PT_LOAD，加上文件内偏移 */
        if (phdr_uaddr == 0) {
            for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == 1 /* PT_LOAD */ && phdr[i].p_offset == 0) {
                    phdr_uaddr = phdr[i].p_vaddr + ehdr->e_phoff;
                    break;
                }
            }
        }
        /* 最后保底（不应走到这里） */
        if (phdr_uaddr == 0)
            phdr_uaddr = image_base + ehdr->e_phoff;
    }
    uint64_t user_sp = USER_STACK_ADDR;
    if (elf_setup_stack(pgd, USER_STACK_ADDR, entry_point, pathname,
                        argv, envp, &user_sp,
                        phdr_uaddr, ehdr->e_phnum, ehdr->e_phentsize) != 0) {
        KLOG_ERROR("[elf] Failed to setup initial stack\n");
        return -12;
    }

    /* 使用已建好的页表创建用户任务（跳过 vm_create_user_process） */
    task_t *current = task_current();

    uint64_t mmap_base = (image_base != 0) ? 0x50000000ULL : 0x30000000ULL;
    new_task = process_create_with_pgd(pathname, entry_point, user_sp, 10, pgd_phys,
                                       ALIGN_UP(max_vaddr, PAGE_SIZE), mmap_base);
    if (new_task == NULL) {
        KLOG_ERROR("[elf] Failed to create task\n");
        return -13;
    }
    /* new_task->parent_id 已被 process_create_with_pgd 设为 current->id，
     * 保持不变：让新进程成为当前进程的子进程，而非"祖父"进程的子进程。
     * 当前进程（execve 调用者）阻塞等待新进程退出后，再以相同退出码退出，
     * 这样祖父进程（ash）的 wait4 才能在 ls 真正完成后才返回。 */
    uint32_t new_task_id = new_task->id;

    KLOG_INFO("[elf] Process '%s' created, PID=%u, pgd=0x%llx, parent=%u\n",
              pathname, new_task->id, pgd_phys, new_task->parent_id);
    KLOG_INFO("[elf]   heap_start=0x%llx, mmap_base=0x%llx\n",
              new_task->heap_end, new_task->mmap_next);

    /* 阻塞当前进程，等待 execve 出的新进程退出 */
    current->is_waiting = true;
    current->wait_pid   = new_task_id;
    task_block(NULL);

    /* 新进程已退出，获取其退出状态并释放槽位，然后以相同状态退出 */
    {
        extern task_t  g_task_pool[];
        extern uint8_t g_stack_used[];
        for (uint32_t i = 0; i < TASK_MAX; i++) {
            if (!g_stack_used[i]) continue;
            if (g_task_pool[i].id == new_task_id) {
                current->exit_status = g_task_pool[i].exit_status;
                g_stack_used[i] = 0;
                g_task_pool[i].stack_base = NULL;
                break;
            }
        }
    }

    task_exit();

    return 0;
}

/**
 * elf_loader_load_from_file - 从文件系统加载并执行 ELF 程序
 */
int elf_loader_load_from_file(const char *pathname, char **argv, char **envp)
{
    ext4_file file;
    int rc;
    size_t fsize, rcnt;
    uint8_t *file_data;
    uint64_t file_size;
    char path_buf[256];
    uint32_t page_count;

    /* 复制路径名 */
    uint64_t i = 0;
    while (pathname[i] && i < sizeof(path_buf) - 1) {
        path_buf[i] = pathname[i];
        i++;
    }
    path_buf[i] = '\0';

    KLOG_INFO("[elf_loader] Loading: %s\n", path_buf);

    /* 打开文件 */
    rc = ext4_fopen(&file, path_buf, "r");
    if (rc != EOK) {
        KLOG_ERROR("[elf_loader] Failed to open '%s': %d\n", path_buf, rc);
        return -1;
    }

    /* 获取文件大小 */
    ext4_fseek(&file, 0, SEEK_END);
    fsize = ext4_ftell(&file);
    ext4_fseek(&file, 0, SEEK_SET);

    if (fsize == 0 || fsize > MAX_FILE_SIZE) {
        KLOG_ERROR("[elf_loader] Invalid file size: %zu\n", fsize);
        ext4_fclose(&file);
        return -2;
    }

    file_size = (uint64_t)fsize;

    /* 分配内存 */
    page_count = (file_size + 4095) / 4096;
    uint64_t file_phys = pmm_alloc_pages(g_pmm, page_count);
    if (file_phys == 0) {
        KLOG_ERROR("[elf_loader] Memory allocation failed\n");
        ext4_fclose(&file);
        return -3;
    }

    file_data = (uint8_t *)phys_to_virt(file_phys);

#if ARCH_RISCV64
    uint64_t satp_val;
    __asm__ volatile("csrr %0, satp" : "=r"(satp_val));
    KLOG_INFO("[elf_loader] file_phys=0x%llx file_data=0x%llx pages=%u satp=0x%llx\n",
              file_phys, (uint64_t)file_data, page_count, satp_val);
#endif

    /* 读取文件 */
    rc = ext4_fread(&file, file_data, file_size, &rcnt);
    if (rc != EOK || rcnt != file_size) {
        KLOG_ERROR("[elf_loader] Read failed: rc=%d\n", rc);
        pmm_free_pages(g_pmm, file_phys, page_count);
        ext4_fclose(&file);
        return -4;
    }

    ext4_fclose(&file);

    KLOG_INFO("[elf_loader] File loaded: %zu bytes\n", file_size);

    /* 加载 ELF */
    rc = elf_load(file_data, file_size, path_buf, argv, envp);

    /* 如果成功，不会到达这里 */
    return rc;
}
