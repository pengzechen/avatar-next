#ifndef TYPES_H_
#define TYPES_H_

/*
 * 基础类型定义
 * 仅支持 64 位架构：AArch64, RISC-V 64, x86_64
 */

/* ===== 整数类型 ===== */

typedef unsigned char        uint8_t;
typedef unsigned short       uint16_t;
typedef unsigned int         uint32_t;
typedef unsigned long long   uint64_t;

typedef char        int8_t;
typedef short       int16_t;
typedef int         int32_t;
typedef long long   int64_t;

/* ===== 布尔类型 ===== */

typedef _Bool bool;
#define true  1
#define false 0

/* ===== 指针相关类型 ===== */

/* 64 位系统中 size_t 为 unsigned long */
typedef unsigned long size_t;
typedef long          ssize_t;

/* 指针到整数的类型 */
typedef unsigned long uintptr_t;
typedef long          intptr_t;

/* 指针差值类型 */
typedef long ptrdiff_t;

/* ===== 虚拟/物理地址类型 ===== */

typedef uint64_t vaddr_t;  /* 虚拟地址 */
typedef uint64_t paddr_t;  /* 物理地址 */

/* ===== 常量定义 ===== */

#define NULL ((void *)0)

/* 类型最大值/最小值 */
#define INT8_MAX   (127)
#define INT8_MIN   (-128)
#define UINT8_MAX  (255U)

#define INT16_MAX  (32767)
#define INT16_MIN  (-32768)
#define UINT16_MAX (65535U)

#define INT32_MAX  (2147483647L)
#define INT32_MIN  (-2147483648L)
#define UINT32_MAX (4294967295UL)

#define INT64_MAX  (9223372036854775807LL)
#define INT64_MIN  (-9223372036854775808LL)
#define UINT64_MAX (18446744073709551615ULL)

#define SIZE_MAX  (18446744073709551615UL)

/* ===== 通用宏 ===== */

/* 获取数组元素个数 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* 获取结构体成员偏移 */
#define offsetof(type, member) __builtin_offsetof(type, member)

/* 获取结构体指针（通过成员指针） */
#define container_of(ptr, type, member) __extension__ ({              \
    const __typeof__(((type *)0)->member) *__mptr = (ptr);           \
    (type *)((char *)__mptr - offsetof(type, member));               \
})

/* 类型安全的 MIN/MAX 宏（避免多次求值） */
#define MIN(a, b) __extension__ ({                                    \
    __typeof__(a) _a = (a);                                           \
    __typeof__(b) _b = (b);                                           \
    _a < _b ? _a : _b;                                                \
})

#define MAX(a, b) __extension__ ({                                    \
    __typeof__(a) _a = (a);                                           \
    __typeof__(b) _b = (b);                                           \
    _a > _b ? _a : _b;                                                \
})

/* 限制值在范围内 */
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)

/* 位操作宏 */
#define BIT(n)  (1UL << (n))
#define BIT64(n) (1ULL << (n))

/* 位字段操作 */
#define SET_BIT(mask, bit)   ((mask) |= BIT(bit))
#define CLEAR_BIT(mask, bit) ((mask) &= ~BIT(bit))
#define TEST_BIT(mask, bit)  (!!((mask) & BIT(bit)))

/* 四字节对齐 */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)

/* 判断是否为 2 的幂 */
#define IS_POWER_OF_TWO(x) (((x) != 0) && (((x) & ((x) - 1)) == 0))

/* 向上取整除法 */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* ===== 编译时断言 ===== */

#define STATIC_ASSERT(expr, msg) typedef char static_assertion_##msg[(expr) ? 1 : -1]

/* ===== 属性宏 ===== */

#define PACKED __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))
#define NORETURN __attribute__((noreturn))
#define UNUSED __attribute__((unused))
#define WEAK __attribute__((weak))
#define ALIAS(alias) __attribute__((alias(#alias)))

#endif  // TYPES_H_