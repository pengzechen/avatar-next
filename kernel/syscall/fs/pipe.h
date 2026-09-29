#ifndef KERNEL_SYSCALL_FS_PIPE_H
#define KERNEL_SYSCALL_FS_PIPE_H

#include "types.h"
#include "list.h"

struct task;

#define PIPE_BUF_SIZE 4096
#define PIPE_MAX      16

typedef struct {
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t rd;
    uint32_t wr;
    uint32_t count;
    int rd_refcount;
    int wr_refcount;
    int rd_pool_idx;
    int wr_pool_idx;
    struct task *blocked_reader;
    struct task *blocked_writer;
} pipe_t;

int pipe_alloc(struct task *task, int fds_out[2]);
/*
 * nonblock 来自调用方持有的 vfs_file_t->flags & O_NONBLOCK（F_SETFL 设的）。
 * 早先这两个接口没有这个参数，管道**无条件阻塞** —— 于是 fcntl(F_SETFL,
 * O_NONBLOCK) 虽然把标志存进了 file->flags，读的时候却没人看，等着 EAGAIN
 * 的用户程序就永久卡死（LTP kill02 实测：它在四个管道上设了 O_NDELAY，
 * 然后去读"不该收到信号"的那两个，期望拿到 EAGAIN，结果挂在那里）。
 */
int pipe_read_endpoint(int pipe_idx, void *buf, size_t count, bool nonblock);
int pipe_write_endpoint(int pipe_idx, const void *buf, size_t count,
                        bool nonblock);
void pipe_ref_endpoint(int pipe_idx, bool is_write_end);
void pipe_close_endpoint(int pipe_idx, bool is_write_end);
bool pipe_poll_readable_endpoint(int pipe_idx);
bool pipe_poll_writable_endpoint(int pipe_idx);

#endif /* KERNEL_SYSCALL_FS_PIPE_H */
