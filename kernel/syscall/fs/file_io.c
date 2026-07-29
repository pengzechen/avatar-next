/*
 * fs/file_io.c - read / write / writev / lseek / sendfile / fsync / fdatasync
 *
 * 由 kernel/syscall/syscall.c 拆出。
 */
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/tty.h"
#include "task/task.h"
#include "task/sched.h"
#include "pseudofs.h"
#include "syscall/fs/pipe.h"
#include "syscall/net/ksocket.h"
#include "syscall/fs/pty.h"
#include "uart/uart.h"
#include <ext4.h>
#include <ext4_errno.h>

#define IOV_MAX_AVATAR 1024
#define IOV_COPY_MAX   16
#define USER_PTR_LIMIT 0x80000000ULL

static bool user_range_ok_local(uint64_t ptr, uint64_t len)
{
    uint64_t end;

    if (ptr == 0)
        return false;
    if (len == 0)
        return true;
    end = ptr + len - 1;
    if (end < ptr)
        return false;
    return end < USER_PTR_LIMIT;
}

static int copy_iov_from_user(struct kernel_iovec *dst,
                              const struct kernel_iovec *uiov,
                              int iovcnt)
{
    if (!uiov || iovcnt < 0 || iovcnt > IOV_MAX_AVATAR || iovcnt > IOV_COPY_MAX)
        return -EINVAL;
    if (iovcnt == 0)
        return 0;
    if (!user_range_ok_local((uint64_t)uiov,
                             (uint64_t)iovcnt * sizeof(*uiov)))
        return -EFAULT;

    for (int i = 0; i < iovcnt; i++) {
        dst[i] = uiov[i];
        if (dst[i].iov_len > (1ULL << 31))
            return -EINVAL;
        if (dst[i].iov_len != 0 &&
            !user_range_ok_local(dst[i].iov_base, dst[i].iov_len))
            return -EFAULT;
    }
    return 0;
}

void read_handler(uint64_t regs[6], task_t *current)
{
    int      fd    = (int)regs[0];
    char    *buf   = (char *)regs[1];
    uint64_t count = regs[2];
    if (!buf || count == 0) { regs[0] = 0; return; }

    fd_obj_t *obj = task_get_fd(current, fd);
    if (obj) {
        if (obj->type == FDT_PIPE) {
            int pool_idx = current->fd_table[fd];
            int rc = pipe_read(pool_idx, buf, (size_t)count);
            regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
            return;
        } else if (obj->type == FDT_SOCKET) {
            int sf = (obj->flags & 04000) ? KSOCK_MSG_DONTWAIT : 0;
            int rc = ksock_recv(obj->sock.sock_idx, buf, (size_t)count, sf);
            regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
            return;
        } else if (obj->type == FDT_PTY) {
            int rc;
            if (obj->pty.is_master)
                rc = pty_master_read(obj->pty.pty_idx, buf, (size_t)count);
            else
                rc = pty_slave_read(obj->pty.pty_idx, buf, (size_t)count);
            regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
            return;
        } else if (obj->type == FDT_PSEUDO) {
            int rc = pseudo_read(obj->pseudo.node_id, &obj->pseudo.off,
                                 buf, (size_t)count);
            regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
        } else if (obj->type == FDT_FILE) {
            size_t rcnt = 0;
            int rc = ext4_fread(&obj->file, buf, (size_t)count, &rcnt);
            regs[0] = (rc == EOK) ? (uint64_t)rcnt : (uint64_t)(int64_t)-EIO;
        } else {
            regs[0] = (uint64_t)(int64_t)-EBADF;
        }
        return;
    }

    if (fd == 0) {
        /* stdin 未重定向：通过 UART 环形缓冲区读取 */
        int raw    = !(g_termios.c_lflag & 0x0002u); /* !ICANON */
        int do_cr  =  (g_termios.c_iflag & 0x0100u); /* ICRNL */
        uint64_t n = 0;
        int eintr  = 0;
        while (n < count) {
            signal_check_uart();
            if (current->pending_sigs & ~current->blocked_sigs) {
                eintr = 1;
                break;
            }
            char c;
            if (uart_ringbuf_pop(&c)) {
                if (do_cr && c == '\r') c = '\n';
                buf[n++] = c;
                if (raw) break;
                if (c == '\n') break;
            } else {
                task_yield();
            }
        }
        regs[0] = eintr ? (uint64_t)(int64_t)-EINTR : n;
    } else {
        regs[0] = (uint64_t)(int64_t)-EBADF;
    }
}

