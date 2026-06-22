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
    struct task *blocked_reader;
    struct task *blocked_writer;
} pipe_t;

int  pipe_alloc(struct task *task, int fds_out[2]);
int  pipe_read(int pool_idx, void *buf, size_t count);
int  pipe_write(int pool_idx, const void *buf, size_t count);
void pipe_ref_read(int pool_idx);
void pipe_ref_write(int pool_idx);
void pipe_close_read(int pool_idx);
void pipe_close_write(int pool_idx);
bool pipe_poll_readable(int pool_idx);
bool pipe_poll_writable(int pool_idx);

#endif /* KERNEL_SYSCALL_FS_PIPE_H */
