/*
 * mm/brk.c - sys_brk 实现
 *
 * 从 kernel/syscall/syscall.c 抽出。语义同 Linux brk(2)：
 *   addr == 0 -> 查询当前堆末尾
 *   addr <  current_brk -> 收缩
 *   addr >  current_brk -> 按页扩展（分配并清零物理页）
 * 失败返回 current_brk（旧值）。
 */
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "klog.h"
#include "task/task.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "arch.h"
#include "vm_user.h"

extern task_t g_task_pool[];
extern uint8_t g_stack_used[];

static uint64_t shared_heap_end(task_t *current)
{
    uint64_t heap_end = current->heap_end;

    for (uint32_t i = 0; i < TASK_MAX; i++) {
        task_t *task = &g_task_pool[i];
        if (!g_stack_used[i] || !task->is_user_process || task->state == TASK_DEAD)
            continue;
        if (task->pgd == current->pgd && task->heap_end > heap_end)
            heap_end = task->heap_end;
    }

    return heap_end;
}

static void sync_shared_heap_end(task_t *current, uint64_t heap_end)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        task_t *task = &g_task_pool[i];
        if (!g_stack_used[i] || !task->is_user_process || task->state == TASK_DEAD)
            continue;
        if (task->pgd == current->pgd)
            task->heap_end = heap_end;
    }
}

void *sys_brk(void *addr)
{
    task_t  *current = task_current();

    if (!current->is_user_process) {
        return (void *)(uint64_t)(int64_t)-12; /* ENOMEM */
    }

    uint64_t current_brk = shared_heap_end(current);
    uint64_t req_brk = (uint64_t)addr;

    if (g_syscall_entry_count <= 16) {
        KLOG_DEBUG("[brk] req=0x%llx current=0x%llx\n", req_brk, current_brk);
    }

    /* brk(0)：查询当前堆末尾 */
    if ((uint64_t)addr == 0) {
        if (g_syscall_entry_count <= 16) {
            KLOG_DEBUG("[brk] query -> 0x%llx\n", current_brk);
        }
        return (void *)current_brk;
    }

    uint64_t new_brk = (uint64_t)addr;

    /* 缩小或不变：释放高水位以上页后更新 */
    if (new_brk <= current_brk) {
        uint64_t old_page_end = ALIGN_UP(current_brk, PAGE_SIZE);
        uint64_t new_page_end = ALIGN_UP(new_brk,     PAGE_SIZE);
        if (new_page_end < old_page_end)
            vm_unmap_user_range((uint64_t)current->pgd,
                                new_page_end,
                                old_page_end - new_page_end);
        sync_shared_heap_end(current, new_brk);
        if (g_syscall_entry_count <= 16) {
            KLOG_DEBUG("[brk] shrink/no-grow -> 0x%llx\n", new_brk);
        }
        return (void *)new_brk;
    }

#if ARCH_AARCH64 || ARCH_RISCV64
    /* 扩展堆：映射新页 */
    uint64_t old_page_end = ALIGN_UP(current_brk, PAGE_SIZE);
    uint64_t new_page_end = ALIGN_UP(new_brk,     PAGE_SIZE);
    void    *pgd          = phys_to_virt((uint64_t)current->pgd);

    for (uint64_t va = old_page_end; va < new_page_end; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_pages(g_pmm, 1);
        if (pa == 0) {
            KLOG_ERROR("[brk] Out of memory at va=0x%llx\n", va);
            return (void *)current_brk; /* 返回旧地址表示失败 */
        }
        memset(phys_to_virt(pa), 0, PAGE_SIZE);
        if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
            /* Page already mapped (e.g. BSS last page overlap) — skip */
            pmm_free_pages(g_pmm, pa, 1);
            continue;
        }
    }
#elif ARCH_X86_64
    /* x86_64：扩展堆，使用 g_pmm */
    {
        uint64_t old_page_end = ALIGN_UP(current_brk, PAGE_SIZE);
        uint64_t new_page_end = ALIGN_UP(new_brk,     PAGE_SIZE);
        void    *pgd          = phys_to_virt((uint64_t)current->pgd);

        for (uint64_t va = old_page_end; va < new_page_end; va += PAGE_SIZE) {
            uint64_t pa = pmm_alloc_pages(g_pmm, 1);
            if (pa == 0) {
                KLOG_ERROR("[brk] Out of memory at va=0x%llx\n", va);
                return (void *)current_brk;
            }
            memset(phys_to_virt(pa), 0, PAGE_SIZE);
            if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
                pmm_free_pages(g_pmm, pa, 1);
                continue; /* 已映射：跳过 */
            }
        }
    }
#endif

    sync_shared_heap_end(current, new_brk);
    if (g_syscall_entry_count <= 16) {
        KLOG_DEBUG("[brk] grow success -> 0x%llx\n", new_brk);
    }
    return (void *)new_brk;
}

void *sys_sbrk(int64_t increment)
{
    /* 简单实现：维护一个简单的堆指针
     * TODO: 实现真正的堆管理器
     */
    static uint64_t heap_end = 0x50000000;
    void *old_brk = (void *)heap_end;

    if (increment == 0) {
        return old_brk;
    }

    heap_end += increment;

    KLOG_DEBUG("[syscall] sbrk(%lld) = 0x%llx\n", increment, old_brk);
    return old_brk;
}
