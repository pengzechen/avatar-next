/*
 * fs/lwext4_port/libc_stub.c - 为 lwext4 提供所需的 C 标准库函数存根
 *
 * 当前提供：
 *   - qsort()：ext4_dir_idx.c 用于目录索引排序
 */

#include <types.h>
#include <string.h>

/* ── qsort ──────────────────────────────────────────────────────────────────
 * 使用插入排序实现。lwext4 中调用 qsort 的场合是目录索引排序（通常 ≤数百条
 * 目录项），插入排序在此场景下已足够，且实现简单无递归栈开销。
 */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    if (nmemb <= 1u || size == 0u)
        return;

    uint8_t *arr = (uint8_t *)base;
    /* 使用栈上临时缓冲区（假设 size ≤ 256 字节，满足 ext4 目录项需求） */
    uint8_t tmp[256];
    if (size > sizeof(tmp))
        return;   /* 超出临时缓冲区，安全退出 */

    for (size_t i = 1u; i < nmemb; i++) {
        memcpy(tmp, arr + i * size, size);
        size_t j = i;
        while (j > 0u && compar(arr + (j - 1u) * size, tmp) > 0) {
            memcpy(arr + j * size, arr + (j - 1u) * size, size);
            j--;
        }
        memcpy(arr + j * size, tmp, size);
    }
}