void pread64_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    char *buf = (char *)regs[1];
    uint64_t count = regs[2];
    int64_t offset = (int64_t)regs[3];

    if (!buf || count == 0) {
        regs[0] = 0;
        return;
    }
    if (offset < 0) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    fd_obj_t *obj = task_get_fd(current, fd);
    if (!obj) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    if (obj->type != FDT_FILE) {
        regs[0] = (uint64_t)(int64_t)-ESPIPE;
        return;
    }

    int64_t saved = ext4_ftell(&obj->file);
    if (saved < 0 || ext4_fseek(&obj->file, offset, SEEK_SET) != EOK) {
        regs[0] = (uint64_t)(int64_t)-EIO;
        return;
    }

    size_t rcnt = 0;
    int rc = ext4_fread(&obj->file, buf, (size_t)count, &rcnt);
    int seek_rc = ext4_fseek(&obj->file, saved, SEEK_SET);
    if (rc != EOK || seek_rc != EOK)
        regs[0] = (uint64_t)(int64_t)-EIO;
    else
        regs[0] = (uint64_t)rcnt;
}

void write_handler(uint64_t regs[6], task_t *current)
{
    int          fd    = (int)regs[0];
    const char  *buf   = (const char *)regs[1];
    uint64_t     count = regs[2];
    if (!buf) { regs[0] = 0; return; }

    fd_obj_t *wobj = task_get_fd(current, fd);
    if (wobj) {
        if (wobj->type == FDT_PIPE) {
            int pool_idx = current->fd_table[fd];
            int rc = pipe_write(pool_idx, buf, (size_t)count);
            regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
            return;
        } else if (wobj->type == FDT_SOCKET) {
            int sf = (wobj->flags & 04000) ? KSOCK_MSG_DONTWAIT : 0;
            int rc = ksock_send(wobj->sock.sock_idx, buf, (size_t)count, sf);
            regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
            return;
        } else if (wobj->type == FDT_PTY) {
            int rc;
            if (wobj->pty.is_master)
                rc = pty_master_write(wobj->pty.pty_idx, buf, (size_t)count);
            else
                rc = pty_slave_write(wobj->pty.pty_idx, buf, (size_t)count);
            regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
            return;
        } else if (wobj->type == FDT_PSEUDO) {
            int rc = pseudo_write(wobj->pseudo.node_id, buf, (size_t)count);
            regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
        } else if (wobj->type == FDT_FILE) {
            size_t wcnt = 0;
            int rc = ext4_fwrite(&wobj->file, buf, (size_t)count, &wcnt);
            regs[0] = (rc == EOK) ? (uint64_t)wcnt : (uint64_t)(int64_t)-EIO;
        } else {
            regs[0] = (uint64_t)(int64_t)-EBADF;
        }
        return;
    }

    if (fd == 1 || fd == 2) {
        regs[0] = sys_write(buf, count);
    } else {
        regs[0] = (uint64_t)(int64_t)-EBADF;
    }
}

void pwrite64_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    const char *buf = (const char *)regs[1];
    uint64_t count = regs[2];
    int64_t offset = (int64_t)regs[3];

    if (!buf || count == 0) {
        regs[0] = 0;
        return;
    }
    if (offset < 0) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    fd_obj_t *obj = task_get_fd(current, fd);
    if (!obj) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    if (obj->type != FDT_FILE) {
        regs[0] = (uint64_t)(int64_t)-ESPIPE;
        return;
    }

    int64_t saved = ext4_ftell(&obj->file);
    if (saved < 0 || ext4_fseek(&obj->file, offset, SEEK_SET) != EOK) {
        regs[0] = (uint64_t)(int64_t)-EIO;
        return;
    }

    size_t wcnt = 0;
    int rc = ext4_fwrite(&obj->file, buf, (size_t)count, &wcnt);
    int seek_rc = ext4_fseek(&obj->file, saved, SEEK_SET);
    if (rc != EOK || seek_rc != EOK)
        regs[0] = (uint64_t)(int64_t)-EIO;
    else
        regs[0] = (uint64_t)wcnt;
}

void readv_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_iovec *uiov = (struct kernel_iovec *)regs[1];
    struct kernel_iovec iov[IOV_COPY_MAX];
    int iovcnt = (int)regs[2];
    int rc = copy_iov_from_user(iov, uiov, iovcnt);

    if (rc < 0) {
        regs[0] = (uint64_t)(int64_t)rc;
        return;
    }
    if (iovcnt == 0) {
        regs[0] = 0;
        return;
    }
    if (iovcnt > IOV_MAX_AVATAR) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    uint64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint64_t base = iov[i].iov_base;
        uint64_t len = iov[i].iov_len;
        if (len == 0)
            continue;
        if (base == 0) {
            regs[0] = total ? total : (uint64_t)(int64_t)-EFAULT;
            return;
        }

        uint64_t rregs[6] = {0};
        rregs[0] = (uint64_t)fd;
        rregs[1] = base;
        rregs[2] = len;
        read_handler(rregs, current);

        int64_t ret = (int64_t)rregs[0];
        if (ret < 0) {
            regs[0] = total ? total : (uint64_t)ret;
            return;
        }
        total += (uint64_t)ret;
        if ((uint64_t)ret < len)
            break;
    }
    regs[0] = total;
}

