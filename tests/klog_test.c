/*
 * 内核日志系统测试
 * 编译: gcc -D__x86_64__ -I../include -c klog_test.c
 */

#include "klog.h"
#include "types.h"

/* 测试不同日志级别 */
void
test_log_levels(void)
{
    /* ERROR 级别 - 总是显示 */
    KLOG_ERROR("This is an error message: %s\n", "test error");

    /* WARN 级别 - LOG>=warn 时显示 */
    KLOG_WARN("This is a warning message: %s\n", "test warning");

    /* INFO 级别 - LOG>=info 时显示 */
    KLOG_INFO("This is an info message: %s\n", "test info");

    /* DEBUG 级别 - LOG>=debug 时显示 */
    KLOG_DEBUG("This is a debug message: %s\n", "test debug");

    /* TRACE 级别 - LOG==trace 时显示 */
    KLOG_TRACE("This is a trace message: %s\n", "test trace");
}

/* 测试模块日志 */
void
test_module_logs(void)
{
    /* 启用 UART 和 TIMER 模块的调试日志 */
    log_set_modules(LOG_MODULE_UART | LOG_MODULE_TIMER);

    /* 模块初始化日志 */
    KLOG_INIT("Init subsystem starting...");
    KLOG_INIT("Memory manager initialized");

    /* UART 驱动日志 */
    KLOG_UART("UART initialized at 115200 baud");
    KLOG_UART("TX buffer size: %d bytes", 256);

    /* 定时器日志 */
    KLOG_TIMER("Timer tick: %d", 100);
    KLOG_TIMER("Next interrupt in: %d us", 500);
}

/* 测试动态日志级别切换 */
void
test_dynamic_level(void)
{
    KLOG_INFO("Setting log level to DEBUG\n");
    set_log_level(LOG_LEVEL_DEBUG);

    KLOG_DEBUG("This debug message should be visible\n");
    KLOG_TRACE("This trace message should NOT be visible\n");

    KLOG_INFO("Setting log level to TRACE\n");
    set_log_level(LOG_LEVEL_TRACE);

    KLOG_DEBUG("This debug message should be visible\n");
    KLOG_TRACE("This trace message should NOW be visible\n");
}

/* 测试带格式的日志 */
void
test_formatted_logs(void)
{
    int value = 42;
    void *ptr = (void *)0xDEADBEEF;

    KLOG_INFO("Value: %d\n", value);
    KLOG_INFO("Pointer: %p\n", ptr);
    KLOG_ERROR("Failed at %s:%d\n", __FILE__, __LINE__);

    /* 避免未使用警告 */
    (void)value;
    (void)ptr;
}

/* 主测试函数 */
void
run_klog_tests(void)
{
    KLOG_INFO("=== Kernel Log System Test ===\n");

    KLOG_INFO("\n--- Testing Log Levels ---\n");
    test_log_levels();

    KLOG_INFO("\n--- Testing Module Logs ---\n");
    test_module_logs();

    KLOG_INFO("\n--- Testing Dynamic Level Switching ---\n");
    test_dynamic_level();

    KLOG_INFO("--- Testing Formatted Logs ---\n");
    test_formatted_logs();

    KLOG_INFO("=== Test Complete ===\n");
}
