/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/exec.h — execve 进程语义接口

   职责：Linux ABI 初始栈构建 + 用户页表初始化(含内核映射) +
         进程创建 + execve 阻塞等待语义
*/

#ifndef KERNEL_TASK_EXEC_H
#define KERNEL_TASK_EXEC_H

#include "types.h"
#include "exception.h" /* trap_frame_t */

/*
   task_execve - 把 ELF 映像原地装进当前任务并执行(Linux 语义)
   @pathname:    程序路径名(用于进程命名和 argv[0] 默认值)
   @file_data:   主程序 ELF 文件内容(内核虚拟地址), 本函数负责释放
   @file_size:   主程序文件大小
   @interp_data: 动态链接器 ELF 数据(静态链接时传 NULL), 本函数负责释放
   @interp_size: 动态链接器文件大小(静态时传 0)
   @argv:        参数数组(可为 NULL)
   @envp:        环境变量数组(可为 NULL)
   @frame:       用户进程从 syscall 进来时的 trap frame;
                 内核线程(开机 busybox_loader)传 NULL

   成功时当前任务的地址空间/入口被替换, PID 不变：
     - frame != NULL：返回 0, syscall 返回路径进入新程序
     - frame == NULL：通过 arch_enter_user() 直接进入用户态, 不返回
   失败时返回负错误码, 旧映像与原任务状态保持不变。
*/
int task_execve(const char *pathname, uint8_t *file_data, uint64_t file_size, uint8_t *interp_data,
                uint64_t interp_size, char **argv, char **envp, trap_frame_t *frame);

#endif /* KERNEL_TASK_EXEC_H */
