/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   mm/mmap.c - sys_mmap 实现(包含匿名映射与文件映射)

   从 kernel/syscall/syscall.c 迁出。文件映射通过 VFS 读取文件内容。
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
#include "shared_page.h"
#include "vfs.h"
#if DRIVER_DMA_BUF
#include "dma_buf.h"
#endif
#if DRIVER_NPU_RKNPU
#include "rknpu_drm.h"
#endif

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
    /* vm_unmap_user_range 的副作用是必须的, 只是返回值目前只有下面那条
       被注释掉的调试日志在用 —— 所以标 UNUSED 而不是删掉整个调用。
    */
    uint64_t freed UNUSED = vm_unmap_user_range((uint64_t)current->pgd, addr, size);

    uint64_t top = addr + size;
    uint64_t next = shared_mmap_next(current);
    /* KLOG_SYSCALL("[munmap] pid=%d addr=0x%llx len=0x%llx size=0x%llx top=0x%llx next=0x%llx freed=%llu\n",
               current->id, addr, len, size, top, next, freed);
    */
    if (top >= next) {
        void *pgd = phys_to_virt((uint64_t)current->pgd);
        uint64_t new_next = addr;
        /*
           回退下限是本进程 mmap 区的基址, 不是 heap_end。

           原来写的是 heap_end(~0x153000), 比 mmap 基址(PIE 0x50000000)
           低得多 —— 把一个进程的 mmap 页全部 munmap 掉之后, mmap_next 会
           一路缩到 heap 里面去, 下一次 mmap 就在堆区分配, 和 brk 打架。
           mmap_user_range_ok() 只挡栈顶和 0 页, 拦不住这种分配。

           顺带这也是 fork 拷贝 mmap 区的前提：[mmap_base, mmap_next) 要
           能覆盖该进程全部 mmap 映射, 就得保证 mmap_next 永不小于基址。
        */
        uint64_t lo = current->mmap_base;
        int budget = 512;
        while (new_next > lo && budget-- > 0) {
            if (mm_vm_get_paddr(pgd, new_next - PAGE_SIZE) != 0)
                break;
            new_next -= PAGE_SIZE;
        }
        if (new_next < next) {
            /* KLOG_SYSCALL("[munmap] pid=%d shrink mmap_next: 0x%llx -> 0x%llx lo=0x%llx\n",
                       current->id, next, new_next, lo);
            */
            sync_shared_mmap_next_to(current, new_next);
        }
    }

    return 0;
}

/*
   注意： 只在 ION 或 dma-buf 至少开一个时才有调用者(两处调用都在各自的
      `#if` 里)。少了这个守卫, riscv64/x86_64 这种两个都关的平台上它会
      变成 -Wunused-function —— 而本项目是零告警的, 所以必须包起来。
*/
#if DRIVER_ION || DRIVER_DMA_BUF

/*
   把一段物理连续的缓冲区逐页映射进用户地址空间。

   ION 与 dma-buf 的句柄不同(一个是 ion handle, 一个是 dma_buf 表下标),
   但"逐页 mmap"这一段逐字节相同 —— 抽出来共用。尤其 riscv 那个 PTE_NOFREE
   位：漏掉一处就会出现「能读, 但页随时会被回收」的怪象, 两处各写一遍迟早漏一个。

   返回 0 表示映射完成；否则返回要原样交给用户态的负 errno。
   注意 -1 与 MMAP_FAILED 是同一个值(syscall_internal.h:179),
   所以调用方 `return (uint64_t)(int64_t)rc;` 对两种情况都对。
*/
static int map_contiguous_buf(task_t *current, void *pgd, uint64_t map_addr, uint64_t map_size, uint64_t pa,
                              uint64_t flags, const char *tag, int fd, uint64_t handle)
{
    for (uint64_t page_off = 0; page_off < map_size; page_off += PAGE_SIZE) {
        uint64_t va = map_addr + page_off;
        uint64_t page_pa = pa + page_off;

        if ((flags & MAP_FIXED) != 0 && mm_vm_get_paddr(pgd, va) != 0)
            vm_unmap_user_range((uint64_t)current->pgd, va, PAGE_SIZE);
        if (mm_vm_map_pages(pgd, va, page_pa, 1, VM_PERM_USER) != 0) {
            KLOG_WARN("[mmap] %s map failed: fd=%d handle=%llu va=0x%llx pa=0x%llx\n", tag, fd,
                      (unsigned long long)handle, (unsigned long long)va, (unsigned long long)page_pa);
            return -1;
        }
        /*
           标成"设备缓冲"：这一页归 dma_buf / ION 的句柄所有, 不归这个映射。

           不标的话 munmap 会把它释放回 PMM, 而句柄仍然指着它 —— 随后
           close(fd) 再释放一次(双重释放), 中间那段时间这些页随时会被
           分配给别的用途：对 NPU 缓冲区来说就是"设备往别人的内存里写"。
           实测症状是满屏 `PMM: partial free detected: ... actually_freed=0`。

           这里不能用 NOFREE 顶替：NOFREE 的语义是"最后一个映射退出
           就释放", 而设备缓冲归句柄所有 —— 只要句柄还开着, 任何映射退出
           都不该释放。详见 include/aarch64/mmu.h 里 PTE_KEEP 的说明。
        */
        mm_vm_set_pte_keep(pgd, va);
    }

    if ((flags & MAP_FIXED) == 0)
        sync_shared_mmap_next(current, map_addr + map_size);
    return 0;
}

