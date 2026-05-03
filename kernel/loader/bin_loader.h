/*
 * kernel/syscall/bin_loader.h - 二进制程序加载器
 */

#ifndef KERNEL_SYSCALL_BIN_LOADER_H
#define KERNEL_SYSCALL_BIN_LOADER_H

#include "types.h"

/**
 * bin_loader_load_from_file - 从文件系统加载并执行程序
 * @pathname: 程序路径
 * @argv: 参数数组
 * @envp: 环境变量数组
 *
 * 从文件系统读取可执行文件，加载到内存并执行。
 * 当前实现：支持平面二进制格式（原始二进制代码）
 *
 * 返回：成功不返回，失败返回负错误码
 */
int bin_loader_load_from_file(const char *pathname, char **argv, char **envp);

#endif /* KERNEL_SYSCALL_BIN_LOADER_H */
