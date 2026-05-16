/*
 * kernel/task/exec.h — execve 进程语义接口
 *
 * 职责：Linux ABI 初始栈构建 + 用户页表初始化（含内核映射） +
 *       进程创建 + execve 阻塞等待语义
 */

#ifndef KERNEL_TASK_EXEC_H
#define KERNEL_TASK_EXEC_H

#include "types.h"

/**
 * task_execve - 将 ELF 二进制加载为新用户进程并执行
 * @pathname:    程序路径名（用于进程命名和 argv[0] 默认值）
 * @file_data:   主程序 ELF 文件内容（内核虚拟地址）
 * @file_size:   主程序文件大小
 * @interp_data: 动态链接器 ELF 数据（静态链接时传 NULL）
 * @interp_size: 动态链接器文件大小（静态时传 0）
 * @argv:        参数数组（可为 NULL）
 * @envp:        环境变量数组（可为 NULL）
 *
 * 创建新进程，阻塞当前任务直到新进程退出，然后以相同退出码退出。
 * 返回：失败时返回负值；成功时调用 task_exit() 不返回。
 */
int task_execve(const char *pathname,
                uint8_t *file_data, uint64_t file_size,
                uint8_t *interp_data, uint64_t interp_size,
                char **argv, char **envp);

#endif /* KERNEL_TASK_EXEC_H */
