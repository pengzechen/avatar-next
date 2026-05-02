#ifndef BITMAP_H
#define BITMAP_H

#include "types.h"

/*
 * include/bitmap.h - 位图管理
 *
 * 用于物理内存管理的位图数据结构
 */

/* ── 位图结构 ───────────────────────────────────────────────────── */

typedef struct {
    uint8_t *bits;  /* 位图缓冲区               */
    size_t   size;  /* 位图大小（以 bit 为单位） */
} bitmap_t;

/* ── 位图 API ───────────────────────────────────────────────────── */

/**
 * bitmap_init - 初始化位图
 * @bitmap: 位图结构指针
 * @buffer: 位图缓冲区
 * @size:   位图大小（bit 数）
 *
 * 使用前必须调用此函数初始化位图。
 */
static inline void bitmap_init(bitmap_t *bitmap, uint8_t *buffer, size_t size)
{
    bitmap->bits = buffer;
    bitmap->size = size;

    /* 清空所有位 */
    size_t bytes = (size + 7) / 8;
    for (size_t i = 0; i < bytes; i++) {
        buffer[i] = 0;
    }
}

/**
 * bitmap_set - 设置指定索引的位（标记为已分配）
 * @bitmap: 位图结构指针
 * @index:  位索引
 *
 * 注意：假设该位当前为 0（未分配）
 */
static inline void bitmap_set(bitmap_t *bitmap, size_t index)
{
    if (index < bitmap->size) {
        bitmap->bits[index / 8] |= (1 << (index % 8));
    }
}

/**
 * bitmap_set_range - 设置指定范围的位
 * @bitmap: 位图结构指针
 * @start:  起始索引
 * @count:  位数量
 *
 * 标记多个连续的位为已分配。
 */
static inline void bitmap_set_range(bitmap_t *bitmap, size_t start, size_t count)
{
    if (start + count > bitmap->size) {
        return;  /* 超出范围 */
    }

    for (size_t i = 0; i < count; i++) {
        bitmap_set(bitmap, start + i);
    }
}

/**
 * bitmap_clear - 清除指定索引的位（标记为未分配）
 * @bitmap: 位图结构指针
 * @index:  位索引
 */
static inline void bitmap_clear(bitmap_t *bitmap, size_t index)
{
    if (index < bitmap->size) {
        bitmap->bits[index / 8] &= ~(1 << (index % 8));
    }
}

/**
 * bitmap_clear_range - 清除指定范围的位
 * @bitmap: 位图结构指针
 * @start:  起始索引
 * @count:  位数量
 */
static inline void bitmap_clear_range(bitmap_t *bitmap, size_t start, size_t count)
{
    if (start + count > bitmap->size) {
        return;  /* 超出范围 */
    }

    for (size_t i = 0; i < count; i++) {
        bitmap_clear(bitmap, start + i);
    }
}

/**
 * bitmap_test - 测试指定索引的位
 * @bitmap: 位图结构指针
 * @index:  位索引
 *
 * 返回：非零表示已分配，0 表示未分配
 */
static inline uint8_t bitmap_test(const bitmap_t *bitmap, size_t index)
{
    if (index < bitmap->size) {
        return (bitmap->bits[index / 8] & (1 << (index % 8))) != 0;
    }
    return 0;
}

/* ── 位图查找函数（需要在 .c 文件中实现）──────────────────────────── */

/**
 * bitmap_find_first_free - 查找第一个空闲位
 * @bitmap: 位图结构指针
 *
 * 返回：第一个空闲位的索引，失败返回 (size_t)-1
 */
size_t bitmap_find_first_free(const bitmap_t *bitmap);

/**
 * bitmap_find_contiguous_free - 查找连续的空闲位
 * @bitmap: 位图结构指针
 * @count:  需要的连续位数
 *
 * 返回：起始索引，失败返回 (size_t)-1
 */
size_t bitmap_find_contiguous_free(const bitmap_t *bitmap, size_t count);

#endif /* BITMAP_H */
