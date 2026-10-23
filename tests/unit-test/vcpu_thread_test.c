/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   VMM_TEST=1：三线程上下文切换自测。
   run_vmm_test() 在 tests/unit-test/vmm_test.c 里，始终参与内核链接。
*/

#include "../boot_mode.h"
#include "klog.h"

extern void run_vmm_test(void);

bool test_boot_active(void)
{
    return true;
}

bool test_boot_skip_net(void)
{
    return true;
}

void test_boot_run(void)
{
    KLOG_INFO("=== VMM_TEST mode: 3-thread context switch test ===\n");
    run_vmm_test();
}
