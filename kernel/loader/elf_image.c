/*
 * kernel/loader/elf_image.c — ELF 镜像加载
 *
 * 职责：ELF 格式校验 + PT_LOAD 段加载到用户地址空间 + RELA 重定位
 * 输入：已初始化的用户页表（含内核映射，由调用方准备）
 * 输出：elf_image_info_t（入口点、VA 范围、程序头地址等）
 */

#include "elf.h"
#include "klog.h"
#include "pmm.h"
#include "mm_vm.h"
#include "string.h"
#include "arch.h"
#include "loader/elf_image.h"
#include "user_layout.h"

extern pmm_t *g_pmm;

/* ── 段加载 ─────────────────────────────────────────────────────── */

/**
 * elf_load_segment - 加载单个 ELF 段到用户空间
 */
static int
elf_load_segment(void *pgd, elf64_phdr_t *phdr, uint8_t *file_data)
{
    uint64_t vaddr  = phdr->p_vaddr;
    uint64_t filesz = phdr->p_filesz;
    uint64_t memsz  = phdr->p_memsz;
    uint64_t offset = phdr->p_offset;
    uint64_t seg_base_paddr;

    uint64_t vaddr_start = ALIGN_DOWN(vaddr, PAGE_SIZE);
    uint64_t vaddr_end   = ALIGN_UP(vaddr + memsz, PAGE_SIZE);
    uint64_t total_pages = (vaddr_end - vaddr_start) / PAGE_SIZE;
    uint64_t page_idx    = 0;

    KLOG_INFO("[elf] Loading segment:\n");
    KLOG_INFO("[elf]   vaddr: 0x%llx - 0x%llx\n", vaddr_start, vaddr_end);
    KLOG_INFO("[elf]   filesz: 0x%llx, memsz: 0x%llx\n", filesz, memsz);

    uint64_t perm = 0;  /* 默认：用户可读写可执行 */

    seg_base_paddr = pmm_alloc_pages(g_pmm, (uint32_t)total_pages);
    if (seg_base_paddr == 0) {
        KLOG_ERROR("[elf] Failed to allocate %llu contiguous pages\n", total_pages);
        return -1;
    }

    if (mm_vm_map_pages(pgd, vaddr_start, seg_base_paddr, (int32_t)total_pages, perm) != 0) {
        KLOG_ERROR("[elf] Failed to map segment pages at 0x%llx (count=%llu)\n",
                   vaddr_start, total_pages);
        pmm_free_pages(g_pmm, seg_base_paddr, (uint32_t)total_pages);
        return -2;
    }

    uint64_t seg_bytes = total_pages * PAGE_SIZE;
    memset(phys_to_virt(seg_base_paddr), 0, seg_bytes);

    if (filesz > 0) {
        uint64_t file_off_in_seg = vaddr - vaddr_start;
        uint8_t *dst = (uint8_t *)phys_to_virt(seg_base_paddr + file_off_in_seg);
        uint8_t *src = file_data + offset;
        memcpy(dst, src, filesz);
    }

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

static int
elf_load_segment_with_base(void *pgd, elf64_phdr_t *phdr,
                            uint8_t *file_data, uint64_t image_base)
{
    elf64_phdr_t adj = *phdr;
    adj.p_vaddr = phdr->p_vaddr + image_base;
    return elf_load_segment(pgd, &adj, file_data);
}

/* ── ELF 镜像加载 ────────────────────────────────────────────────── */

int
elf_image_load(uint8_t *file_data, uint64_t file_size, void *pgd, elf_image_info_t *out)
{
    elf64_ehdr_t *ehdr;
    elf64_phdr_t *phdr;
    uint64_t entry_point;
    uint64_t image_base = 0;
    uint64_t min_vaddr  = (uint64_t)-1;
    uint64_t max_vaddr  = 0;

    if (file_size < sizeof(elf64_ehdr_t)) {
        KLOG_ERROR("[elf] File too small\n");
        return -1;
    }

    ehdr = (elf64_ehdr_t *)file_data;

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

    if (ehdr->e_phnum == 0 || ehdr->e_phentsize != sizeof(elf64_phdr_t)) {
        KLOG_ERROR("[elf] Invalid program header\n");
        return -7;
    }

    phdr = (elf64_phdr_t *)(file_data + ehdr->e_phoff);

    /* ET_DYN 使用 PIE 基址 USER_CODE_BASE */
    if (ehdr->e_type == ET_DYN) {
        image_base = USER_CODE_BASE;
        KLOG_INFO("[elf] ET_DYN image base: 0x%llx\n", image_base);
    }

    if (image_base != 0)
        entry_point += image_base;

    KLOG_INFO("[elf] Loading ELF segments...\n");

    /* 加载所有 PT_LOAD 段 */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            int rc = elf_load_segment_with_base(pgd, &phdr[i], file_data, image_base);
            if (rc < 0) {
                KLOG_ERROR("[elf] Failed to load segment %u\n", i);
                return -8;
            }

            if (phdr[i].p_vaddr + image_base < min_vaddr)
                min_vaddr = phdr[i].p_vaddr + image_base;
            if (phdr[i].p_vaddr + image_base + phdr[i].p_memsz > max_vaddr)
                max_vaddr = phdr[i].p_vaddr + image_base + phdr[i].p_memsz;
        }
    }

    KLOG_INFO("[elf] ELF loaded: vaddr 0x%llx - 0x%llx\n", min_vaddr, max_vaddr);
    KLOG_INFO("[elf] Entry point: 0x%llx\n", entry_point);

    /* ── 处理 RELA 重定位（PIE 需要）────────────────────────────── */
    KLOG_INFO("[elf] Processing RELA relocations...\n");
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            elf64_dyn_t *dyn = (elf64_dyn_t *)(file_data + phdr[i].p_offset);
            uint64_t dyn_count = phdr[i].p_filesz / sizeof(elf64_dyn_t);

            uint64_t rela_addr = 0;
            uint64_t rela_size = 0;
            uint64_t rela_ent  = 0;

            for (uint64_t j = 0; j < dyn_count; j++) {
                if (dyn[j].d_tag == DT_RELA)
                    rela_addr = dyn[j].d_un.d_ptr;
                else if (dyn[j].d_tag == DT_RELASZ)
                    rela_size = dyn[j].d_un.d_val;
                else if (dyn[j].d_tag == DT_RELAENT)
                    rela_ent = dyn[j].d_un.d_val;
            }

            if (rela_addr && rela_size && rela_ent) {
                KLOG_INFO("[elf] Found RELA: addr=0x%llx, size=%llu, ent=%llu\n",
                          rela_addr, rela_size, rela_ent);

                uint64_t load_bias = (ehdr->e_type == ET_DYN) ? image_base : 0;

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

                uint64_t rela_count = rela_size / rela_ent;
                uint64_t unsupported_count = 0;
                for (uint64_t r = 0; r < rela_count; r++) {
                    uint64_t rela_file_offset = 0;

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

                    if (r_type == reloc_relative) {
                        uint64_t target_vaddr    = load_bias + rela->r_offset;
                        uint64_t page_vaddr      = ALIGN_DOWN(target_vaddr, PAGE_SIZE);
                        uint64_t paddr           = mm_vm_get_paddr(pgd, page_vaddr);

                        if (paddr == 0) {
                            KLOG_ERROR("[elf] Cannot get paddr for 0x%llx\n", target_vaddr);
                            continue;
                        }

                        uint64_t new_value       = load_bias + rela->r_addend;
                        uint64_t offset_in_page  = target_vaddr - page_vaddr;
                        uint64_t *target         = (uint64_t *)phys_to_virt(paddr + offset_in_page);

                        KLOG_TRACE("[elf] RELATIVE 0x%llx: 0x%llx -> 0x%llx\n",
                                   target_vaddr, *target, new_value);
                        *target = new_value;
                    } else if (r_type == reloc_jump_slot ||
                               r_type == reloc_glob_dat) {
                        uint64_t target_vaddr    = load_bias + rela->r_offset;
                        uint64_t page_vaddr      = ALIGN_DOWN(target_vaddr, PAGE_SIZE);
                        uint64_t paddr           = mm_vm_get_paddr(pgd, page_vaddr);

                        if (paddr == 0) {
                            KLOG_ERROR("[elf] Cannot get paddr for 0x%llx\n", target_vaddr);
                            continue;
                        }

                        uint64_t new_value       = load_bias + rela->r_addend;
                        uint64_t offset_in_page  = target_vaddr - page_vaddr;
                        uint64_t *target         = (uint64_t *)phys_to_virt(paddr + offset_in_page);

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

    /* ── 计算程序头的用户态地址（AT_PHDR auxv）────────────────── */
    uint64_t phdr_uaddr;
    if (ehdr->e_type == ET_DYN) {
        phdr_uaddr = image_base + ehdr->e_phoff;
    } else {
        /* 优先查找 PT_PHDR 段（类型值 6） */
        phdr_uaddr = 0;
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == 6U /* PT_PHDR */) {
                phdr_uaddr = phdr[i].p_vaddr;
                break;
            }
        }
        /* 没有 PT_PHDR：用第一个 PT_LOAD 的基址 + 文件内偏移 */
        if (phdr_uaddr == 0) {
            for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset == 0) {
                    phdr_uaddr = phdr[i].p_vaddr + ehdr->e_phoff;
                    break;
                }
            }
        }
        if (phdr_uaddr == 0)
            phdr_uaddr = image_base + ehdr->e_phoff;
    }

    out->entry_point = entry_point;
    out->min_vaddr   = min_vaddr;
    out->max_vaddr   = max_vaddr;
    out->phdr_uaddr  = phdr_uaddr;
    out->phnum       = ehdr->e_phnum;
    out->phent       = ehdr->e_phentsize;

    return 0;
}
