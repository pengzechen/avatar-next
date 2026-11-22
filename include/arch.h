/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef ARCH_H
#define ARCH_H

/*
   架构检测和配置
   用于在编译时确定目标架构

   注意：本文件只是编译期宏，不是 include/arch/ 目录的索引 ——
   架构特定的头文件统一放在 include/arch/<arch>/ 下。
*/

#if defined(ARCH_X86_64)
#define ARCH_NAME "x86_64"
#elif defined(ARCH_AARCH64)
#define ARCH_NAME "aarch64"
#elif defined(ARCH_RISCV64)
#define ARCH_NAME "riscv64"
#else
#error "Unsupported architecture - please define ARCH_X86_64, ARCH_AARCH64 or ARCH_RISCV64"
#endif

/*
   架构特定的头文件路径(include 根下的相对路径)
*/
#define ARCH_INCLUDE_PATH(arch, file) "arch/" #arch "/" #file

#endif /* ARCH_H */
