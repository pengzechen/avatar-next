/*
 * kernel/mm/shared_page.c - MAP_SHARED 物理页引用计数
 *
 * 这张表会被 mmap、fork、munmap、进程退出四条路径、多个 CPU 并发访问，
 * 所以用一把 IRQ-safe 自旋锁保护；并用哈希桶代替线性扫描：fork 一个带
 * 大量共享页的进程时，每页一次 ref，线性扫描会退化成 O(n^2)。
 *
 * 表满时 ref 返回 false，**不**静默丢弃引用。旧实现里表满后调用方照样
 * 把页标成 NOFREE，而 unref 找不到记录时又会直接 pmm_free_pages() ——
 * 等于把一个还有别的进程在用的页还给了分配器。现在 unref 对未知地址
 * 只报错、不释放：找不到账本说明引用记账已经坏了，继续 free 只会把数据
 * 损坏推迟到更难查的时候。
 */

#include "shared_page.h"
#include "klog.h"
#include "page.h"
#include "pmm.h"
#include "spinlock.h"

/*
 * 表容量：4096 页 = 16 MiB 共享页。旧实现只有 256 项，一个 1 MiB 的
 * 共享缓冲就能把表占满，然后触发上面说的静默丢引用。
 *
 * 桶数取 2 的幂，平均链长 = 4096 / 512 = 8，查找代价与容量基本无关。
 */
#define SHARED_PAGE_BUCKETS 512u
#define SHARED_PAGE_MAX     4096u

typedef struct {
    uint64_t paddr;
    uint32_t refcount;
    int32_t next; /* 桶链或空闲链的下标 */
} shared_page_entry_t;

static shared_page_entry_t g_entries[SHARED_PAGE_MAX];
static int32_t g_buckets[SHARED_PAGE_BUCKETS];
static int32_t g_free_head;
static bool g_initialized;
static spinlock_t g_lock = SPINLOCK_INIT;

static void shared_pages_init_locked(void)
{
    if (g_initialized)
        return;

    for (uint32_t i = 0; i < SHARED_PAGE_BUCKETS; i++)
        g_buckets[i] = -1;

    for (uint32_t i = 0; i < SHARED_PAGE_MAX; i++) {
        g_entries[i].paddr = 0;
        g_entries[i].refcount = 0;
        g_entries[i].next = (i + 1 < SHARED_PAGE_MAX) ? (int32_t)(i + 1) : -1;
    }
    g_free_head = 0;
    g_initialized = true;
}

static uint32_t shared_page_bucket(uint64_t paddr)
{
    /*
     * 页号混合一下再取低位。物理页号通常很密集，直接 & 掩码也能用；
     * 这里多一步乘法是为了避免低地址的大段连续页挤在少数桶里。
     */
    uint64_t x = paddr >> PAGE_SHIFT;

    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t)(x & (SHARED_PAGE_BUCKETS - 1));
}

static int32_t shared_page_find_locked(uint64_t paddr)
{
    uint32_t bucket = shared_page_bucket(paddr);

    for (int32_t i = g_buckets[bucket]; i >= 0; i = g_entries[i].next) {
        if (g_entries[i].paddr == paddr)
            return i;
    }
    return -1;
}

bool shared_page_ref(uint64_t paddr)
{
    if (paddr == 0 || (paddr & (PAGE_SIZE - 1)) != 0)
        return false;

    uint64_t flags;
    spin_lock_irqsave(&g_lock, &flags);
    shared_pages_init_locked();

    int32_t idx = shared_page_find_locked(paddr);
    if (idx >= 0) {
        if (g_entries[idx].refcount == 0xFFFFFFFFu) {
            spin_unlock_irqrestore(&g_lock, flags);
            return false;
        }
        g_entries[idx].refcount++;
        spin_unlock_irqrestore(&g_lock, flags);
        return true;
    }

    if (g_free_head < 0) {
        spin_unlock_irqrestore(&g_lock, flags);
        return false;
    }

    idx = g_free_head;
    g_free_head = g_entries[idx].next;

    g_entries[idx].paddr = paddr;
    g_entries[idx].refcount = 1;

    uint32_t bucket = shared_page_bucket(paddr);
    g_entries[idx].next = g_buckets[bucket];
    g_buckets[bucket] = idx;

    spin_unlock_irqrestore(&g_lock, flags);
    return true;
}

void shared_page_unref(uint64_t paddr)
{
    if (paddr == 0)
        return;

    if ((paddr & (PAGE_SIZE - 1)) != 0) {
        KLOG_ERROR(
            "[shared_page] unref of unaligned paddr=0x%llx; not freeing\n",
            paddr);
        return;
    }

    uint64_t flags;
    spin_lock_irqsave(&g_lock, &flags);
    shared_pages_init_locked();

    uint32_t bucket = shared_page_bucket(paddr);
    int32_t prev = -1;
    int32_t idx = g_buckets[bucket];
    while (idx >= 0 && g_entries[idx].paddr != paddr) {
        prev = idx;
        idx = g_entries[idx].next;
    }

    if (idx < 0) {
        spin_unlock_irqrestore(&g_lock, flags);
        KLOG_ERROR("[shared_page] unref of untracked paddr=0x%llx; not freeing\n",
                   paddr);
        return;
    }

    if (g_entries[idx].refcount > 1) {
        g_entries[idx].refcount--;
        spin_unlock_irqrestore(&g_lock, flags);
        return;
    }

    /* 最后一个持有者：先把账本摘掉，再释放物理页。 */
    if (prev < 0)
        g_buckets[bucket] = g_entries[idx].next;
    else
        g_entries[prev].next = g_entries[idx].next;

    g_entries[idx].paddr = 0;
    g_entries[idx].refcount = 0;
    g_entries[idx].next = g_free_head;
    g_free_head = idx;

    /*
     * 持锁释放：否则"摘账本 → 解锁 → free"之间，另一个 CPU 可能为同一
     * paddr 新建一条记录，然后这个页被我们释放掉 —— 账本指向已回收的页。
     * 锁序固定为 shared_page -> pmm，PMM 路径不会反向调用本模块。
     */
    pmm_free_pages(&g_pmm, paddr, 1);

    spin_unlock_irqrestore(&g_lock, flags);
}
