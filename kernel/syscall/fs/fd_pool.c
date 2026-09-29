/*
 * fd_pool.c - 全局文件描述符对象池实现
 *
 * 从 kernel/syscall/syscall.c 抽出。
 * fd_table[i] in task_t 持有进入本池的索引（-1 = 未打开）。
 * fd 0/1/2（stdin/stdout/stderr）特殊处理（UART），不占用本池槽位。
 *
 */
#include "syscall/fs/fd_pool.h"
#include "arch.h"
#include "spinlock.h"
#include "klog.h"
#include "task/task.h"

#if DRIVER_ION
#include "ion/ion.h"
#endif

/*
 * 池和 fd 表的并发保护。
 *
 * `g_fd_pool` 是**全局**的，而"扫一个 FDT_FREE 槽 -> 占住它"是读改写：
 * 两个核同时进来会双双看到同一个 FREE 槽、双双占住它，于是两个任务的 fd
 * 指向同一个 fd_obj —— 后开的那个把前一个的 vfs_file 覆盖掉，谁先 close
 * 谁就把还在用的槽 fd_pool_free 掉，另一个任务接着用的是已经归还的槽
 * （真有别的 open 复用，就是"用错文件"甚至 use-after-free）。
 *
 * 锁只圈**槽位状态和 fd 表表项的读改写**那几行，不圈 vfs_close / 日志：
 * 前者会走到 lwext4 的全局 fs 锁上（锁序要单向：fd 池锁 -> fs 锁），
 * 后者在持锁期间打串口是自找麻烦。
 */
static spinlock_t g_fd_pool_lock = SPINLOCK_INIT;

/* 池数组本体：mm/mmap.c 等其它模块通过 extern 在 fd_pool.h 中可见。 */
fd_obj_t g_fd_pool[FD_POOL_SIZE];

int fd_pool_alloc(void)
{
    uint64_t flags = arch_irq_save();
    spin_lock(&g_fd_pool_lock);

    int slot = -1;
    for (int i = 0; i < FD_POOL_SIZE; i++) {
        if (g_fd_pool[i].type == FDT_FREE) {
            g_fd_pool[i].type = FDT_ALLOCATED;
            g_fd_pool[i].vfs_file = NULL;
            slot = i;
            break;
        }
    }

    spin_unlock(&g_fd_pool_lock);
    arch_irq_restore(flags);

    if (slot < 0) {
        KLOG_ERROR("[fd] pool_alloc: no free slots (FD_POOL_SIZE=%d)\n",
                   FD_POOL_SIZE);
        return -1;
    }
    KLOG_SYSCALL("[fd] pool_alloc: allocated slot %d\n", slot);
    return slot;
}

void fd_pool_free(int idx)
{
    if (idx < 0 || idx >= FD_POOL_SIZE)
        return;

    uint64_t flags = arch_irq_save();
    spin_lock(&g_fd_pool_lock);
    g_fd_pool[idx].wq.waiter_count = 0;
    g_fd_pool[idx].vfs_file = NULL;
    g_fd_pool[idx].type = FDT_FREE;
    spin_unlock(&g_fd_pool_lock);
    arch_irq_restore(flags);

    KLOG_SYSCALL("[fd] pool_free: freeing slot %d\n", idx);
}

void fd_obj_ref(int idx)
{
    if (idx < 0 || idx >= FD_POOL_SIZE)
        return;

    fd_obj_t *obj = &g_fd_pool[idx];
    if (obj->type == FDT_FREE)
        return;

    if (obj->vfs_file)
        vfs_ref(obj->vfs_file);
}

void fd_obj_close(int idx)
{
    if (idx < 0 || idx >= FD_POOL_SIZE)
        return;

    fd_obj_t *obj = &g_fd_pool[idx];
    if (obj->type == FDT_FREE)
        return;

    if (obj->vfs_file) {
        vfs_close(obj->vfs_file);
        obj->vfs_file = NULL;
    } else if (obj->type == FDT_EPOLL) {
        epoll_destroy(obj->epoll.ep_idx);
    }
#if DRIVER_ION
    else if (obj->type == FDT_ION) {
        ion_free((ion_handle_t)obj->ion.handle);
    }
#endif
}

