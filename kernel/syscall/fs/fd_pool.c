/*
 * fd_pool.c - 全局文件描述符对象池实现
 *
 * 从 kernel/syscall/syscall.c 抽出。
 * fd_table[i] in task_t 持有进入本池的索引（-1 = 未打开）。
 * fd 0/1/2（stdin/stdout/stderr）特殊处理（UART），不占用本池槽位。
 *
 * 编译需要 lwext4 头文件搜索路径（LWEXT4_CFLAGS）— fd_obj_t 内嵌
 * ext4_file / ext4_dir。
 */
#include "syscall/fs/fd_pool.h"
#include "klog.h"
#include "task/task.h"

/* 池数组本体：mm/mmap.c 等其它模块通过 extern 在 fd_pool.h 中可见。 */
fd_obj_t g_fd_pool[FD_POOL_SIZE];

int fd_pool_alloc(void)
{
    for (int i = 0; i < FD_POOL_SIZE; i++) {
        if (g_fd_pool[i].type == FDT_FREE) {
            KLOG_DEBUG("[fd] pool_alloc: allocated slot %d\n", i);
            return i;
        }
    }
    KLOG_ERROR("[fd] pool_alloc: no free slots (FD_POOL_SIZE=%d)\n", FD_POOL_SIZE);
    return -1;
}

void fd_pool_free(int idx)
{
    if (idx >= 0 && idx < FD_POOL_SIZE) {
        KLOG_DEBUG("[fd] pool_free: freeing slot %d\n", idx);
        g_fd_pool[idx].type = FDT_FREE;
    }
}

int task_alloc_fd(task_t *task, int pool_idx)
{
    /* fd 0,1,2 reserved for stdin/stdout/stderr */
    for (int fd = 3; fd < (int)TASK_MAX_FD; fd++) {
        /* 使用 (int8_t)-1 避免类型提升问题 */
        if (task->fd_table[fd] == (int8_t)-1) {
            task->fd_table[fd] = (int8_t)pool_idx;
            KLOG_DEBUG("[fd] task_alloc_fd: pid=%u allocated fd=%d for pool_idx=%d\n",
                      task->id, fd, pool_idx);
            return fd;
        }
    }

    /* 打印 fd_table 的前几个槽位用于调试 */
    KLOG_ERROR("[fd] task_alloc_fd: pid=%u no free fd (TASK_MAX_FD=%d)\n",
              task->id, TASK_MAX_FD);
    KLOG_ERROR("[fd] fd_table dump: [0]=%d [1]=%d [2]=%d [3]=%d [4]=%d [5]=%d\n",
              task->fd_table[0], task->fd_table[1], task->fd_table[2],
              task->fd_table[3], task->fd_table[4], task->fd_table[5]);

    return -1;
}

fd_obj_t *task_get_fd(task_t *task, int fd)
{
    if (fd < 0 || fd >= (int)TASK_MAX_FD)
        return NULL;
    int idx = task->fd_table[fd];
    if (idx < 0 || idx >= FD_POOL_SIZE)
        return NULL;
    if (g_fd_pool[idx].type == FDT_FREE)
        return NULL;
    return &g_fd_pool[idx];
}
