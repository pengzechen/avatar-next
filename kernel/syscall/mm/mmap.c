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
#include "vm_user.h"
#include "user_layout.h"
#include <ext4.h>

#if DRIVER_ION
#include "ion/ion.h"
#endif

extern task_t g_task_pool[];
extern uint8_t g_stack_used[];

static uint64_t shared_mmap_next(task_t *current)
{
    uint64_t next = current->mmap_next;

    for (uint32_t i = 0; i < TASK_MAX; i++) {
        task_t *task = &g_task_pool[i];
        if (!g_stack_used[i] || !task->is_user_process || task->state == TASK_DEAD)
            continue;
        if (task->pgd == current->pgd && task->mmap_next > next)
            next = task->mmap_next;
    }

    return next;
}

static void sync_shared_mmap_next(task_t *current, uint64_t next)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        task_t *task = &g_task_pool[i];
        if (!g_stack_used[i] || !task->is_user_process || task->state == TASK_DEAD)
            continue;
        if (task->pgd == current->pgd && task->mmap_next < next)
            task->mmap_next = next;
    }
}

static int mmap_user_range_ok(uint64_t start, uint64_t size)
{
    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;

    if (size == 0 || start + size < start)
        return 0;
    if (start < PAGE_SIZE)
        return 0;
    if (start + size > stack_bottom)
        return 0;
    return 1;
}

static void sync_shared_mmap_next_to(task_t *current, uint64_t val)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        task_t *task = &g_task_pool[i];
        if (!g_stack_used[i] || !task->is_user_process || task->state == TASK_DEAD)
            continue;
        if (task->pgd == current->pgd)
            task->mmap_next = val;
    }
}

uint64_t sys_munmap(uint64_t addr, uint64_t len)
{
    task_t *current = task_current();
    if (!current || !current->is_user_process || current->pgd == NULL)
        return (uint64_t)(int64_t)-EINVAL;
    if (len == 0 || (addr & (PAGE_SIZE - 1)) != 0)
        return (uint64_t)(int64_t)-EINVAL;

    uint64_t size = ALIGN_UP(len, PAGE_SIZE);
    vm_unmap_user_range((uint64_t)current->pgd, addr, size);

    uint64_t top  = addr + size;
    uint64_t next = shared_mmap_next(current);
    if (top >= next) {
        void *pgd = phys_to_virt((uint64_t)current->pgd);
        uint64_t new_next = addr;
        uint64_t lo = ALIGN_UP(current->heap_end, PAGE_SIZE);
        int budget = 512;
        while (new_next > lo && budget-- > 0) {
            if (mm_vm_get_paddr(pgd, new_next - PAGE_SIZE) != 0)
                break;
            new_next -= PAGE_SIZE;
        }
        if (new_next < next)
            sync_shared_mmap_next_to(current, new_next);
    }

    return 0;
}

