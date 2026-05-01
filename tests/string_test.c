/*
 * 字符串函数测试
 * 编译: gcc -D__x86_64__ -I../include -c string_test.c
 */

#include "string.h"
#include "types.h"
#include "klog.h"

/* 简单测试函数 */
void test_string_functions(void)
{
    char    buf[64];
    int     result;

    KLOG_INFO("Starting string test...");

    /* strlen 测试 */
    KLOG_INFO("Testing strlen...");
    if (strlen("hello") != 5) {
        /* strlen 失败 */
    }

    /* strcpy 测试 */
    KLOG_INFO("Testing strcpy...");
    strcpy(buf, "test");
    if (strlen(buf) != 4) {
        /* strcpy 失败 */
    }

    /* strcat 测试 */
    KLOG_INFO("Testing strcat...");
    strcpy(buf, "hello ");
    strcat(buf, "world");
    if (strlen(buf) != 11) {
        /* strcat 失败 */
    }

    /* strcmp 测试 */
    KLOG_INFO("Testing strcmp...");
    result = strcmp("abc", "abc");
    if (result != 0) {
        /* strcmp 相等测试失败 */
    }

    result = strcmp("abc", "abd");
    if (result >= 0) {
        /* strcmp 小于测试失败 */
    }

    /* memset 测试 */
    KLOG_INFO("Testing memset...");
    memset(buf, 0, sizeof(buf));
    if (buf[0] != 0 || buf[63] != 0) {
        /* memset 失败 */
    }

    /* memcpy 测试 */
    KLOG_INFO("Testing memcpy...");
    char src[] = "hello world";
    memcpy(buf, src, sizeof(src));
    if (strlen(buf) != strlen(src)) {
        /* memcpy 失败 */
    }

    /* memcmp 测试 */
    KLOG_INFO("Testing memcmp...");
    result = memcmp("abc", "abc", 3);
    if (result != 0) {
        /* memcmp 相等测试失败 */
    }

    /* atol 测试 */
    KLOG_INFO("Testing atol...");
    if (atol("12345") != 12345) {
        /* atol 失败 */
    }

    KLOG_INFO("String test complete!");
}
