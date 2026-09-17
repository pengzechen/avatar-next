#ifndef ASSERT_H_
#define ASSERT_H_

#include "klog.h"

/*
 * 内核断言系统
 *
 * 断言始终启用，没有编译期开关。断言失败时输出错误信息并 panic。
 *
 * 若某处检查在发布版本中确实需要去掉，请直接删除那一行断言，
 * 而不是引入"整包禁用"的开关——实测全内核禁用只能省 1% 的 .text，
 * 却会让真正的 bug 静默通过。
 */

/* ===== 平台 panic 函数声明 ===== */

/*
 * platform_panic - 平台 panic 处理函数，由平台层实现
 *
 * 当前实现见 platforms/qemu/qemu_platform.c：关中断后死循环停机，不会返回。
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

/* ===== 运行时断言 ===== */

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
 * assert_always - 关键路径断言
 *
 * 与 assert 行为一致，仅错误信息措辞不同，用于标注"这里失败说明
 * 内核核心状态已被破坏"的检查点。
 *
 * 注意：历史上它存在的意义是"即使 ASSERT=off 也生效"，该开关已移除，
 * 因此现在它与 assert 完全等价。
 *
 * 使用示例:
 *   assert_always(!in_irq_context());
 */
#define assert_always(cond) \
    do { \
        if (!(cond)) { \
            KLOG_ERROR("Critical assertion failed: %s, file %s, line %d", \
                       #cond, __FILE__, __LINE__); \
            platform_panic(); \
        } \
    } while (0)

#endif  // ASSERT_H_
