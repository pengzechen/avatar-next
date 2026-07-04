/*
 * io/epoll.h — epoll 子系统数据结构与 API
 *
 * 提供 Linux 兼容的 epoll_create1/epoll_ctl/epoll_pwait 语义。
 * 核心设计：每个 fd_obj_t 持有一个 fd_waitqueue_t，fd 状态变化时
 * 通过 fd_notify_waiters() 主动唤醒所有监听该 fd 的 epoll 实例。
 */
#ifndef KERNEL_SYSCALL_IO_EPOLL_H
#define KERNEL_SYSCALL_IO_EPOLL_H

#include "types.h"
#include "arch.h"

struct task;
typedef struct task task_t;

/* ── epoll_event flags ──────────────────────────────────────────── */
#define EPOLLIN      0x001
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLRDHUP   0x2000
#define EPOLLET      (1U << 31)

/* ── epoll_ctl operations ───────────────────────────────────────── */
#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_DEL  2
#define EPOLL_CTL_MOD  3

/* ── epoll_create1 flags ────────────────────────────────────────── */
#define EPOLL_CLOEXEC  0x80000

/* ── 容量限制 ───────────────────────────────────────────────────── */
#define EPOLL_MAX_INSTANCES  16
#define EPOLL_MAX_ITEMS      64
#define FD_WAIT_MAX          4

/* ── fd_waitqueue：挂在每个 fd_obj_t 上的等待链 ─────────────────── */

struct epoll_instance;

typedef struct {
    struct epoll_instance *ep;
    int item_idx;
} fd_waiter_t;

typedef struct {
    fd_waiter_t waiters[FD_WAIT_MAX];
    int         waiter_count;
} fd_waitqueue_t;

/* ── epoll 监听项 ───────────────────────────────────────────────── */

typedef struct {
    int      fd;
    int      pool_idx;
    uint32_t events;
    uint64_t data;
    bool     ready;
} epoll_item_t;

/* ── epoll 实例 ─────────────────────────────────────────────────── */

typedef struct epoll_instance {
    bool          in_use;
    uint32_t      owner_pid;
    epoll_item_t  items[EPOLL_MAX_ITEMS];
    int           item_count;
    int           ready_count;
    task_t       *blocked_task;
} epoll_instance_t;

/* ── 用户空间 epoll_event 布局（与 Linux ABI 兼容）─────────────── */

struct epoll_event {
    uint32_t events;
#if ARCH_X86_64
    uint64_t data;
} __attribute__((packed));
#else
    uint64_t data;
};
#endif

/* ── API ────────────────────────────────────────────────────────── */

/* fd 状态变化时调用：唤醒所有监听该 pool_idx 的 epoll 实例 */
void fd_notify_waiters(int pool_idx, uint32_t events);

/* fd 关闭时调用：从所有 epoll 实例中注销该 pool_idx */
void fd_close_notify(int pool_idx);

/* 统一 poll 接口：返回 fd 当前就绪事件掩码 */
uint32_t fd_poll(task_t *task, int fd);

/* syscall handlers */
void epoll_create1_handler(uint64_t regs[6], task_t *current);
void epoll_ctl_handler    (uint64_t regs[6], task_t *current);
void epoll_pwait_handler  (uint64_t regs[6], task_t *current);

/* close(epfd) 时调用 */
void epoll_destroy(int ep_idx);

#endif /* KERNEL_SYSCALL_IO_EPOLL_H */
