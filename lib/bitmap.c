/*
 * lib/bitmap.c - 位图管理实现
 */

#include "bitmap.h"
#include "klog.h"

/*
 * bitmap_find_first_free - 查找第一个空闲位
 * @bitmap: 位图结构指针
 *
 * 返回：第一个空闲位的索引，失败返回 (size_t)-1
 */
size_t bitmap_find_first_free(const bitmap_t *bitmap)
{
    for (size_t i = 0; i < bitmap->size; i++) {
        if (!bitmap_test(bitmap, i)) {
            return i;
        }
    }
    return (size_t)-1;
}

/*
 * bitmap_find_contiguous_free - 查找连续的空闲位
 * @bitmap: 位图结构指针
 * @count:  需要的连续位数
 *
 * 返回：起始索引，失败返回 (size_t)-1
 */
size_t bitmap_find_contiguous_free(const bitmap_t *bitmap, size_t count)
{
    if (count == 0 || count > bitmap->size) {
        return (size_t)-1;
    }

    for (size_t i = 0; i + count <= bitmap->size; i++) {
        size_t j;
        for (j = 0; j < count; j++) {
            if (bitmap_test(bitmap, i + j)) {
                break;
            }
        }
        if (j == count) {
            return i;  /* 找到足够的连续空闲位 */
        }
    }
    return (size_t)-1;
}
