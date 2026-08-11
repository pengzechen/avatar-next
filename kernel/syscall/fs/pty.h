#ifndef KERNEL_SYSCALL_FS_PTY_H
#define KERNEL_SYSCALL_FS_PTY_H

#include "types.h"

struct task;

#define PTY_MAX       8
#define PTY_BUF_SIZE  4096

typedef struct {
    uint8_t buf[PTY_BUF_SIZE];
    uint32_t rd;
    uint32_t wr;
    uint32_t count;
} pty_ring_t;

typedef struct {
    bool     in_use;
    bool     master_open;
    bool     slave_open;
    bool     locked;
    int      slave_refcnt;
    int      master_refcnt;

    uint32_t fg_pgid;

    pty_ring_t  m2s;     /* master write → slave read */
    pty_ring_t  s2m;     /* slave write → master read */

    struct task *blocked_master;
    struct task *blocked_slave;

    /* termios minimal subset */
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[19];

    /* winsize */
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} pty_pair_t;

int  pty_alloc_master(struct task *task);
int  pty_open_slave(struct task *task, int pty_idx);
int  pty_master_read(int pty_idx, void *buf, size_t count, bool nonblock);
int  pty_master_write(int pty_idx, const void *buf, size_t count, bool nonblock);
int  pty_slave_read(int pty_idx, void *buf, size_t count, bool nonblock);
int  pty_slave_write(int pty_idx, const void *buf, size_t count, bool nonblock);
void pty_close_master(int pty_idx);
void pty_close_slave(int pty_idx);
void pty_ref_slave(int pty_idx);
void pty_ref_master(int pty_idx);
int  pty_ioctl(int pty_idx, bool is_master, uint32_t req, void *argp);
bool pty_poll_readable_master(int pty_idx);
bool pty_poll_readable_slave(int pty_idx);
bool pty_poll_writable(int pty_idx);
int  pty_match_pts_path(const char *path);

#endif /* KERNEL_SYSCALL_FS_PTY_H */
