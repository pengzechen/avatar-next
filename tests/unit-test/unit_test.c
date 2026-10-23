/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit-test/unit_test.c — 内核测试的统一入口

   普通启动什么测试都不跑。只有选中对应的构建变体时，才在固定的启动点执行
   该测试：
     PMM_TEST=1   → RUN_PMM_TESTS，跑完 PMM 测试后关机
     KMEM_TEST=1  → RUN_KMEM_TEST，跑完后继续启动（仅 AArch64 有实现）

   kernel/main.c 只调用 run_unit_tests()，具体测试、架构判断和测试后的动作
   都收在这里，不再散落在 main.c 的 #ifdef 里。
*/

#include "arch.h"
#include "klog.h"
#include "pmm.h"             /* run_pmm_tests() */
#include "platform_ops.h"    /* platform_shutdown() */

#if defined(RUN_KMEM_TEST) && ARCH_AARCH64
extern void kmem_test(void); /* tests/unit-test/kmem_test.c */
#endif

void run_unit_tests(void)
{
#ifdef RUN_PMM_TESTS
    run_pmm_tests();
    platform_shutdown(); /* 避免后续启动继续改写 PMM 状态 */
#endif

#if defined(RUN_KMEM_TEST) && ARCH_AARCH64
    KLOG_INFO("=== KMEM TEST ===\n");
    kmem_test();
    KLOG_INFO("=== KMEM TEST: PASS ===\n");
#endif
}
