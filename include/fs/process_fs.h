/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef PROCESS_FS_H
#define PROCESS_FS_H

/*
   进程级文件系统状态：fd 表 + 当前工作目录

   这两块从 task_t 里拆出来，原因见 docs/task_t_refactor.md：
   fd 表（512 B）+ cwd（128 B）占了 TCB 相当大一块，而它们属于
   “进程的打开文件/文件系统上下文”，不是调度器需要关心的字段。
   当前 clone 仍按旧行为复制（fork/线程各一份）；引用计数为后续
   CLONE_FILES / CLONE_FS 共享留好模型。
*/

#include "types.h"

#define FILES_FD_MAX 256u /* 与 task.h 的 TASK_MAX_FD 一致 */
#define FS_CWD_MAX   128u /* 与 task.h 的 TASK_CWD_LEN 一致 */

typedef struct files_struct {
    volatile int refcount;
    int16_t fd_table[FILES_FD_MAX];       /* g_fd_pool 索引, -1 = 未打开 */
    uint8_t fd_cloexec[FILES_FD_MAX / 8]; /* FD_CLOEXEC 位图 */
} files_struct_t;

typedef struct fs_struct {
    volatile int refcount;
    char cwd[FS_CWD_MAX];
} fs_struct_t;

/* 分配一个空 fd 表（全 -1）/ 默认 cwd（"/"），引用计数 = 1。 */
files_struct_t *files_alloc(void);
fs_struct_t *fs_alloc(void);

/* 归还引用；最后一个引用释放池槽位。 */
void files_put(files_struct_t *files);
void fs_put(fs_struct_t *fs);

#endif /* PROCESS_FS_H */
