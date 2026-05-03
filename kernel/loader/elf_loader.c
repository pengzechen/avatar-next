/*
 * kernel/loader/elf_loader.c - ELF 程序加载器
 *
 * 支持加载静态链接的 ELF PIE 可执行文件
 */

#include "elf.h"
#include "loader/bin_loader.h"
#include "klog.h"
#include "task/task.h"
#include "pmm.h"
#include "mm_vm.h"
#include "string.h"
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
    uint64_t paddr;

    /* 对齐虚拟地址到页边界 */
    uint64_t vaddr_start = ALIGN_DOWN(vaddr, PAGE_SIZE);
    uint64_t vaddr_end = ALIGN_UP(vaddr + memsz, PAGE_SIZE);

    KLOG_INFO("[elf] Loading segment:\n");
    KLOG_INFO("[elf]   vaddr: 0x%llx - 0x%llx\n", vaddr_start, vaddr_end);
    KLOG_INFO("[elf]   filesz: 0x%llx, memsz: 0x%llx\n", filesz, memsz);

    /* 分配并映射物理页 */
    for (uint64_t cur_vaddr = vaddr_start; cur_vaddr < vaddr_end; cur_vaddr += PAGE_SIZE) {
        paddr = pmm_alloc_pages(g_pmm, 1);
        if (paddr == 0) {
            KLOG_ERROR("[elf] Failed to allocate page\n");
            return -1;
        }

        /* 计算权限 */
        uint64_t perm = 0;  /* 默认：用户可读写可执行 */

        /* 映射页表 */
        if (mm_vm_map_pages(pgd, cur_vaddr, paddr, 1, perm) != 0) {
            KLOG_ERROR("[elf] Failed to map page at 0x%llx\n", cur_vaddr);
            pmm_free_pages(g_pmm, paddr, 1);
            return -2;
        }

        /* 将整页清零（保证页内填充字节和 BSS 都是 0）*/
        memset(phys_to_virt(paddr), 0, PAGE_SIZE);

        /* 拷贝文件内容到物理页
         *
         * copy_start : 本页内，segment 数据起始偏移（segment 未跨页起始时为 0）
         * src_file_offset : 本页对应的文件偏移
         *   - cur_vaddr < vaddr : segment 从本页中部开始，src 从 p_offset 开始
         *   - cur_vaddr >= vaddr: segment 已覆盖本页起始，src 偏移 = p_offset + (cur_vaddr - vaddr)
         */
        uint64_t copy_start = (cur_vaddr < vaddr) ? (vaddr - cur_vaddr) : 0;
        uint64_t file_remaining = (vaddr + filesz > cur_vaddr) ?
                                  MIN(vaddr + filesz - cur_vaddr, PAGE_SIZE) : 0;

        if (file_remaining > 0) {
            uint64_t seg_byte_offset = (cur_vaddr >= vaddr) ? (cur_vaddr - vaddr) : 0;
            uint8_t *dst = (uint8_t *)phys_to_virt(paddr + copy_start);
            uint8_t *src = file_data + offset + seg_byte_offset;
            uint64_t copy_size = file_remaining - copy_start;

            for (uint64_t i = 0; i < copy_size; i++) {
                dst[i] = src[i];
            }
        }
    }

    return 0;
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
    static const char *default_argv[] = { "/busybox", "sh", NULL };
    static const char *default_envp[] = {
        "PATH=/bin:/usr/bin:/", "HOME=/", "TERM=vt100", NULL
    };

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

    /* ── 8 字节对齐 ─────────────────────────────────────────── */
    ptr = (uint8_t *)((uint64_t)ptr & ~7ULL);

    /* ── auxv ───────────────────────────────────────────────── */
