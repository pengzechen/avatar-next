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
#include "klog.h"
#include "uart/uart.h"
#include "vfs.h"

#define IOV_MAX_AVATAR 1024
#define IOV_COPY_MAX   16
#define USER_PTR_LIMIT 0x80000000ULL

/* write_handler 往内核搬用户数据的块大小。取小值是为了直接在栈上开，
 * 不为此引入一次 kmalloc/free（write 是最高频的 syscall 之一）。
 * 内核栈 16 KB，2 KB 的局部数组在 syscall 调用链深度下是安全的。 */
#define WRITE_USER_CHUNK 2048

/* 本地这份曾经只查范围，现在统一走 syscall/fs/path.c 的 user_range_accessible：
 * 范围 + **每页映射**。理由见那里的注释（writev02 用"合法但已 munmap"的地址
 * 让 copy_*_bytes 里的裸拷贝踩空 → 内核态 #PF → 停机）。 */

static bool trace_heavy_task(task_t *current)
{
    return current && current->is_user_process &&
           current->heap_end >= 0x3000000ULL;
}

static int copy_iov_from_user(struct kernel_iovec *dst,
                              const struct kernel_iovec *uiov, int iovcnt)
{
    if (!uiov || iovcnt < 0 || iovcnt > IOV_MAX_AVATAR || iovcnt > IOV_COPY_MAX)
        return -EINVAL;
    if (iovcnt == 0)
        return 0;
    if (!user_range_accessible(uiov, (uint64_t)iovcnt * sizeof(*uiov)))
        return -EFAULT;

    for (int i = 0; i < iovcnt; i++) {
        dst[i] = uiov[i];
        if (dst[i].iov_len > (1ULL << 31))
            return -EINVAL;
        if (dst[i].iov_len != 0 &&
            !user_range_accessible((const void *)(uintptr_t)dst[i].iov_base,
                                   dst[i].iov_len))
            return -EFAULT;
    }
    return 0;
}

