#ifndef KERNEL_SYSCALL_FS_PIPE_H
#define KERNEL_SYSCALL_FS_PIPE_H

#include "types.h"
#include "list.h"

struct task;

#define PIPE_BUF_SIZE  4096
#define PIPE_MAX       16

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t rd;
    uint32_t wr;
    uint32_t count;
    int      rd_refcount;
    int      wr_refcount;
    int      rd_pool_idx;
    int      wr_pool_idx;
    struct task *blocked_reader;
    struct task *blocked_writer;
} pipe_t;

int  pipe_alloc(struct task *task, int fds_out[2]);
int  pipe_read_endpoint(int pipe_idx, void *buf, size_t count);
int  pipe_write_endpoint(int pipe_idx, const void *buf, size_t count);
void pipe_ref_endpoint(int pipe_idx, bool is_write_end);
void pipe_close_endpoint(int pipe_idx, bool is_write_end);
bool pipe_poll_readable_endpoint(int pipe_idx);
bool pipe_poll_writable_endpoint(int pipe_idx);

#endif /* KERNEL_SYSCALL_FS_PIPE_H */
