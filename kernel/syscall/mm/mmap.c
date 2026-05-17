/*
 * mm/mmap.c - sys_mmap 实现（包含匿名映射与文件映射）
 *
 * 从 kernel/syscall/syscall.c 迁出。文件映射使用 fd_pool 中的
 * ext4_file，所以本文件必须用 $(LWEXT4_CFLAGS) 编译。
 */
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "klog.h"
#include "task/task.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "arch.h"
#include <ext4.h>

uint64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset)
{
    (void)prot;

    task_t *current = task_current();
    if (!current->is_user_process) {
        return MMAP_FAILED;
    }

    /* ── 文件映射（ld.so 用于映射 .so 段）───────────────────── */
    if (!(flags & MAP_ANONYMOUS)) {
        if (fd < 0 || fd >= (int)TASK_MAX_FD) {
            KLOG_WARN("[mmap] file-backed: bad fd=%d\n", fd);
            return MMAP_FAILED;
        }
        int8_t fidx = current->fd_table[fd];
        if (fidx < 0 || (int)fidx >= FD_POOL_SIZE) {
            KLOG_WARN("[mmap] file-backed: fd %d not open\n", fd);
            return MMAP_FAILED;
        }
        fd_obj_t *fobj = &g_fd_pool[(uint8_t)fidx];
        if (fobj->type != FDT_FILE) {
            KLOG_WARN("[mmap] file-backed: fd %d not a regular file\n", fd);
            return MMAP_FAILED;
        }
        if (len == 0) return (uint64_t)(int64_t)-EINVAL;

        uint64_t map_addr;
        if ((flags & MAP_FIXED) != 0) {
            if (addr & (PAGE_SIZE - 1)) return (uint64_t)(int64_t)-EINVAL;
            map_addr = addr;
        } else {
            map_addr = ALIGN_UP(current->mmap_next, PAGE_SIZE);
        }
        uint64_t map_size = ALIGN_UP(len, PAGE_SIZE);

        void *pgd = phys_to_virt((uint64_t)current->pgd);

        for (uint64_t page_off = 0; page_off < map_size; page_off += PAGE_SIZE) {
            uint64_t va = map_addr + page_off;
            uint64_t pa;

            if ((flags & MAP_FIXED) != 0) {
                pa = mm_vm_get_paddr(pgd, va);
                if (pa == 0) {
                    pa = pmm_alloc_pages(g_pmm, 1);
                    if (pa == 0) return MMAP_FAILED;
                    if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
                        pmm_free_pages(g_pmm, pa, 1);
                        return MMAP_FAILED;
                    }
                }
            } else {
                pa = pmm_alloc_pages(g_pmm, 1);
                if (pa == 0) return MMAP_FAILED;
                if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
                    pmm_free_pages(g_pmm, pa, 1);
                    return MMAP_FAILED;
                }
            }

            memset(phys_to_virt(pa), 0, PAGE_SIZE);

            /* 从文件读取对应区间的数据，剩余已由 memset 清零 */
            uint64_t file_off = offset + page_off;
            uint64_t to_read  = PAGE_SIZE;
            if (page_off + PAGE_SIZE > len)
                to_read = len - page_off;
            if (to_read > 0) {
                ext4_fseek(&fobj->file, (int64_t)file_off, SEEK_SET);
                size_t got = 0;
                ext4_fread(&fobj->file, phys_to_virt(pa), to_read, &got);
            }
        }

        if ((flags & MAP_FIXED) == 0)
            current->mmap_next = map_addr + map_size;

        KLOG_DEBUG("[mmap] file 0x%llx-0x%llx fd=%d off=0x%llx\n",
                   map_addr, map_addr + map_size, fd, offset);
        return map_addr;
    }

    if (len == 0) {
        return (uint64_t)(int64_t)-EINVAL;
    }

    uint64_t page_off = 0;
    uint64_t map_addr;
    uint64_t size;

    if ((flags & MAP_FIXED) != 0) {
        if ((addr & (PAGE_SIZE - 1)) != 0) {
            return (uint64_t)(int64_t)-EINVAL;
        }
        map_addr = addr;
        page_off = 0;
    } else {
        map_addr = ALIGN_UP(current->mmap_next, PAGE_SIZE);
        page_off = 0;
    }

    size = ALIGN_UP(len + page_off, PAGE_SIZE);

    KLOG_DEBUG("[mmap] req: addr=0x%llx len=0x%llx flags=0x%x fd=%d off=0x%llx -> base=0x%llx size=0x%llx\n",
               addr, len, flags, fd, offset, map_addr, size);

#if ARCH_AARCH64 || ARCH_RISCV64
    extern pmm_t pmm;
    void *pgd = phys_to_virt((uint64_t)current->pgd);

    for (uint64_t va = map_addr; va < map_addr + size; va += PAGE_SIZE) {
        if ((flags & MAP_FIXED) != 0) {
            uint64_t old_pa = mm_vm_get_paddr(pgd, va);
            if (old_pa != 0) {
                memset(phys_to_virt(old_pa), 0, PAGE_SIZE);
                continue;
            }
        }

        uint64_t pa = pmm_alloc_pages(&pmm, 1);
        if (pa == 0) {
            KLOG_ERROR("[mmap] Out of memory at va=0x%llx\n", va);
            return MMAP_FAILED;
        }
        memset(phys_to_virt(pa), 0, PAGE_SIZE);
        if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
            KLOG_WARN("[mmap] map failed: va=0x%llx flags=0x%x\n", va, flags);
            pmm_free_pages(&pmm, pa, 1);
            return MMAP_FAILED;
        }
    }
#elif ARCH_X86_64
    {
        void *pgd = phys_to_virt((uint64_t)current->pgd);

        for (uint64_t va = map_addr; va < map_addr + size; va += PAGE_SIZE) {
            if ((flags & MAP_FIXED) != 0) {
                uint64_t old_pa = mm_vm_get_paddr(pgd, va);
                if (old_pa != 0) {
                    memset(phys_to_virt(old_pa), 0, PAGE_SIZE);
                    continue;
                }
            }

            uint64_t pa = pmm_alloc_pages(g_pmm, 1);
            if (pa == 0) {
                KLOG_ERROR("[mmap] Out of memory at va=0x%llx\n", va);
                return MMAP_FAILED;
            }
            memset(phys_to_virt(pa), 0, PAGE_SIZE);
            if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
                KLOG_WARN("[mmap] map failed: va=0x%llx flags=0x%x\n", va, flags);
                pmm_free_pages(g_pmm, pa, 1);
                return MMAP_FAILED;
            }
        }
    }
#endif

    if ((flags & MAP_FIXED) == 0) {
        current->mmap_next = map_addr + size;
    }

    KLOG_DEBUG("[mmap] 0x%llx - 0x%llx (len=0x%llx)\n", map_addr, map_addr + size, len);
    return map_addr + page_off;
}
