/*
 * fs/path.c - 路径规范化、用户/内核字符串拷贝、stat 填充
 *
 * 由 kernel/syscall/syscall.c 拆出。
 */
#include "syscall/fs/path.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/syscall_internal.h"
#include "kernel_stat.h"
#include "string.h"
#include <ext4.h>
#include <ext4_errno.h>

#define USER_PTR_LIMIT 0x80000000ULL

static bool user_range_ok(const void *ptr, uint64_t len)
{
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end;

    if (start == 0)
        return false;
    if (len == 0)
        return true;

    end = start + len - 1;
    if (end < start)
        return false;
    return end < USER_PTR_LIMIT;
}

void resolve_path(const char *cwd, const char *path, char *out, int outlen)
{
    char tmp[128];

    if (path == NULL || path[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < (int)sizeof(tmp) - 1) {
            tmp[i] = path[i];
            i++;
        }
        tmp[i] = '\0';
    } else {
        int i = 0;
        while (cwd[i] && i < (int)sizeof(tmp) - 1) {
            tmp[i] = cwd[i];
            i++;
        }
        if (i > 0 && tmp[i - 1] != '/' && i < (int)sizeof(tmp) - 1)
            tmp[i++] = '/';
        int j = 0;
        while (path[j] && i < (int)sizeof(tmp) - 1) {
            tmp[i++] = path[j++];
        }
        tmp[i] = '\0';
    }

    /* 规范化：处理 //、.、.. */
    int seg_start[64];
    int seg_len[64];
    int seg_count = 0;
    int i = 0;

    while (tmp[i] != '\0') {
        while (tmp[i] == '/')
            i++;
        if (tmp[i] == '\0')
            break;

        int start = i;
        while (tmp[i] != '\0' && tmp[i] != '/')
            i++;
        int len = i - start;

        if (len == 1 && tmp[start] == '.')
            continue;
        if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (seg_count > 0)
                seg_count--;
            continue;
        }

        if (seg_count < (int)(sizeof(seg_start) / sizeof(seg_start[0]))) {
            seg_start[seg_count] = start;
            seg_len[seg_count]   = len;
            seg_count++;
        }
    }

    if (outlen <= 0)
        return;

    int pos = 0;
    out[pos++] = '/';
    for (int s = 0; s < seg_count && pos < outlen - 1; s++) {
        for (int k = 0; k < seg_len[s] && pos < outlen - 1; k++) {
            out[pos++] = tmp[seg_start[s] + k];
        }
        if (s != seg_count - 1 && pos < outlen - 1)
            out[pos++] = '/';
    }
    out[pos] = '\0';
}

int resolve_path_at(task_t *task, int dirfd, const char *pathname,
                    char *abspath, int abspath_len)
{
    if (!pathname)
        return -ENOENT;

    if (pathname[0] == '/') {
        resolve_path(task->cwd, pathname, abspath, abspath_len);
        return 0;
    }

    if (dirfd == AT_FDCWD) {
        resolve_path(task->cwd, pathname, abspath, abspath_len);
        return 0;
    }

    fd_obj_t *base = task_get_fd(task, dirfd);
    if (!base || (base->type != FDT_DIR && base->type != FDT_PSEUDO))
        return -EBADF;

    resolve_path(base->path, pathname, abspath, abspath_len);
    return 0;
}

void follow_symlinks(char *out, size_t outsz)
{
    char cur[128];
    int n = 0;
    while (out[n] && n < 127) { cur[n] = out[n]; n++; }
    cur[n] = '\0';

    for (int depth = 0; depth < 8; depth++) {
        char target[128];
        size_t rcnt = 0;
        if (ext4_readlink(cur, target, sizeof(target) - 1, &rcnt) != EOK)
            break;
        target[rcnt] = '\0';

        if (target[0] == '/') {
            n = 0;
            while (target[n] && n < 127) { cur[n] = target[n]; n++; }
            cur[n] = '\0';
        } else {
            int slash = 0;
            for (int i = 0; cur[i]; i++)
                if (cur[i] == '/') slash = i;
            char parent[128];
            int k;
            for (k = 0; k <= slash && k < 126; k++)
                parent[k] = cur[k];
            parent[k] = '\0';
            resolve_path(parent, target, cur, sizeof(cur));
        }
    }

    n = 0;
    while (cur[n] && n < (int)outsz - 1) { out[n] = cur[n]; n++; }
    out[n] = '\0';
}

int copy_string_from_user(const char *ustr, char *kbuf, int maxlen)
{
    if (!user_range_ok(ustr, (uint64_t)maxlen)) return -1;
    int i = 0;
    while (i < maxlen - 1) {
        kbuf[i] = ustr[i];
        if (ustr[i] == '\0') return i;
        i++;
    }
    kbuf[i] = '\0';
    return i;
}

int copy_string_to_user(const char *kstr, char *ubuf, int maxlen)
{
    if (!user_range_ok(ubuf, (uint64_t)maxlen)) return -1;
    int i = 0;
    while (i < maxlen - 1 && kstr[i]) {
        ubuf[i] = kstr[i];
        i++;
    }
    ubuf[i] = '\0';
    return i;
}

int copy_from_user_bytes(const void *usrc, void *kdst, uint64_t len)
{
    if (!user_range_ok(usrc, len))
        return -1;
    memcpy(kdst, usrc, len);
    return 0;
}

int copy_to_user_bytes(const void *ksrc, void *udst, uint64_t len)
{
    if (!user_range_ok(udst, len))
        return -1;
    memcpy(udst, ksrc, len);
    return 0;
}

int fill_stat_from_ext4(struct kernel_stat *st, const char *path)
{
    memset(st, 0, sizeof(*st));
    st->st_dev     = 1;
    st->st_nlink   = 1;
    st->st_blksize = 4096;

    uint32_t mode = 0;
    int rc = ext4_mode_get(path, &mode);
    if (rc != EOK)
        return -rc;
    st->st_mode = mode;

    uint32_t ino = 0;
    struct ext4_inode raw;
    if (ext4_raw_inode_fill(path, &ino, &raw) == EOK)
        st->st_ino = ino;

    if ((mode & 0170000) == 0100000) {
        ext4_file f;
        rc = ext4_fopen2(&f, path, 0 /* O_RDONLY */);
        if (rc != EOK)
            return -rc;
        st->st_size   = (int64_t)ext4_fsize(&f);
        st->st_blocks = (st->st_size + 511) / 512;
        ext4_fclose(&f);
    }

    return 0;
}
