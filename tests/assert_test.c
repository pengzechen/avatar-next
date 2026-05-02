/*
 * 断言系统测试
 * 编译: gcc -D__x86_64__ -I../include -c assert_test.c
 */

#include "assert.h"
#include "types.h"

/* platform_panic is now provided by the platform layer */

/* 测试基本断言 */
void
test_basic_assert(void)
{
    int x = 42;
    int y = 0;

    /* 这个断言会成功 */
    assert(x > 0);

    /* 这个断言会失败并 panic */
    /* assert(y != 0);  这里会触发 panic - 已禁用 */
    (void)y;  /* 避免未使用警告 */
}

/* 测试总是启用的断言 */
void
test_assert_always(void)
{
    void *ptr = (void *)0x1000;  /* 使用非空指针避免 panic */

    /* 即使 ASSERT=off，这个断言仍然有效 */
    assert_always(ptr != NULL);  /* 这里会触发 panic - 已修复 */
}

/* 测试编译时断言 */
void
test_static_assert(void)
{
    /* 编译时检查，如果失败则编译报错 */
    static_assert(sizeof(int) == 4, "int must be 4 bytes");
    static_assert(sizeof(long) == 8, "long must be 8 bytes on 64-bit");
    static_assert(sizeof(void *) == 8, "pointer must be 8 bytes on 64-bit");
}

/* 测试辅助宏 */
void
test_helper_macros(void)
{
    int value = 100;

    /* 条件检查 */
    if (value < 0) {
        assert_not_reached();  /* 不应该执行到这里 */
    }

    /* 不可能的路径 */
    switch (value) {
        case 0:
        case 100:
            break;
        default:
            assert_unreachable(1);  /* 不可能到达 */
    }
}

/* 测试数组边界 */
void
test_array_bounds(void)
{
    int arr[10];
    size_t index = 5;

    /* 检查数组索引 */
    assert(index < ARRAY_SIZE(arr));
    arr[index] = 42;
}

/* 测试指针有效性 */
void
test_pointer_validity(void)
{
    void *ptr = (void *)0x1000;

    /* 检查指针对齐 */
    assert(IS_ALIGNED((uintptr_t)ptr, 8));

    /* 检查指针不为空 */
    assert(ptr != NULL);
}

/* 测试算术假设 */
void
test_arithmetic_assumptions(void)
{
    int a = 10, b = 20;

    /* 假设检查 */
    assert(a + b > a);
    assert(b > a);

    /* 验证结果 */
    int result = a * b;
    assert(result == 200);
}

/* 主测试函数 */
void
run_assert_tests(void)
{
    KLOG_INFO("=== Assert System Test ===\n");

    /* 注意：以下测试会触发 panic！ */

    /* test_basic_assert();      // 会 panic */
    /* test_assert_always();     // 会 panic */
    test_static_assert();        /* 编译时检查 */
    /* test_helper_macros();     // 可能 panic */
    test_array_bounds();         /* 应该成功 */
    test_pointer_validity();     /* 应该成功 */
    test_arithmetic_assumptions();  /* 应该成功 */

    KLOG_INFO("=== Non-panic tests passed ===\n");
}
