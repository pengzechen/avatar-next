/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit-test/unit_test.c — 默认启动路径上的内核自检入口

   这里只放"每次正常启动都应该跑、失败即说明内核坏了"的自检。会改变机器状态、
   需要专门 rootfs、或只在排障时想跑的测试，由各自的构建变体/手动调用负责，
   不要加到 run_unit_tests()。

   具体测试和架构判断都收在 UT 内，kernel/main.c 只认 run_unit_tests()。
*/

#include "arch.h"
#include "klog.h"

#if ARCH_AARCH64
extern void kmem_test(void); /* tests/unit-test/kmem_test.c */
#endif

void run_unit_tests(void)
{
#if ARCH_AARCH64
    /* 开机自检脚手架：仍然执行, 但默认等级下不再占行 */
    KLOG_INIT("=== Running VMM Tests ===\n");
    kmem_test();
    KLOG_INIT("VMM tests completed\n");
#endif
}