uint64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset)
{
    task_t *current = task_current();
    if (!current->is_user_process) {
        return MMAP_FAILED;
    }

    /* ── 文件映射（ld.so 用于映射 .so 段）───────────────────── */
    if (!(flags & MAP_ANONYMOUS)) {
        if (len == 0) return (uint64_t)(int64_t)-EINVAL;

        int16_t fidx = -1;
        fd_obj_t *fobj = NULL;
        if (fd >= 0 && fd < (int)TASK_MAX_FD) {
            fidx = current->fd_table[fd];
            if (fidx >= 0 && (int)fidx < FD_POOL_SIZE)
                fobj = &g_fd_pool[fidx];
        }

        uint64_t map_addr;
        if ((flags & MAP_FIXED) != 0) {
            if (addr & (PAGE_SIZE - 1)) return (uint64_t)(int64_t)-EINVAL;
            map_addr = addr;
        } else {
            map_addr = ALIGN_UP(shared_mmap_next(current), PAGE_SIZE);
        }
        uint64_t map_size = ALIGN_UP(len, PAGE_SIZE);

        if (!mmap_user_range_ok(map_addr, map_size)) {
            KLOG_WARN("[mmap] file range rejected: path=%s req=0x%llx base=0x%llx size=0x%llx flags=0x%x\n",
                      fobj ? fobj->path : "<none>", addr, map_addr, map_size, flags);
            return (uint64_t)(int64_t)-ENOMEM;
        }

        void *pgd = phys_to_virt((uint64_t)current->pgd);

#if DRIVER_ION
        if (fobj && fobj->type == FDT_ION) {
            void *ion_va = NULL;
            uint64_t ion_pa = 0;
            ion_handle_t handle = (ion_handle_t)fobj->ion.handle;
            size_t ion_size = ion_get_size(handle);

            if (ion_size == 0 || offset != 0 || ion_get_buf(handle, &ion_va, &ion_pa) != 0) {
                KLOG_WARN("[mmap] ion fd=%d handle=%u invalid (flags=0x%x)\n",
                          fd, handle, flags);
                return MMAP_FAILED;
            }
            if (map_size > ALIGN_UP((uint64_t)ion_size, PAGE_SIZE)) {
                KLOG_WARN("[mmap] ion fd=%d handle=%u len=0x%llx exceeds size=0x%llx\n",
                          fd, handle, map_size, (unsigned long long)ion_size);
                return (uint64_t)(int64_t)-EINVAL;
            }

            for (uint64_t page_off = 0; page_off < map_size; page_off += PAGE_SIZE) {
                uint64_t va = map_addr + page_off;
                uint64_t pa = ion_pa + page_off;

                if ((flags & MAP_FIXED) != 0 && mm_vm_get_paddr(pgd, va) != 0)
                    vm_unmap_user_range((uint64_t)current->pgd, va, PAGE_SIZE);
                if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
                    KLOG_WARN("[mmap] ion map failed: fd=%d handle=%u va=0x%llx pa=0x%llx\n",
                              fd, handle, va, pa);
                    return MMAP_FAILED;
                }
#if ARCH_RISCV64
                uint64_t *pte = rv_walk_l0_pte(pgd, va, false);
                if (pte)
                    *pte |= RV_PTE_NOFREE;
#endif
            }

            if ((flags & MAP_FIXED) == 0)
                sync_shared_mmap_next(current, map_addr + map_size);
            KLOG_DEBUG("[mmap] ion fd=%d handle=%u pa=0x%llx va=0x%llx req=0x%llx 0x%llx-0x%llx prot=0x%x flags=0x%x\n",
                       fd, handle, ion_pa, (unsigned long long)(uintptr_t)ion_va,
                       addr, map_addr, map_addr + map_size,
                       prot, flags);
            return map_addr;
        }
#endif

        if (fd < 0 || fd >= (int)TASK_MAX_FD) {
            KLOG_WARN("[mmap] file-backed: bad fd=%d\n", fd);
            return MMAP_FAILED;
        }
        if (!fobj || fobj->type != FDT_FILE) {
            KLOG_WARN("[mmap] file-backed: fd %d not a regular file\n", fd);
            return MMAP_FAILED;
        }

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
            sync_shared_mmap_next(current, map_addr + map_size);

        KLOG_DEBUG("[mmap] file path=%s req=0x%llx 0x%llx-0x%llx prot=0x%x flags=0x%x fd=%d off=0x%llx\n",
               fobj->path, addr, map_addr, map_addr + map_size,
               prot, flags, fd, offset);
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
        map_addr = ALIGN_UP(shared_mmap_next(current), PAGE_SIZE);
        page_off = 0;
    }

    size = ALIGN_UP(len + page_off, PAGE_SIZE);

    if (!(flags & MAP_FIXED) && !mmap_user_range_ok(map_addr, size)) {
        void *pgd = phys_to_virt((uint64_t)current->pgd);
        uint64_t lo = ALIGN_UP(current->heap_end, PAGE_SIZE);
        uint64_t hi = USER_STACK_TOP - USER_STACK_SIZE;
        uint64_t need = size / PAGE_SIZE;
        uint64_t run = 0;
        uint64_t cand = 0;
        for (uint64_t va = lo; va + size <= hi; va += PAGE_SIZE) {
            if (mm_vm_get_paddr(pgd, va) == 0) {
                if (run == 0) cand = va;
                if (++run >= need) {
                    map_addr = cand;
                    goto found;
                }
            } else {
                run = 0;
            }
        }
        KLOG_WARN("[mmap] anon gap-search failed: len=0x%llx free_pg=%llu\n",
                  len, pmm_get_free_pages(g_pmm));
        return (uint64_t)(int64_t)-ENOMEM;
found:;
    }

    if (!mmap_user_range_ok(map_addr, size)) {
        KLOG_WARN("[mmap] anon range rejected: req=0x%llx base=0x%llx size=0x%llx flags=0x%x\n",
                  addr, map_addr, size, flags);
        return (uint64_t)(int64_t)-ENOMEM;
    }

    KLOG_DEBUG("[mmap] req: addr=0x%llx len=0x%llx flags=0x%x fd=%d off=0x%llx -> base=0x%llx size=0x%llx\n",
               addr, len, flags, fd, offset, map_addr, size);

#if ARCH_AARCH64 || ARCH_RISCV64
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
        sync_shared_mmap_next(current, map_addr + size);
    }

    KLOG_DEBUG("[mmap] 0x%llx - 0x%llx (len=0x%llx)\n", map_addr, map_addr + size, len);
    return map_addr + page_off;
}
