#ifndef RISCV64_STRING_IMPL_H
#define RISCV64_STRING_IMPL_H

/*
 * RISC-V 64位架构的字符串函数优化实现
 * 使用 RISC-V 向量扩展（V扩展）优化
 */

/* 使用通用实现 */
static inline void *
memcpy(void *dest, const void *src, size_t n)
{
    return memcpy_generic(dest, src, n);
}

/* TODO: 可以添加 RISC-V V 扩展优化版本
 * static inline void *
 * memcpy_vector(void *dest, const void *src, size_t n)
 * {
 *     // 使用 V 扩展的向量加载/存储指令
 *     // vle.v、vse.v 等
 *     ...
 * }
 */

#endif /* RISCV64_STRING_IMPL_H */