#endif /* DRIVER_ION || DRIVER_DMA_BUF */

uint64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset)
{
    /* prot 目前只在 `#if DRIVER_ION` 那段(见下面 ion 分支)的调试日志里出现,
       该宏关闭的平台上它就是没人用；保护属性本身尚未实现。
    */
    (void)prot;

    task_t *current = task_current();
    if (!current->is_user_process) {
        return MMAP_FAILED;
    }

    /* ── 文件映射(ld.so 用于映射 .so 段)───────────────────── */
    if (!(flags & MAP_ANONYMOUS)) {
        if (len == 0)
            return (uint64_t)(int64_t)-EINVAL;

        int16_t fidx = -1;
        fd_obj_t *fobj = NULL;
        if (fd >= 0 && fd < (int)TASK_MAX_FD) {
            fidx = current->fd_table[fd];
            if (fidx >= 0 && (int)fidx < FD_POOL_SIZE)
                fobj = &g_fd_pool[fidx];
        }

        uint64_t map_addr;
        if ((flags & MAP_FIXED) != 0) {
            if (addr & (PAGE_SIZE - 1))
                return (uint64_t)(int64_t)-EINVAL;
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
                KLOG_WARN("[mmap] ion fd=%d handle=%u invalid (flags=0x%x)\n", fd, handle, flags);
                return MMAP_FAILED;
            }
            if (map_size > ALIGN_UP((uint64_t)ion_size, PAGE_SIZE)) {
                KLOG_WARN("[mmap] ion fd=%d handle=%u len=0x%llx exceeds size=0x%llx\n", fd, handle, map_size,
                          (unsigned long long)ion_size);
                return (uint64_t)(int64_t)-EINVAL;
            }

            int rc = map_contiguous_buf(current, pgd, map_addr, map_size, ion_pa, flags, "ion", fd, (uint64_t)handle);
            if (rc != 0)
                return (uint64_t)(int64_t)rc;

            KLOG_SYSCALL(
                "[mmap] ion fd=%d handle=%u pa=0x%llx va=0x%llx req=0x%llx 0x%llx-0x%llx prot=0x%x flags=0x%x\n", fd,
                handle, ion_pa, (unsigned long long)(uintptr_t)ion_va, addr, map_addr, map_addr + map_size, prot,
                flags);
            return map_addr;
        }
#endif

#if DRIVER_DMA_BUF
        /*
           dma-buf fd：一个 fd 就是一个缓冲区句柄, 句柄里存的是 dma_buf 表下标。
           设备地址一律经 dma_buf_dev_addr() 取 —— 那是全系统唯一出口,
           将来上 IOMMU 只改它(见 include/dma_buf.h)。
        */
        if (fobj && fobj->type == FDT_DMABUF) {
            dma_buf_t *dbuf = dma_buf_from_index(fobj->dmabuf.idx);
            if (!dbuf || offset != 0) {
                KLOG_WARN("[mmap] dmabuf fd=%d idx=%u invalid (offset=0x%llx)\n", fd, fobj ? fobj->dmabuf.idx : 0U,
                          (unsigned long long)offset);
                return MMAP_FAILED;
            }
            uint64_t db_size = ALIGN_UP((uint64_t)dma_buf_size(dbuf), PAGE_SIZE);
            if (map_size > db_size) {
                KLOG_WARN("[mmap] dmabuf fd=%d len=0x%llx exceeds size=0x%llx\n", fd, (unsigned long long)map_size,
                          (unsigned long long)db_size);
                return (uint64_t)(int64_t)-EINVAL;
            }

            uint64_t db_pa = dma_buf_dev_addr(dbuf);
            int rc = map_contiguous_buf(current, pgd, map_addr, map_size, db_pa, flags, "dmabuf", fd,
                                        (uint64_t)fobj->dmabuf.idx);
            if (rc != 0)
                return (uint64_t)(int64_t)rc;

            KLOG_SYSCALL("[mmap] dmabuf fd=%d idx=%u pa=0x%llx 0x%llx-0x%llx prot=0x%x flags=0x%x\n", fd,
                         fobj->dmabuf.idx, (unsigned long long)db_pa, (unsigned long long)map_addr,
                         (unsigned long long)(map_addr + map_size), prot, flags);
            return map_addr;
        }
#endif

#if DRIVER_NPU_RKNPU
        /*
           DRM 渲染节点的假偏移：RKNPU_MEM_MAP 回一个不透明的 offset,
           用户态拿它 mmap(fd, ..., offset)。这是 DRM GEM 的老约定
           (offset 不是文件偏移, 是驱动自定义的令牌)。

           只有 offset 非 0 才走这里 —— offset 0 落回下面"不是普通文件"的
           拒绝路径, 语义不变。
        */
        if (offset != 0 && fobj && fobj->type == FDT_VFS && fd_obj_file(fobj) &&
            fd_obj_file(fobj)->kind == VFS_FILE_PSEUDO) {
            uint64_t dpa = 0;
            uint64_t dsz = 0;
            if (rknpu_drm_mmap_lookup((void *)fd_obj_file(fobj), offset, &dpa, &dsz) == 0) {
                if (map_size > ALIGN_UP(dsz, PAGE_SIZE)) {
                    KLOG_WARN("[mmap] rknpu off=0x%llx len=0x%llx exceeds size=0x%llx\n", (unsigned long long)offset,
                              (unsigned long long)map_size, (unsigned long long)dsz);
                    return (uint64_t)(int64_t)-EINVAL;
                }
                int rc = map_contiguous_buf(current, pgd, map_addr, map_size, dpa, flags, "rknpu", fd, offset);
                if (rc != 0)
                    return (uint64_t)(int64_t)rc;
                return map_addr;
            }
        }
#endif

        if (fd < 0 || fd >= (int)TASK_MAX_FD) {
            KLOG_WARN("[mmap] file-backed: bad fd=%d\n", fd);
            return MMAP_FAILED;
        }
        if (!fobj || !vfs_file_is_regular(fd_obj_file(fobj))) {
            KLOG_WARN("[mmap] file-backed: fd %d not a regular file\n", fd);
            return MMAP_FAILED;
        }

        for (uint64_t page_off = 0; page_off < map_size; page_off += PAGE_SIZE) {
            uint64_t va = map_addr + page_off;
            uint64_t pa;

            if ((flags & MAP_FIXED) == 0 && mm_vm_get_paddr(pgd, va) != 0) {
                KLOG_WARN(
                    "[mmap] file non-fixed overlaps existing PTE: pid=%d va=0x%llx base=0x%llx size=0x%llx path=%s flags=0x%x\n",
                    current->id, va, map_addr, map_size, fobj->path, flags);
            }

            bool new_page = false;
            if ((flags & MAP_FIXED) != 0) {
                pa = mm_vm_get_paddr(pgd, va);
                if (pa == 0)
                    new_page = true;
            } else {
                new_page = true;
            }

            if (new_page) {
                pa = pmm_alloc_pages(&g_pmm, 1);
                if (pa == 0)
                    return MMAP_FAILED;

                /*
                   先 ref 再映射：ref 失败(共享页表满)时, 这次映射必须
                   在页表里留下零个 NOFREE 痕迹, 否则 unmap 找不到账本。
                */
                if ((flags & MAP_SHARED) && !shared_page_ref(pa)) {
                    KLOG_ERROR("[mmap] shared page table full: pid=%d va=0x%llx pa=0x%llx\n", current->id, va, pa);
                    pmm_free_pages(&g_pmm, pa, 1);
                    return MMAP_FAILED;
                }
                if (mm_vm_map_pages(pgd, va, pa, 1, VM_PERM_USER) != 0) {
                    if (flags & MAP_SHARED)
                        shared_page_unref(pa);
                    else
                        pmm_free_pages(&g_pmm, pa, 1);
                    return MMAP_FAILED;
                }
            }

            memset(phys_to_virt(pa), 0, PAGE_SIZE);

            /* 从文件读取对应区间的数据, 剩余已由 memset 清零 */
            uint64_t file_off = offset + page_off;
            uint64_t to_read = PAGE_SIZE;
            if (page_off + PAGE_SIZE > len)
                to_read = len - page_off;
            if (to_read > 0) {
                vfs_read_to_phys(fd_obj_file(fobj), file_off, phys_to_virt(pa), to_read);
            }

            if (flags & MAP_SHARED) {
                /*
                   新页在映射前已经 ref 过；MAP_FIXED 复用旧页时才在这里
                   补一次。ref 失败时不要改 PTE —— 旧映射保持原样。
                */
                if (!new_page && !shared_page_ref(pa)) {
                    KLOG_ERROR("[mmap] shared page table full: pid=%d va=0x%llx pa=0x%llx\n", current->id, va, pa);
                    return MMAP_FAILED;
                }
#if ARCH_RISCV64
                uint64_t *pte = rv_walk_l0_pte(pgd, va, false);
                if (pte)
                    *pte |= RV_PTE_NOFREE;
#elif ARCH_AARCH64
                memory_set_pte_nofree(pgd, va);
#elif ARCH_X86_64
                uint64_t *pte = x86_walk_pt(pgd, va, false);
                if (pte)
                    *pte |= PTE_NOFREE;
#endif
            }
        }

        if ((flags & MAP_FIXED) == 0)
            sync_shared_mmap_next(current, map_addr + map_size);

        /* KLOG_SYSCALL("[mmap] file path=%s req=0x%llx 0x%llx-0x%llx prot=0x%x flags=0x%x fd=%d off=0x%llx\n",
               fobj->path, addr, map_addr, map_addr + map_size,
               prot, flags, fd, offset);
        */
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
                if (run == 0)
                    cand = va;
                if (++run >= need) {
                    map_addr = cand;
                    goto found;
                }
            } else {
                run = 0;
            }
        }
        KLOG_WARN("[mmap] anon gap-search failed: len=0x%llx free_pg=%llu\n", len, pmm_get_free_pages(&g_pmm));
        return (uint64_t)(int64_t)-ENOMEM;
    found:;
    }

    if (!mmap_user_range_ok(map_addr, size)) {
        KLOG_WARN("[mmap] anon range rejected: req=0x%llx base=0x%llx size=0x%llx flags=0x%x\n", addr, map_addr, size,
                  flags);
        return (uint64_t)(int64_t)-ENOMEM;
    }

    /* KLOG_SYSCALL("[mmap] req: addr=0x%llx len=0x%llx flags=0x%x fd=%d off=0x%llx -> base=0x%llx size=0x%llx\n",
               addr, len, flags, fd, offset, map_addr, size);
    */

