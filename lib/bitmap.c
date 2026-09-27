/*
 * lib/bitmap.c - 位图管理实现
 */

#include "bitmap.h"

/*
 * bitmap_find_first_free - 查找第一个空闲位
 * @bitmap: 位图结构指针
 *
 * 返回：第一个空闲位的索引，失败返回 (size_t)-1
 */
size_t bitmap_find_first_free(const bitmap_t *bitmap) {
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

/*
 * bitmap_find_contiguous_free_from - 从 start 开始查找连续的空闲位
 *
 * 与 bitmap_find_contiguous_free 的区别：从 @start 起扫，而不是每次都从 0。
 *
 * 为什么需要（实测）：PMM 用它分配物理页。低地址被内核和 rootfs 保留区
 * 占着（约 66048 页），而每次都从 0 开始找的话，每分配一页都要白扫那 66048
 * 位 —— 加载 38 MB 内核要 9437 次分配，累计约 6 亿次位测试，TCG 下白白
 * 多花近 2 秒（guest 自报启动 1.8s、墙上却有 3.6s，差额就是它）。
 *
 * 语义与调用方的回绕约定：只在 [start, size) 里找；调用方负责在失败时
 * 从 0 再找一遍，保证不会漏掉 start 之前的空洞。
 */
size_t bitmap_find_contiguous_free_from(const bitmap_t *bitmap, size_t count,
                                        size_t start) {
  if (count == 0 || count > bitmap->size) {
    return (size_t)-1;
  }
  if (start + count > bitmap->size) {
    return (size_t)-1;          /* 起点太靠后，交给调用方回绕 */
  }

  for (size_t i = start; i + count <= bitmap->size; i++) {
    size_t j;
    for (j = 0; j < count; j++) {
      if (bitmap_test(bitmap, i + j)) {
        i += j;                 /* 跳过已占位，别一位一位挪 */
        break;
      }
    }
    if (j == count) {
      return i;
    }
  }
  return (size_t)-1;
}

size_t bitmap_find_contiguous_free(const bitmap_t *bitmap, size_t count) {
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
      return i; /* 找到足够的连续空闲位 */
    }
  }
  return (size_t)-1;
}
