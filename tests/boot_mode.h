/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/boot_mode.h — 内核测试启动模式的通用钩子

   kernel/main.c 只调用这三个函数，不包含任何 guest/VMM 测试细节。
   具体变体链接不同的 tests/unit-test/{guest_test,vcpu_thread_test}.c：

     默认实现            在 kernel/main.c 里，以 weak 符号提供（正常 busybox）
     guest_test.c        GUEST_LINUX=1  直启 Linux guest
     vcpu_thread_test.c  VMM_TEST=1     VMM 三线程自测

   变体由 Makefile 的 GUEST_LINUX= / VMM_TEST= 选择链接哪个 boot 对象；
   强定义在链接时覆盖 main.c 的 weak 默认实现，main.c 里没有 RUN_* 宏。
*/

#ifndef TESTS_BOOT_MODE_H
#define TESTS_BOOT_MODE_H

#include "types.h"

/* 当前变体是否接管启动流程 */
bool test_boot_active(void);

/* 是否跳过网络驱动/网络栈初始化（guest 直启和 VMM 自测都不需要） */
bool test_boot_skip_net(void);

/* 执行该变体的启动动作；只有 active 为 true 时调用 */
void test_boot_run(void);

#endif /* TESTS_BOOT_MODE_H */
