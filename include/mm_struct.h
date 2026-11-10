/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef MM_STRUCT_H
#define MM_STRUCT_H

/*
   进程地址空间（mm_struct）

   从 task_t 里拆出 pgd/heap/mmap/interp/exe_path，原因见
   docs/task_t_refactor.md：线程组必须共享同一份地址空间，fork 则
   复制一份新的；用引用计数把“谁拥有 pgd、最后一个用户何时销毁”
   从 is_thread/shares_pgd 两个 bool 收敛到对象生命周期上。

   注意：user_entry/user_sp/user_stack_* 是每线程的（线程有自己的
   用户栈），仍留在 task_t。
*/

#include "types.h"

#define MM_EXE_PATH_LEN 128u /* 与 task.h 的 TASK_EXE_LEN 一致 */

typedef struct mm_struct {
    volatile int refcount;
    bool owns_pgd; /* true=pgd 是本地址空间的用户页表；false=内核任务占位 */
    uint64_t *pgd;
    uint64_t heap_end;
    uint64_t mmap_next;
    uint64_t mmap_base;
    uint64_t interp_end;
    char exe_path[MM_EXE_PATH_LEN];
} mm_struct_t;

/* 分配一个空 mm（pgd=NULL, owns_pgd=false），引用计数 = 1。 */
mm_struct_t *mm_alloc(void);

/* CLONE_VM 线程共享：引用计数 +1，返回同一个 mm。 */
mm_struct_t *mm_share(mm_struct_t *mm);

/* 归还引用；最后一个引用且 owns_pgd 时销毁用户页表。 */
void mm_put(mm_struct_t *mm);

#endif /* MM_STRUCT_H */
