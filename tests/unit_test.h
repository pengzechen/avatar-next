/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/unit_test.h — 默认启动路径上的内核自检入口

   `kernel/main.c` 只调用 `run_unit_tests()`，不包含具体测试、日志和架构判断。
   具体跑哪些自检由 `tests/unit-test/unit_test.c` 决定。
*/

#ifndef TESTS_UNIT_TEST_H
#define TESTS_UNIT_TEST_H

/* 运行"每次正常启动都应该跑"的内核自检；没有自检的架构为空实现。 */
void run_unit_tests(void);

#endif /* TESTS_UNIT_TEST_H */
