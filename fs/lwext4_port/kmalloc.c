/*
 * fs/lwext4_port/kmalloc.c - lwext4 内存分配器
 *
 * 为 lwext4 提供 ext4_user_malloc / ext4_user_free / ext4_user_calloc /
 * ext4_user_realloc 四个接口，通过一个静态堆上的 first-fit free-list
 * 分配器实现，不依赖任何操作系统服务。
 *
 * 堆大小默认 512KB，可通过编译选项 -DLWEXT4_HEAP_SIZE=<bytes> 调整。
 */

#include <types.h>
#include <string.h>

#ifndef LWEXT4_HEAP_SIZE
#define LWEXT4_HEAP_SIZE  (512UL * 1024UL)   /* 512 KB */
#endif

/* ── 内部对齐 ──────────────────────────────────────────────────── */
#define ALIGN_SIZE       16u
#define HEAP_ALIGN_UP(n) (((n) + (ALIGN_SIZE - 1u)) & ~(ALIGN_SIZE - 1u))

/* ── 块头 ───────────────────────────────────────────────────────── */
typedef struct blk_hdr {
    uint32_t        size;   /* 可用数据区大小（不含 blk_hdr 自身） */
    uint32_t        free;   /* 1=空闲, 0=已分配                    */
    struct blk_hdr *next;   /* 链表中的下一个块                    */
} blk_hdr_t;

/* ── 静态堆 ─────────────────────────────────────────────────────── */
static uint8_t   heap_mem[LWEXT4_HEAP_SIZE] __attribute__((aligned(ALIGN_SIZE)));
static blk_hdr_t *heap_list = NULL;

/* ── 堆初始化（懒初始化，第一次 malloc 时触发）─────────────────── */
static void heap_init(void)
{
    heap_list        = (blk_hdr_t *)heap_mem;
    heap_list->size  = (uint32_t)(LWEXT4_HEAP_SIZE - sizeof(blk_hdr_t));
    heap_list->free  = 1u;
    heap_list->next  = NULL;
}

/* ── ext4_user_malloc ───────────────────────────────────────────── */
void *ext4_user_malloc(size_t size)
{
    if (size == 0u)
        return NULL;

    if (!heap_list)
        heap_init();

    size = HEAP_ALIGN_UP(size);

    blk_hdr_t *blk = heap_list;
    while (blk) {
        if (blk->free && blk->size >= (uint32_t)size) {
            /* 若剩余空间足够容纳新的块头 + 最小分配单元，则分裂 */
            uint32_t split_threshold = (uint32_t)(sizeof(blk_hdr_t) + ALIGN_SIZE);
            if (blk->size >= (uint32_t)size + split_threshold) {
                blk_hdr_t *next = (blk_hdr_t *)((uint8_t *)blk
                                  + sizeof(blk_hdr_t) + size);
                next->size  = blk->size - (uint32_t)size - (uint32_t)sizeof(blk_hdr_t);
                next->free  = 1u;
                next->next  = blk->next;
                blk->size   = (uint32_t)size;
                blk->next   = next;
            }
            blk->free = 0u;
            return (void *)((uint8_t *)blk + sizeof(blk_hdr_t));
        }
        blk = blk->next;
    }
    return NULL;   /* 堆耗尽 */
}

/* ── ext4_user_free ─────────────────────────────────────────────── */
void ext4_user_free(void *ptr)
{
    if (!ptr)
        return;

    blk_hdr_t *blk = (blk_hdr_t *)((uint8_t *)ptr - sizeof(blk_hdr_t));
    blk->free = 1u;

    /* 向后合并相邻空闲块 */
    blk_hdr_t *cur = heap_list;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += (uint32_t)sizeof(blk_hdr_t) + cur->next->size;
            cur->next  = cur->next->next;
            /* 不前进 cur，继续尝试合并 */
        } else {
            cur = cur->next;
        }
    }
}

/* ── ext4_user_calloc ───────────────────────────────────────────── */
void *ext4_user_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void  *p     = ext4_user_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

/* ── ext4_user_realloc ──────────────────────────────────────────── */
void *ext4_user_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return ext4_user_malloc(size);

    if (size == 0u) {
        ext4_user_free(ptr);
        return NULL;
    }

    blk_hdr_t *blk = (blk_hdr_t *)((uint8_t *)ptr - sizeof(blk_hdr_t));

    /* 原块已足够大，直接返回 */
    if ((size_t)blk->size >= HEAP_ALIGN_UP(size))
        return ptr;

    void *newptr = ext4_user_malloc(size);
    if (newptr) {
        memcpy(newptr, ptr, (size_t)blk->size);
        ext4_user_free(ptr);
    }
    return newptr;
}
