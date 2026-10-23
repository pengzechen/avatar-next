/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/boot.h — 架构引导代码与 C 内核入口之间的公共声明
*/

#ifndef BOOT_H
#define BOOT_H

/*
   C 内核入口。由各架构 boot.S 调用, 不返回。
*/
void kernel_main(void);

#endif /* BOOT_H */
