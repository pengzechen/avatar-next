/*
 * fs/pipe.c - kernel pipe implementation
 *
 * Ring buffer pipe with blocking read/write via task_block/task_unblock.
 * Each pipe_alloc() creates one pipe_t and two fd_obj_t entries
 * (read end + write end).
 */
#include "syscall/fs/pipe.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/io/epoll.h"
#include "task/task.h"
#include "klog.h"
#include "string.h"

static pipe_t g_pipes[PIPE_MAX];

static int pipe_slot_alloc(void)
{
    for (int i = 0; i < PIPE_MAX; i++) {
        if (g_pipes[i].rd_refcount == 0 && g_pipes[i].wr_refcount == 0)
            return i;
    }
    return -1;
}

static pipe_t *pipe_get_by_idx(int pi)
{
    if (pi < 0 || pi >= PIPE_MAX)
        return NULL;
    if (g_pipes[pi].rd_refcount == 0 && g_pipes[pi].wr_refcount == 0)
        return NULL;
    return &g_pipes[pi];
}

int pipe_alloc(task_t *task, int fds_out[2])
{
    int pi = pipe_slot_alloc();
    if (pi < 0)
        return -1;

    int rd_pool = fd_pool_alloc();
    if (rd_pool < 0)
        return -1;
    int wr_pool = fd_pool_alloc();
    if (wr_pool < 0) {
        fd_pool_free(rd_pool);
        return -1;
    }

    pipe_t *p = &g_pipes[pi];
    memset(p, 0, sizeof(*p));
    p->rd_refcount = 1;
    p->wr_refcount = 1;
    p->rd_pool_idx = rd_pool;
    p->wr_pool_idx = wr_pool;

    vfs_file_t *rd_vf = NULL;
    if (vfs_create_pipe_file(pi, false, 0, &rd_vf) != 0) {
        fd_pool_free(rd_pool);
        fd_pool_free(wr_pool);
        p->rd_refcount = 0;
        p->wr_refcount = 0;
        return -1;
    }
    fd_obj_attach_vfs(rd_pool, rd_vf);

    vfs_file_t *wr_vf = NULL;
    if (vfs_create_pipe_file(pi, true, 0, &wr_vf) != 0) {
        vfs_discard_unopened(rd_vf);
        fd_pool_free(rd_pool);
        fd_pool_free(wr_pool);
        p->rd_refcount = 0;
        p->wr_refcount = 0;
        return -1;
    }
    fd_obj_attach_vfs(wr_pool, wr_vf);

    int rd_fd = task_alloc_fd(task, rd_pool);
    if (rd_fd < 0) {
        vfs_discard_unopened(rd_vf);
        vfs_discard_unopened(wr_vf);
        fd_pool_free(rd_pool);
        fd_pool_free(wr_pool);
        p->rd_refcount = 0;
        p->wr_refcount = 0;
        return -1;
    }
    int wr_fd = task_alloc_fd(task, wr_pool);
    if (wr_fd < 0) {
        task->fd_table[rd_fd] = -1;
        vfs_discard_unopened(rd_vf);
        vfs_discard_unopened(wr_vf);
        fd_pool_free(rd_pool);
        fd_pool_free(wr_pool);
        p->rd_refcount = 0;
        p->wr_refcount = 0;
        return -1;
    }

    fds_out[0] = rd_fd;
    fds_out[1] = wr_fd;
    return 0;
}

/*
 * pipe_interrupts - 这个任务现在会被信号打断吗？
 *
 * 判据必须比 "pending_sigs & ~blocked_sigs" 更严：那个式子会把**默认动作
 * 就是忽略**的信号也算进来（SIGCHLD 最典型）。shell 读管道时子进程退出会
 * 挂起 SIGCHLD，用它判就会让 read 平白返 EINTR —— Linux 不会
 * （prepare_signal() 对忽略类信号不置 TIF_SIGPENDING）。这里照
 * deliver_pending_signals() 的动作表来判，保证"会被打断"和"内核真会投递"
 * 是同一套判据。
 *
 * 为什么管道需要它：管道阻塞是真正的 task_block()（不是忙等），不检查信号
 * 就会在 SIGALRM 到达后重新睡下去 —— alarm() 打断阻塞 read 的语义就不成立。
 * LTP 每个测例都靠这个看门狗从卡死里脱身。
 */
