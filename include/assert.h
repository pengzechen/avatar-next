#ifndef ASSERT_H_
#define ASSERT_H_

#include "klog.h"

/*
 * 内核断言系统
 *
 * Makefile 参数:
 *   ASSERT=panic  - 断言失败时 panic（DEBUG 模式默认）
 *   ASSERT=off    - 完全禁用断言（RELEASE 模式）
 */

/* ===== 平台 panic 函数声明 ===== */

/*
 * platform_panic - 平台 panic 处理函数
 * 该函数应调用架构特定的 halt 实现来停止系统
 *
 * 注意：此函数由平台层提供，通常实现为：
 *   void platform_panic(void) {
 *       arch_halt();  // 调用架构特定的 halt
 *   }
 */
extern void platform_panic(void);

/* ===== 编译时断言 ===== */

/*
 * static_assert - 编译时断言
 * 如果条件为常量 false，编译时报错
 *
 * 使用示例:
 *   static_assert(sizeof(int) == 4, "int must be 4 bytes");
 */
#define static_assert(cond, msg) _Static_assert(cond, msg)

/* ===== 运行时断言配置 ===== */

/* 默认启用断言（可通过 Makefile 的 ASSERT=off 禁用） */
#if defined(ASSERT_OFF)
    #define ASSERT_ENABLED 0
#elif !defined(ASSERT_ENABLED)
    #define ASSERT_ENABLED 1
#endif

/* ===== 运行时断言实现 ===== */

#if ASSERT_ENABLED

/*
 * assert - 运行时断言
 * 如果条件为 false，输出错误信息并调用 platform_panic()
 *
 * 使用示例:
 *   assert(ptr != NULL);
 *   assert(x > 0);
 */
#define assert(cond) \
    do { \
        if (!(cond)) { \
            KLOG_ERROR("Assertion failed: %s, file %s, line %d", \
                       #cond, __FILE__, __LINE__); \
            platform_panic(); \
        } \
    } while (0)

/*
 * assert_always - 总是启用的断言
 * 即使定义了 ASSERT_OFF，此断言仍然有效
 * 用于关键检查，不应该被禁用
 *
 * 使用示例:
 *   assert_always(ptr != NULL);  // 关键检查，总是启用
 */
#define assert_always(cond) \
    do { \
        if (!(cond)) { \
            KLOG_ERROR("Critical assertion failed: %s, file %s, line %d", \
                       #cond, __FILE__, __LINE__); \
            platform_panic(); \
        } \
    } while (0)

#else

/* ASSERT_OFF 模式：禁用 assert，但保留 assert_always */
#define assert(cond)         ((void)0)
#define assert_always(cond) \
    do { \
        if (!(cond)) { \
            KLOG_ERROR("Critical assertion failed: %s, file %s, line %d", \
                       #cond, __FILE__, __LINE__); \
            platform_panic(); \
        } \
    } while (0)

#endif

/* ===== 调试辅助宏 ===== */

/*
 * assert_not_reached - 声明代码不应该执行到这里
 * 如果执行到这里，说明有逻辑错误
 */
#define assert_not_reached() \
    do { \
        KLOG_ERROR("Code should not reach here: file %s, line %d", \
                   __FILE__, __LINE__); \
        platform_panic(); \
    } while (0)

/*
 * assert_unreachable(expr) - 声明表达式永远不会为真
 * 用于标记不可能的代码路径
 */
#define assert_unreachable(expr) assert(!(expr))

#endif  // ASSERT_H_