#define PUSH64(v) do { ptr -= 8; *(uint64_t *)ptr = (uint64_t)(v); } while(0)
    PUSH64(0);       /* AT_NULL value */
    PUSH64(0);       /* AT_NULL type  */
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

    KLOG_INFO("[elf] Loading ELF segments...\n");

    /* 加载所有 PT_LOAD 段 */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            int rc = elf_load_segment(pgd, &phdr[i], file_data);
            if (rc < 0) {
                KLOG_ERROR("[elf] Failed to load segment %u\n", i);
                return -9;
            }

            if (phdr[i].p_vaddr < min_vaddr) {
                min_vaddr = phdr[i].p_vaddr;
            }
            if (phdr[i].p_vaddr + phdr[i].p_memsz > max_vaddr) {
                max_vaddr = phdr[i].p_vaddr + phdr[i].p_memsz;
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
                if (ehdr->e_type == ET_DYN && min_vaddr != 0) {
                    /* PIE 加载到非 0 地址，需要调整 */
                    load_bias = 0; /* 我们的实现总是加载到指定的虚拟地址 */
                }

                KLOG_INFO("[elf] ELF type=%u, min_vaddr=0x%llx, load_bias=0x%llx\n",
                          ehdr->e_type, min_vaddr, load_bias);

                /* 处理每个 RELA 条目 */
                uint64_t rela_count = rela_size / rela_ent;
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
                    if (r_type == R_AARCH64_RELATIVE) {
                        /* RELATIVE: 基址 + addend */
                        uint64_t target_vaddr = rela->r_offset;
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
                    } else if (r_type == R_AARCH64_JUMP_SLOT ||
                               r_type == R_AARCH64_GLOB_DAT) {
                        /* JUMP_SLOT/GLOB_DAT: 函数地址
                         * 对于静态链接的 PIE，这些应该指向 load_bias + addend
                         * 但如果 addend 很小（比如 3），可能是编译器生成的占位符
                         */
                        uint64_t target_vaddr = rela->r_offset;
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
                                      r_type == R_AARCH64_JUMP_SLOT ? "JUMP_SLOT" : "GLOB_DAT",
                                      target_vaddr, rela->r_addend, new_value);
                        }

                        KLOG_TRACE("[elf] %s 0x%llx: 0x%llx -> 0x%llx\n",
                                  r_type == R_AARCH64_JUMP_SLOT ? "JUMP_SLOT" : "GLOB_DAT",
                                  target_vaddr, *target, new_value);
                        *target = new_value;
                    } else if (r_type != 0) {
                        KLOG_WARN("[elf] Unsupported relocation type: %u at offset 0x%llx\n",
                                  r_type, rela->r_offset);
                    }
                }

                KLOG_INFO("[elf] Processed %llu RELA relocations\n", rela_count);
            }
            break;
        }
    }

    /* 映射用户栈到 ELF 页表中 */
    uint64_t stack_bottom = ALIGN_DOWN(USER_STACK_ADDR - USER_STACK_SIZE, PAGE_SIZE);
    for (uint64_t vaddr = stack_bottom; vaddr < USER_STACK_ADDR; vaddr += PAGE_SIZE) {
        uint64_t stack_page = pmm_alloc_pages(g_pmm, 1);
        if (stack_page == 0) {
            KLOG_ERROR("[elf] Failed to allocate stack page\n");
            return -10;
        }
        if (mm_vm_map_pages(pgd, vaddr, stack_page, 1, 0) != 0) {
            KLOG_ERROR("[elf] Failed to map stack page at 0x%llx\n", vaddr);
            pmm_free_pages(g_pmm, stack_page, 1);
            return -11;
        }
    }
    KLOG_INFO("[elf] User stack mapped: 0x%llx - 0x%llx\n", stack_bottom, (uint64_t)USER_STACK_ADDR);

    /* 在用户栈最高页构建 Linux ABI 初始栈（argc/argv/envp/auxv） */
    /* AT_PHDR: 程序头在用户空间的地址 = 加载基址 0 + ehdr->e_phoff */
    uint64_t phdr_uaddr = ehdr->e_phoff;  /* for base=0, file_offset == user_vaddr */
    uint64_t user_sp = USER_STACK_ADDR;
    if (elf_setup_stack(pgd, USER_STACK_ADDR, entry_point, argv, envp, &user_sp,
                        phdr_uaddr, ehdr->e_phnum, ehdr->e_phentsize) != 0) {
        KLOG_ERROR("[elf] Failed to setup initial stack\n");
        return -12;
    }

    /* 使用已建好的页表创建用户任务（跳过 vm_create_user_process） */
    task_t *current = task_current();
    uint32_t saved_parent_id = current->parent_id;

    new_task = process_create_with_pgd(pathname, entry_point, user_sp, 10, pgd_phys,
                                       ALIGN_UP(max_vaddr, PAGE_SIZE), 0x30000000ULL);
    if (new_task == NULL) {
        KLOG_ERROR("[elf] Failed to create task\n");
        return -13;
    }

    /* 修复父进程关系：新进程应该继承当前进程的父进程
     * 这样父进程的 wait4 会等待新进程，而不是已退出的当前进程 */
    new_task->parent_id = saved_parent_id;

    KLOG_INFO("[elf] Process '%s' created, PID=%u, pgd=0x%llx, parent=%u\n",
              pathname, new_task->id, pgd_phys, new_task->parent_id);
    KLOG_INFO("[elf]   heap_start=0x%llx, mmap_base=0x%llx\n",
              new_task->heap_end, new_task->mmap_next);

    /* 退出当前进程 */
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

    (void)argv;
    (void)envp;

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