void fd_obj_attach_vfs(int idx, vfs_file_t *file)
{
    if (idx < 0 || idx >= FD_POOL_SIZE || !file)
        return;

    fd_obj_t *obj = &g_fd_pool[idx];
    obj->vfs_file = file;
    obj->flags = file->flags;
    obj->type = FDT_VFS;
    int k = 0;
    while (file->path[k] && k < (int)sizeof(obj->path) - 1) {
        obj->path[k] = file->path[k];
        k++;
    }
    obj->path[k] = '\0';
}

int task_alloc_fd(task_t *task, int pool_idx)
{
    /*
     * 同一进程的两个线程共享 fd_table，所以"找 -1 槽 -> 写上"同样要互斥，
     * 否则两个线程会拿到同一个 fd 号、后写的那个把先写的挤掉。
     */
    uint64_t flags = arch_irq_save();
    spin_lock(&g_fd_pool_lock);

    int fd = -1;
    /* fd 0,1,2 reserved for stdin/stdout/stderr */
    for (int i = 3; i < (int)TASK_MAX_FD; i++) {
        if (task->fd_table[i] == -1) {
            task->fd_table[i] = (int16_t)pool_idx;
            fd = i;
            break;
        }
    }

    spin_unlock(&g_fd_pool_lock);
    arch_irq_restore(flags);

    if (fd >= 0) {
        KLOG_SYSCALL(
            "[fd] task_alloc_fd: pid=%u allocated fd=%d for pool_idx=%d\n",
            task->id, fd, pool_idx);
        return fd;
    }

    /* 打印 fd_table 的前几个槽位用于调试 */
    KLOG_ERROR("[fd] task_alloc_fd: pid=%u no free fd (TASK_MAX_FD=%d)\n",
               task->id, TASK_MAX_FD);
    KLOG_ERROR(
        "[fd] fd_table dump: [0]=%d [1]=%d [2]=%d [3]=%d [4]=%d [5]=%d\n",
        task->fd_table[0], task->fd_table[1], task->fd_table[2],
        task->fd_table[3], task->fd_table[4], task->fd_table[5]);

    return -1;
}

/*
 * 原子地摘走 fd_table[fd]，返回它原来的池索引（-1 = 本来就没开）。
 *
 * close 路径必须用它而不是"先读 fd_table[fd] 再去做关闭"：同一进程的两个
 * 线程同时 close 同一个 fd 时，两边都会读到同一个 idx，于是同一个槽被
 * 关两次、归还两次 —— 第二次归还时槽可能已经被别的 open 拿走了，直接把
 * 别人正在用的槽标成 FREE。摘表项这一步是"谁抢到谁负责关"的分界点。
 */
int task_take_fd(task_t *task, int fd)
{
    if (!task || fd < 0 || fd >= (int)TASK_MAX_FD)
        return -1;

    uint64_t flags = arch_irq_save();
    spin_lock(&g_fd_pool_lock);
    int idx = task->fd_table[fd];
    task->fd_table[fd] = -1;
    spin_unlock(&g_fd_pool_lock);
    arch_irq_restore(flags);

    return idx;
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
    obj->type = FDT_ION;
    obj->flags = 0;
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
    /*
     * 只在内核任务/进程之间谈继承没意义：idle 之类的内核任务一张 fd 表都没有
     * （正常全是 -1），而且它们的 fd_table 一旦忘了初始化就是一片 0 ——
     * 而 0 是合法槽位号，会被当成「256 个 fd」逐个拷贝，一次就把整个池子
     * 吃光（实测过：idle/1 作父进程，128 个槽瞬间占满，此后宿主里所有
     * open() 都失败）。这里直接按「没有可继承的 fd」处理，表清成 -1。
     */
    if (!parent->is_user_process) {
        for (uint32_t fd = 0; fd < TASK_MAX_FD; fd++)
            child->fd_table[fd] = -1;
        return;
    }

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
        fd_obj_ref(new_idx);
        child->fd_table[fd] = (int16_t)new_idx;
    }
}
