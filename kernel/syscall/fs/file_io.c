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
#include "uart/uart.h"
#include <ext4.h>
#include <ext4_errno.h>

void read_handler(uint64_t regs[6], task_t *current)
{
    int      fd    = (int)regs[0];
    char    *buf   = (char *)regs[1];
    uint64_t count = regs[2];
    if (!buf || count == 0) { regs[0] = 0; return; }

    fd_obj_t *obj = task_get_fd(current, fd);
    if (obj) {
        if (obj->type == FDT_PSEUDO) {
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

void write_handler(uint64_t regs[6], task_t *current)
{
    int          fd    = (int)regs[0];
    const char  *buf   = (const char *)regs[1];
    uint64_t     count = regs[2];
    if (!buf) { regs[0] = 0; return; }

    fd_obj_t *wobj = task_get_fd(current, fd);
    if (wobj) {
        if (wobj->type == FDT_PSEUDO) {
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

void writev_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_iovec *iov = (struct kernel_iovec *)regs[1];
    int iovcnt = (int)regs[2];
    if (!iov || iovcnt <= 0) { regs[0] = 0; return; }

    fd_obj_t *wv_obj = task_get_fd(current, fd);
    if (!wv_obj && fd != 0 && fd != 1 && fd != 2) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    uint64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
        if (wv_obj) {
            if (wv_obj->type == FDT_PSEUDO) {
                pseudo_write(wv_obj->pseudo.node_id,
                             (const void *)iov[i].iov_base, iov[i].iov_len);
            } else if (wv_obj->type == FDT_FILE) {
                size_t wcnt = 0;
                ext4_fwrite(&wv_obj->file, (const void *)iov[i].iov_base,
                            iov[i].iov_len, &wcnt);
            }
        } else {
            sys_write((const char *)iov[i].iov_base, iov[i].iov_len);
        }
        total += iov[i].iov_len;
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
        int rc = ext4_fseek(&obj->file, offset, (uint32_t)whence);
        if (rc == EOK)
            regs[0] = (uint64_t)ext4_ftell(&obj->file);
        else
            regs[0] = (uint64_t)(int64_t)-EINVAL;
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
