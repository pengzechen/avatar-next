/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/syscall/elf_loader.h - ELF 程序加载器
*/

#ifndef KERNEL_SYSCALL_ELF_LOADER_H
#define KERNEL_SYSCALL_ELF_LOADER_H

#include "types.h"
#include "exception.h" /* trap_frame_t */

/*
   elf_loader_load_from_file - 从文件系统加载并执行 ELF 程序(原地 exec)
   @pathname: 程序路径
   @argv: 参数数组
   @envp: 环境变量数组
   @frame:    syscall 路径的 trap frame; 内核线程加载器传 NULL

   支持静态链接的 ELF PIE 可执行文件。

   返回：失败返回负错误码；成功时用户进程返回 0(syscall 层随后返回新
   程序入口), 内核线程不返回。
*/
int elf_loader_load_from_file(const char *pathname, char **argv, char **envp, trap_frame_t *frame);

#endif /* KERNEL_SYSCALL_ELF_LOADER_H */
