/*
 * kernel/mm/shared_page.c — MAP_SHARED 物理页引用计数
 *
 * PTE_NOFREE 标记防止 fork 子进程释放共享页，但当所有持有者
 * 退出后，需要有人释放物理页。此模块为每个 NOFREE 页维护引用
 * 计数，最后一个 unref 调用真正释放物理页。
 */

#include "shared_page.h"
#include "pmm.h"

#define SHARED_PAGE_MAX 256

typedef struct {
    uint64_t paddr;
    uint32_t refcount;
} shared_page_entry_t;

static shared_page_entry_t g_shared_pages[SHARED_PAGE_MAX];

void shared_page_ref(uint64_t paddr)
{
    if (paddr == 0)
        return;

    for (uint32_t i = 0; i < SHARED_PAGE_MAX; i++) {
        if (g_shared_pages[i].paddr == paddr) {
            g_shared_pages[i].refcount++;
            return;
        }
    }

    for (uint32_t i = 0; i < SHARED_PAGE_MAX; i++) {
        if (g_shared_pages[i].refcount == 0) {
            g_shared_pages[i].paddr = paddr;
            g_shared_pages[i].refcount = 1;
            return;
        }
    }
}

void shared_page_unref(uint64_t paddr)
{
    if (paddr == 0)
        return;

    for (uint32_t i = 0; i < SHARED_PAGE_MAX; i++) {
        if (g_shared_pages[i].paddr == paddr && g_shared_pages[i].refcount > 0) {
            g_shared_pages[i].refcount--;
            if (g_shared_pages[i].refcount == 0) {
                g_shared_pages[i].paddr = 0;
                pmm_free_pages(g_pmm, paddr, 1);
            }
            return;
        }
    }

    pmm_free_pages(g_pmm, paddr, 1);
}