void writev_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_iovec *uiov = (struct kernel_iovec *)regs[1];
    struct kernel_iovec iov[IOV_COPY_MAX];
    int iovcnt = (int)regs[2];
    int rc = copy_iov_from_user(iov, uiov, iovcnt);

    if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }
    if (iovcnt == 0) { regs[0] = 0; return; }

    fd_obj_t *wv_obj = task_get_fd(current, fd);
    if (!wv_obj && fd != 0 && fd != 1 && fd != 2) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    uint64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
        uint64_t wregs[6] = { (uint64_t)fd, iov[i].iov_base, iov[i].iov_len, 0, 0, 0 };
        write_handler(wregs, current);
        int64_t ret = (int64_t)wregs[0];
        if (ret < 0) {
            regs[0] = total ? total : (uint64_t)ret;
            return;
        }
        total += (uint64_t)ret;
        if ((uint64_t)ret < iov[i].iov_len)
            break;
    }
    regs[0] = total;
}

void lseek_handler(uint64_t regs[6], task_t *current)
{
    int     fd     = (int)regs[0];
    int64_t offset = (int64_t)regs[1];
    int     whence = (int)regs[2];
    fd_obj_t *obj = task_get_fd(current, fd);
    if (!obj) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }
    if (obj->type == FDT_PSEUDO) {
        if      (whence == 0) obj->pseudo.off = (uint64_t)offset;
        else if (whence == 1) obj->pseudo.off = (uint64_t)((int64_t)obj->pseudo.off + offset);
        else                  obj->pseudo.off = 0;
        regs[0] = obj->pseudo.off;
    } else if (obj->type == FDT_FILE) {
        if (whence == 2) {
            int64_t fsize = (int64_t)ext4_fsize(&obj->file);
            int64_t new_off = fsize + offset;
            if (new_off < 0) { regs[0] = (uint64_t)(int64_t)-EINVAL; return; }
            offset = new_off;
            whence = 0;
        }
        int rc = ext4_fseek(&obj->file, offset, (uint32_t)whence);
        if (rc == EOK) {
            regs[0] = (uint64_t)ext4_ftell(&obj->file);
        } else if (whence == 0 && offset >= 0) {
            obj->file.fpos = (uint64_t)offset;
            regs[0] = (uint64_t)offset;
        } else {
            regs[0] = (uint64_t)(int64_t)-EINVAL;
        }
    } else {
        regs[0] = (uint64_t)(int64_t)-EBADF;
    }
}

void sendfile_handler(uint64_t regs[6], task_t *current)
{
    int       out_fd = (int)regs[0];
    int       in_fd  = (int)regs[1];
    uint64_t *poff   = (uint64_t *)regs[2];
    size_t    count  = (size_t)regs[3];

    fd_obj_t *in_obj  = (in_fd  >= 3) ? task_get_fd(current, in_fd)  : NULL;
    fd_obj_t *out_obj = (out_fd >= 3) ? task_get_fd(current, out_fd) : NULL;
    if (in_fd >= 3 && !in_obj) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    uint64_t saved_off = 0;
    if (poff && in_obj && in_obj->type == FDT_PSEUDO) {
        saved_off = in_obj->pseudo.off;
        in_obj->pseudo.off = *poff;
    }

    char   sbuf[1024];
    size_t total = 0;
    while (total < count) {
        size_t want = count - total;
        if (want > sizeof(sbuf)) want = sizeof(sbuf);

        int nr = 0;
        if (in_obj && in_obj->type == FDT_PSEUDO) {
            nr = pseudo_read(in_obj->pseudo.node_id,
                             &in_obj->pseudo.off, sbuf, want);
        } else if (in_obj && in_obj->type == FDT_FILE) {
            size_t rcnt = 0;
            int rc = ext4_fread(&in_obj->file, sbuf, want, &rcnt);
            nr = (rc == EOK) ? (int)rcnt : -(int)EIO;
        }
        if (nr <= 0) break;

        if (out_fd == 0 || out_fd == 1 || out_fd == 2) {
            for (int i = 0; i < nr; i++) uart_putc(sbuf[i]);
        } else if (out_obj && out_obj->type == FDT_PSEUDO) {
            pseudo_write(out_obj->pseudo.node_id, sbuf, (size_t)nr);
        } else if (out_obj && out_obj->type == FDT_FILE) {
            size_t wcnt = 0;
            ext4_fwrite(&out_obj->file, sbuf, (size_t)nr, &wcnt);
        }
        total += (size_t)nr;
    }

    if (poff && in_obj && in_obj->type == FDT_PSEUDO) {
        *poff = in_obj->pseudo.off;
        in_obj->pseudo.off = saved_off;
    }

    regs[0] = (uint64_t)total;
}
