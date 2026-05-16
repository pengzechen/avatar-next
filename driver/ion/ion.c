/* driver/ion/ion.c — 裸机 Ion 内存分配器实现
 *
 * 原理：
 *   · 底层通过 pmm_alloc_pages(g_pmm, n) 分配物理连续页
 *   · 通过 phys_to_virt(pa) 获得内核虚拟地址（sv39 直接映射）
 *   · 用固定大小的表（ION_MAX_BUFS 项）追踪 handle → (pa, size)
 *   · handle = 表中下标 + 1（0 为无效哨兵）
 *   · 线程安全：用 spinlock 保护分配表
 *
 * 对应 ref: rcore-os/tgoskits 中的 IonHeapManager + IonBufferManager。
 */
#include "ion.h"
#include "types.h"
#include "mm_vm.h"
#include "pmm.h"
#include "spinlock.h"
#include "klog.h"
#include "string.h"

/* ── 内部条目 ───────────────────────────────────────────────────────────── */
typedef struct {
    uint64_t   paddr;       /* 物理地址（PMM 分配起始） */
    uint32_t   page_count;  /* 分配的页数               */
    size_t     size;        /* 用户请求大小（字节）      */
    bool       used;        /* 是否被占用               */
} ion_entry_t;

static ion_entry_t  g_ion_table[ION_MAX_BUFS];
static spinlock_t   g_ion_lock = SPINLOCK_INIT;
static bool         g_ion_initialized = false;

#define PAGE_SIZE_4K  4096U

/* ── 初始化（懒初始化，第一次调用时触发） ─────────────────────────────── */
static void ion_init_once(void)
{
    if (g_ion_initialized)
        return;
    memset(g_ion_table, 0, sizeof(g_ion_table));
    g_ion_initialized = true;
}

/* ── 辅助：向上对齐到页大小 ────────────────────────────────────────────── */
static inline uint32_t size_to_pages(size_t size)
{
    return (uint32_t)((size + PAGE_SIZE_4K - 1U) / PAGE_SIZE_4K);
}

/* ── ion_alloc ─────────────────────────────────────────────────────────── */
int ion_alloc(size_t size, void **vaddr, uint64_t *paddr, ion_handle_t *handle)
{
    if (!size || !vaddr || !paddr || !handle)
        return -1;

    ion_init_once();

    uint32_t pages = size_to_pages(size);
    if (pages == 0U)
        return -1;

    /* 分配物理内存 */
    if (!g_pmm) {
        KLOG_ERROR("ion_alloc: g_pmm not initialized\n");
        return -1;
    }

    uint64_t pa = pmm_alloc_pages(g_pmm, pages);
    if (pa == 0U) {
        KLOG_ERROR("ion_alloc: pmm_alloc_pages(%u) failed\n", pages);
        return -1;
    }

    /* 找空闲表项 */
    int32_t slot = -1;
    spin_lock(&g_ion_lock);
    for (uint32_t i = 0U; i < ION_MAX_BUFS; i++) {
        if (!g_ion_table[i].used) {
            slot = (int32_t)i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock(&g_ion_lock);
        pmm_free_pages(g_pmm, pa, pages);
        KLOG_ERROR("ion_alloc: table full (max %u buffers)\n", ION_MAX_BUFS);
        return -1;
    }

    g_ion_table[slot].paddr      = pa;
    g_ion_table[slot].page_count = pages;
    g_ion_table[slot].size       = pages * PAGE_SIZE_4K;
    g_ion_table[slot].used       = true;
    spin_unlock(&g_ion_lock);

    /* 清零缓冲区（DMA coherent 语义） */
    void *va = phys_to_virt(pa);
    memset(va, 0, pages * PAGE_SIZE_4K);

    *vaddr  = va;
    *paddr  = pa;
    *handle = (ion_handle_t)(slot + 1U);

    KLOG_DEBUG("ion_alloc: size=%zu pages=%u pa=0x%llx handle=%u\n",
               size, pages, (unsigned long long)pa, *handle);
    return 0;
}

/* ── ion_free ──────────────────────────────────────────────────────────── */
int ion_free(ion_handle_t handle)
{
    if (handle == ION_HANDLE_INVALID)
        return -1;

    ion_init_once();

    uint32_t idx = (uint32_t)handle - 1U;
    if (idx >= ION_MAX_BUFS)
        return -1;

    spin_lock(&g_ion_lock);
    if (!g_ion_table[idx].used) {
        spin_unlock(&g_ion_lock);
        KLOG_ERROR("ion_free: handle %u not allocated\n", handle);
        return -1;
    }

    uint64_t pa     = g_ion_table[idx].paddr;
    uint32_t pages  = g_ion_table[idx].page_count;
    memset(&g_ion_table[idx], 0, sizeof(ion_entry_t));
    spin_unlock(&g_ion_lock);

    pmm_free_pages(g_pmm, pa, pages);
    KLOG_DEBUG("ion_free: handle=%u pa=0x%llx pages=%u\n",
               handle, (unsigned long long)pa, pages);
    return 0;
}

/* ── ion_get_buf ───────────────────────────────────────────────────────── */
int ion_get_buf(ion_handle_t handle, void **vaddr, uint64_t *paddr)
{
    if (handle == ION_HANDLE_INVALID)
        return -1;

    ion_init_once();

    uint32_t idx = (uint32_t)handle - 1U;
    if (idx >= ION_MAX_BUFS)
        return -1;

    spin_lock(&g_ion_lock);
    if (!g_ion_table[idx].used) {
        spin_unlock(&g_ion_lock);
        return -1;
    }
    uint64_t pa = g_ion_table[idx].paddr;
    spin_unlock(&g_ion_lock);

    if (vaddr)
        *vaddr = phys_to_virt(pa);
    if (paddr)
        *paddr = pa;

    return 0;
}

/* ── ion_get_size ──────────────────────────────────────────────────────── */
size_t ion_get_size(ion_handle_t handle)
{
    if (handle == ION_HANDLE_INVALID)
        return 0U;

    ion_init_once();

    uint32_t idx = (uint32_t)handle - 1U;
    if (idx >= ION_MAX_BUFS)
        return 0U;

    spin_lock(&g_ion_lock);
    size_t sz = g_ion_table[idx].used ? g_ion_table[idx].size : 0U;
    spin_unlock(&g_ion_lock);
    return sz;
}