void read_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    char *buf = (char *)regs[1];
    uint64_t count = regs[2];
    if (!buf || count == 0) {
        regs[0] = 0;
        return;
    }

    fd_obj_t *obj = task_get_fd(current, fd);
    if (obj && fd_obj_file(obj)) {
        /*
         * read 侧和 write 侧是**同一个洞**：vfs_read 会直接往用户缓冲里写
         * （ext4_fread → memcpy），未映射的地址就是内核态 #PF → 停机。
         * write 侧现在把数据先搬进内核缓冲；read 侧方向相反，代价最小的做法
         * 是先验"这一段每一页都映射着"。校验通过后当前页表就是该进程的
         * 用户页表（syscall 期间 CR3 未切走），直接写是安全的。
         */
        if (!user_range_accessible(buf, count)) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        if (trace_heavy_task(current)) {
            KLOG_SYSCALL(
                "[vfsio] pid=%u read fd=%d path=%s buf=0x%llx count=0x%llx off=0x%llx\n",
                current->id, fd, fd_obj_file(obj)->path, (uint64_t)buf, count,
                fd_obj_file(obj)->offset);
        }
        int rc = vfs_read(fd_obj_file(obj), buf, (size_t)count);
        if (trace_heavy_task(current)) {
            KLOG_SYSCALL("[vfsio] pid=%u read fd=%d rc=%d new_off=0x%llx\n",
                         current->id, fd, rc, fd_obj_file(obj)->offset);
        }
        regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
        return;
    }

    if (fd == 0 || (obj && !fd_obj_file(obj))) {
        /* stdin 未重定向：通过 UART 环形缓冲区读取 */
        int raw = !(g_termios.c_lflag & 0x0002u);  /* !ICANON */
        int do_cr = (g_termios.c_iflag & 0x0100u); /* ICRNL */
        uint64_t n = 0;
        int eintr = 0;
        while (n < count) {
            signal_check_uart();
            if (current->pending_sigs & ~current->blocked_sigs) {
                eintr = 1;
                break;
            }
            char c;
            if (uart_ringbuf_pop(&c)) {
                if (do_cr && c == '\r')
                    c = '\n';
                buf[n++] = c;
                if (raw)
                    break;
                if (c == '\n')
                    break;
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
    if (!vfs_file_is_regular(fd_obj_file(obj))) {
        regs[0] = (uint64_t)(int64_t)-ESPIPE;
        return;
    }

    if (trace_heavy_task(current)) {
        KLOG_SYSCALL(
            "[vfsio] pid=%u pread fd=%d path=%s buf=0x%llx count=0x%llx off=0x%llx\n",
            current->id, fd, fd_obj_file(obj)->path, (uint64_t)buf, count,
            (uint64_t)offset);
    }
    int rc = vfs_pread(fd_obj_file(obj), buf, (size_t)count, offset);
    if (trace_heavy_task(current)) {
        KLOG_SYSCALL("[vfsio] pid=%u pread fd=%d rc=%d\n", current->id, fd, rc);
    }
    regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
}

void write_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    const char *buf = (const char *)regs[1];
    uint64_t count = regs[2];
    if (!buf) {
        regs[0] = 0;
        return;
    }

    fd_obj_t *wobj = task_get_fd(current, fd);
    if (wobj && fd_obj_file(wobj)) {
        /*
         * ⚠️ 用户缓冲必须先落进内核，再把**内核地址**交给 VFS/ext4/块设备。
         *
         * 这里以前是把 buf（用户指针）裸传给 vfs_write，于是一路穿到
         * ramblk_bwrite 的 `memcpy(dst, buf, n)` —— 用户给个不在页表里的
         * 地址就是**内核态 #PF → platform_panic → 整机停机**。
         *
         * LTP writev02 正是冲着这个来的，它的 iovec 第一项就是
         * `{(caddr_t)-1, 8192}`：期望内核返回 EFAULT，而不是崩。
         * 实测崩溃现场：CR2=0x50002000、U=0、
         *   #0 ramblk_bwrite #2 ext4_fwrite #5 write_handler #6 writev_handler
         *
         * 用分块而不是一次 kmalloc 整块：count 是用户给的，可以大到离谱，
         * 先分配再校验等于让用户决定内核分配多少；分块则天然有界，第一块
         * 拷不动就直接 EFAULT（copy_from_user_bytes 内部走 user_range_ok）。
         * 与 Linux 的"部分写"语义也一致：已经写成功的字节数照常返回。
         */
        char kbuf[WRITE_USER_CHUNK];
        uint64_t done = 0;

        while (done < count) {
            size_t n = (size_t)(count - done);
            if (n > sizeof(kbuf))
                n = sizeof(kbuf);

            if (copy_from_user_bytes(buf + done, kbuf, n) < 0) {
                /* 一个字节都没写成 → EFAULT；已写成的部分按部分写返回 */
                regs[0] = done ? done : (uint64_t)(int64_t)-EFAULT;
                return;
            }

            int rc = vfs_write(fd_obj_file(wobj), kbuf, n);
            if (rc < 0) {
                regs[0] = done ? done : (uint64_t)(int64_t)rc;
                return;
            }
            done += (uint64_t)rc;
            if ((size_t)rc < n)
                break; /* 后端短写，别硬凑 */
        }

        regs[0] = done;
        return;
    }

    if (fd == 1 || fd == 2 || (wobj && !fd_obj_file(wobj))) {
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
    if (!vfs_file_is_regular(fd_obj_file(obj))) {
        regs[0] = (uint64_t)(int64_t)-ESPIPE;
        return;
    }

    int rc = vfs_pwrite(fd_obj_file(obj), buf, (size_t)count, offset);
    regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
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

        uint64_t rregs[6] = { 0 };
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

    if (rc < 0) {
        regs[0] = (uint64_t)(int64_t)rc;
        return;
    }
    if (iovcnt == 0) {
        regs[0] = 0;
        return;
    }

    fd_obj_t *wv_obj = task_get_fd(current, fd);
    if (!wv_obj && fd != 0 && fd != 1 && fd != 2) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    uint64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0)
            continue;
        uint64_t wregs[6] = {
            (uint64_t)fd, iov[i].iov_base, iov[i].iov_len, 0, 0, 0
        };
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
    int fd = (int)regs[0];
    int64_t offset = (int64_t)regs[1];
    int whence = (int)regs[2];
    fd_obj_t *obj = task_get_fd(current, fd);
    if (!obj) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    if (fd_obj_file(obj)) {
        uint64_t new_off = 0;
        int rc = vfs_seek(fd_obj_file(obj), offset, whence, &new_off);
        regs[0] = rc == 0 ? new_off : (uint64_t)(int64_t)rc;
    } else {
        regs[0] = (uint64_t)(int64_t)-EBADF;
    }
}

void sendfile_handler(uint64_t regs[6], task_t *current)
{
    int out_fd = (int)regs[0];
    int in_fd = (int)regs[1];
    uint64_t *poff = (uint64_t *)regs[2];
    size_t count = (size_t)regs[3];

    fd_obj_t *in_obj = (in_fd >= 3) ? task_get_fd(current, in_fd) : NULL;
    fd_obj_t *out_obj = (out_fd >= 3) ? task_get_fd(current, out_fd) : NULL;
    if (in_fd >= 3 && !in_obj) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }

    uint64_t saved_off = 0;
    if (poff && in_obj && fd_obj_file(in_obj)) {
        saved_off = fd_obj_file(in_obj)->offset;
        fd_obj_file(in_obj)->offset = *poff;
    }

    char sbuf[1024];
    size_t total = 0;
    while (total < count) {
        size_t want = count - total;
        if (want > sizeof(sbuf))
            want = sizeof(sbuf);

        int nr = 0;
        if (in_obj && fd_obj_file(in_obj)) {
            nr = vfs_read(fd_obj_file(in_obj), sbuf, want);
        }
        if (nr <= 0)
            break;

        if (out_fd == 0 || out_fd == 1 || out_fd == 2) {
            /*
             * sbuf 是**内核**缓冲（上面 vfs_read 填的），整段交给
             * klog_write —— 它是往物理 UART 写的唯一入口，整段持锁，
             * 所以这一块不会与别的核的日志逐字符插花。
             */
            klog_write(sbuf, (size_t)nr);
        } else if (out_obj && fd_obj_file(out_obj)) {
            vfs_write(fd_obj_file(out_obj), sbuf, (size_t)nr);
        }
        total += (size_t)nr;
    }

    if (poff && in_obj && fd_obj_file(in_obj)) {
        *poff = fd_obj_file(in_obj)->offset;
        fd_obj_file(in_obj)->offset = saved_off;
    }

    regs[0] = (uint64_t)total;
}
