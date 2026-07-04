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
#include "syscall/net/ksocket.h"
#include "syscall/fs/pipe.h"
#include "syscall/fs/pty.h"
#include "klog.h"
#include "task/task.h"

/* 池数组本体：mm/mmap.c 等其它模块通过 extern 在 fd_pool.h 中可见。 */
fd_obj_t g_fd_pool[FD_POOL_SIZE];

int fd_pool_alloc(void)
{
    for (int i = 0; i < FD_POOL_SIZE; i++) {
        if (g_fd_pool[i].type == FDT_FREE) {
            g_fd_pool[i].type = FDT_ALLOCATED;
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
        g_fd_pool[idx].wq.waiter_count = 0;
        g_fd_pool[idx].type = FDT_FREE;
    }
}

int task_alloc_fd(task_t *task, int pool_idx)
{
    /* fd 0,1,2 reserved for stdin/stdout/stderr */
    for (int fd = 3; fd < (int)TASK_MAX_FD; fd++) {
        if (task->fd_table[fd] == -1) {
            task->fd_table[fd] = (int16_t)pool_idx;
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

int task_alloc_ion_fd(task_t *task, uint32_t handle)
{
    if (!task)
        return -1;

    int pool = fd_pool_alloc();
    if (pool < 0)
        return -1;

    fd_obj_t *obj = &g_fd_pool[pool];
    obj->type       = FDT_ION;
    obj->flags      = 0;
    obj->ion.handle = handle;

    const char *path = "/dev/ion_buffer";
    int k = 0;
    while (path[k] && k < (int)sizeof(obj->path) - 1) {
        obj->path[k] = path[k];
        k++;
    }
    obj->path[k] = '\0';

    int fd = task_alloc_fd(task, pool);
    if (fd < 0) {
        fd_pool_free(pool);
        return -1;
    }
    return fd;
}

int task_get_ion_handle(task_t *task, int fd, uint32_t *handle)
{
    fd_obj_t *obj = task_get_fd(task, fd);
    if (!obj || obj->type != FDT_ION || !handle)
        return -1;
    *handle = obj->ion.handle;
    return 0;
}

void fd_table_inherit(task_t *child, task_t *parent)
{
    for (uint32_t fd = 0; fd < TASK_MAX_FD; fd++) {
        /* execve: FD_CLOEXEC 标记的 fd 不继承 */
        if ((parent->fd_cloexec[fd / 8] >> (fd % 8)) & 1) {
            child->fd_table[fd] = -1;
            continue;
        }
        int pidx = (int)parent->fd_table[fd];
        if (pidx < 0 || pidx >= FD_POOL_SIZE) {
            child->fd_table[fd] = -1;
            continue;
        }
        fd_obj_t *src = &g_fd_pool[pidx];
        if (src->type == FDT_FREE || src->type == FDT_EPOLL) {
            child->fd_table[fd] = -1;
            continue;
        }
        int new_idx = fd_pool_alloc();
        if (new_idx < 0) {
            child->fd_table[fd] = -1;
            continue;
        }
        g_fd_pool[new_idx] = *src;
        g_fd_pool[new_idx].wq.waiter_count = 0;
        if (src->type == FDT_SOCKET)
            ksock_ref(src->sock.sock_idx);
        else if (src->type == FDT_PIPE) {
            if (src->pipe.is_write_end)
                pipe_ref_write(new_idx);
            else
                pipe_ref_read(new_idx);
        }
        else if (src->type == FDT_PTY) {
            if (src->pty.is_master)
                pty_ref_master(src->pty.pty_idx);
            else
                pty_ref_slave(src->pty.pty_idx);
        }
        child->fd_table[fd] = (int16_t)new_idx;
    }
}