#if ARCH_AARCH64 || ARCH_RISCV64
    void *pgd = phys_to_virt((uint64_t)current->pgd);

    for (uint64_t va = map_addr; va < map_addr + size; va += PAGE_SIZE) {
        if ((flags & MAP_FIXED) == 0 && mm_vm_get_paddr(pgd, va) != 0) {
            KLOG_WARN(
                "[mmap] anon non-fixed overlaps existing PTE: pid=%d va=0x%llx base=0x%llx size=0x%llx flags=0x%x next=0x%llx heap=0x%llx\n",
                current->id, va, map_addr, size, flags, shared_mmap_next(current), current->heap_end);
        }

        if ((flags & MAP_FIXED) != 0) {
            uint64_t old_pa = mm_vm_get_paddr(pgd, va);
            if (old_pa != 0) {
                memset(phys_to_virt(old_pa), 0, PAGE_SIZE);
                continue;
            }
        }

        uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
        if (pa == 0) {
            KLOG_ERROR("[mmap] Out of memory at va=0x%llx\n", va);
            return MMAP_FAILED;
        }
        memset(phys_to_virt(pa), 0, PAGE_SIZE);

        /* 先 ref 再映射：失败时页表里不能出现没有账本的 NOFREE 页。 */
        if ((flags & MAP_SHARED) && !shared_page_ref(pa)) {
            KLOG_ERROR("[mmap] shared page table full: pid=%d va=0x%llx pa=0x%llx\n", current->id, va, pa);
            pmm_free_pages(&g_pmm, pa, 1);
            return MMAP_FAILED;
        }
        if (mm_vm_map_pages(pgd, va, pa, 1, VM_PERM_USER) != 0) {
            KLOG_WARN("[mmap] map failed: va=0x%llx flags=0x%x\n", va, flags);
            if (flags & MAP_SHARED)
                shared_page_unref(pa);
            else
                pmm_free_pages(&g_pmm, pa, 1);
            return MMAP_FAILED;
        }
#if ARCH_RISCV64
        if (flags & MAP_SHARED) {
            uint64_t *pte = rv_walk_l0_pte(pgd, va, false);
            if (pte)
                *pte |= RV_PTE_NOFREE;
        }
#elif ARCH_AARCH64
        if (flags & MAP_SHARED)
            memory_set_pte_nofree(pgd, va);
#endif
    }
