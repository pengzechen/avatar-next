/*
 * kernel/syscall/elf_loader.h - ELF 程序加载器
 */

#ifndef KERNEL_SYSCALL_ELF_LOADER_H
#define KERNEL_SYSCALL_ELF_LOADER_H

#include "types.h"

/**
 * elf_loader_load_from_file - 从文件系统加载并执行 ELF 程序
 * @pathname: 程序路径
 * @argv: 参数数组
 * @envp: 环境变量数组
 *
 * 支持静态链接的 ELF PIE 可执行文件。
 *
 * 返回：成功不返回，失败返回负错误码
 */
int elf_loader_load_from_file(const char *pathname, char **argv, char **envp);

#endif /* KERNEL_SYSCALL_ELF_LOADER_H */