static bool pipe_interrupts(void)
{
    task_t *t = task_current();
    if (!t)
        return false;

    uint64_t unblocked = t->pending_sigs & ~t->blocked_sigs;
    while (unblocked) {
        /*
         * 取最低置位的位序号。**不要**用 __builtin_ctzll：
         * freestanding 构建下它会生成对 libgcc 的 __ctzdi2 调用，而
         * rv64gc 没有 ctz 指令、链接里也没有 libgcc（-nodefaultlibs）
         * → riscv64 `undefined reference to __ctzdi2`（x86_64/aarch64 能
         * 内联成 bsf/rbit，所以只有 riscv64 暴露）。
         * 与 vplic.c / hext_run.c 的写法保持一致。
         */
        uint64_t low = unblocked & (~unblocked + 1); /* 取最低置位 */
        int i = 0;
        while (low > 1) {
            low >>= 1;
            i++;
        }
        unblocked &= unblocked - 1; /* 清掉最低置位 */

        int sig = i + 1;

        uint64_t h = t->sig_actions[i].sa_handler;
        if (h == SIG_IGN)
            continue; /* 显式忽略 */
        if (h != SIG_DFL)
            return true; /* 装了 handler → 一定会打断 */

        /* SIG_DFL：默认动作是忽略的那几个不算，其余（多数是终止）算 */
        switch (sig) {
        case SIGCHLD:
        case SIGCONT:
        case SIGURG:
        case SIGWINCH:
        case SIGTTIN:
        case SIGTTOU:
        case SIGTSTP:
            continue;
        default:
            return true;
        }
    }
    return false;
}

int pipe_read_endpoint(int pipe_idx, void *buf, size_t count, bool nonblock)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p || p->rd_refcount <= 0)
        return -1;

    uint8_t *dst = (uint8_t *)buf;
    size_t total = 0;

    while (total == 0) {
        while (p->count > 0 && total < count) {
            dst[total++] = p->buf[p->rd % PIPE_BUF_SIZE];
            p->rd++;
            p->count--;
        }
        /* 已有数据就先返回数据 —— 信号和数据的优先级上，Linux 也是数据优先 */
        if (total > 0)
            break;
        if (p->wr_refcount <= 0)
            return 0;

        /* O_NONBLOCK：没数据立刻返回 EAGAIN，绝不睡。
         * 放在 EINTR 判断之前：非阻塞本来就不睡，没有"被信号打断"可言；
         * pending 信号仍会在本次 syscall 返回边界被投递。 */
        if (nonblock)
            return -11; /* -EAGAIN */

        /* 别带着 pending 的信号睡下去：睡下去只能等下个事件才醒，
         * 而信号本身不会唤醒它（除非 itimer 那条路显式 unblock）。 */
        if (pipe_interrupts())
            return -4; /* -EINTR */

        p->blocked_reader = task_current();
        task_block(NULL);
        p->blocked_reader = NULL;
    }

    if (p->blocked_writer) {
        task_t *w = p->blocked_writer;
        p->blocked_writer = NULL;
        task_unblock(w);
    }

    fd_notify_waiters(p->wr_pool_idx, EPOLLOUT);

    return (int)total;
}

int pipe_write_endpoint(int pipe_idx, const void *buf, size_t count,
                        bool nonblock)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p || p->wr_refcount <= 0)
        return -1;

    if (p->rd_refcount <= 0) {
        task_send_signal(task_current(), SIGPIPE);
        return -32; /* -EPIPE */
    }

    const uint8_t *src = (const uint8_t *)buf;
    size_t total = 0;

    while (total < count) {
        while (p->count < PIPE_BUF_SIZE && total < count) {
            p->buf[p->wr % PIPE_BUF_SIZE] = src[total++];
            p->wr++;
            p->count++;
        }

        if (p->blocked_reader) {
            task_t *r = p->blocked_reader;
            p->blocked_reader = NULL;
            task_unblock(r);
        }

        fd_notify_waiters(p->rd_pool_idx, EPOLLIN);

        if (total < count) {
            if (p->rd_refcount <= 0) {
                task_send_signal(task_current(), SIGPIPE);
                return total > 0 ? (int)total : -32;
            }

            /* O_NONBLOCK：缓冲区满时立刻返回。已经写进去一部分就先返回
             * 那部分 —— Linux 对管道也是"部分写"语义，不是丢弃。 */
            if (nonblock)
                return total > 0 ? (int)total : -11; /* -EAGAIN */

            p->blocked_writer = task_current();
            task_block(NULL);
            p->blocked_writer = NULL;
        }
    }

    return (int)total;
}

void pipe_ref_endpoint(int pipe_idx, bool is_write_end)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p)
        return;
    if (is_write_end)
        p->wr_refcount++;
    else
        p->rd_refcount++;
}

void pipe_close_endpoint(int pipe_idx, bool is_write_end)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p)
        return;
    if (is_write_end) {
        if (--p->wr_refcount <= 0) {
            p->wr_refcount = 0;
            if (p->blocked_reader) {
                task_t *r = p->blocked_reader;
                p->blocked_reader = NULL;
                task_unblock(r);
            }
            fd_notify_waiters(p->rd_pool_idx, EPOLLHUP);
        }
        return;
    }

    if (--p->rd_refcount <= 0) {
        p->rd_refcount = 0;
        if (p->blocked_writer) {
            task_t *w = p->blocked_writer;
            p->blocked_writer = NULL;
            task_unblock(w);
        }
        fd_notify_waiters(p->wr_pool_idx, EPOLLERR);
    }
}

bool pipe_poll_readable_endpoint(int pipe_idx)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p)
        return false;
    return p->count > 0 || p->wr_refcount <= 0;
}

bool pipe_poll_writable_endpoint(int pipe_idx)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p)
        return false;
    return p->count < PIPE_BUF_SIZE || p->rd_refcount <= 0;
}