#elif ARCH_X86_64
    {
        void *pgd = phys_to_virt((uint64_t)current->pgd);

        for (uint64_t va = map_addr; va < map_addr + size; va += PAGE_SIZE) {
            if ((flags & MAP_FIXED) == 0 && mm_vm_get_paddr(pgd, va) != 0) {
                KLOG_WARN(
                    "[mmap] anon non-fixed overlaps existing PTE: pid=%d va=0x%llx base=0x%llx size=0x%llx flags=0x%x next=0x%llx heap=0x%llx\n",
                    current->id, va, map_addr, size, flags, shared_mmap_next(current), current->heap_end);
            }

            if ((flags & MAP_FIXED) != 0) {
                uint64_t old_pa = mm_vm_get_paddr(pgd, va);
                if (old_pa != 0) {
                    memset(phys_to_virt(old_pa), 0, PAGE_SIZE);
                    continue;
                }
            }

            uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
            if (pa == 0) {
                KLOG_ERROR("[mmap] Out of memory at va=0x%llx\n", va);
                return MMAP_FAILED;
            }
            memset(phys_to_virt(pa), 0, PAGE_SIZE);

            /* 先 ref 再映射：失败时页表里不能出现没有账本的 NOFREE 页。 */
            if ((flags & MAP_SHARED) && !shared_page_ref(pa)) {
                KLOG_ERROR("[mmap] shared page table full: pid=%d va=0x%llx pa=0x%llx\n", current->id, va, pa);
                pmm_free_pages(&g_pmm, pa, 1);
                return MMAP_FAILED;
            }
            if (mm_vm_map_pages(pgd, va, pa, 1, VM_PERM_USER) != 0) {
                KLOG_WARN("[mmap] map failed: va=0x%llx flags=0x%x\n", va, flags);
                if (flags & MAP_SHARED)
                    shared_page_unref(pa);
                else
                    pmm_free_pages(&g_pmm, pa, 1);
                return MMAP_FAILED;
            }
            if (flags & MAP_SHARED) {
                uint64_t *pte = x86_walk_pt(pgd, va, false);
                if (pte)
                    *pte |= PTE_NOFREE;
            }
        }
    }
#endif

    if ((flags & MAP_FIXED) == 0) {
        sync_shared_mmap_next(current, map_addr + size);
    }

    /* KLOG_SYSCALL("[mmap] 0x%llx - 0x%llx (len=0x%llx)\n", map_addr, map_addr + size, len); */
    return map_addr + page_off;
}
