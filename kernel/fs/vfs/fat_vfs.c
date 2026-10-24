/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/fs/vfs/fat_vfs.c - FatFs 的 VFS 胶水

   设计要点：
     - 绝对路径 /boot/foo 先转成 FatFs 的卷内路径 /foo（fat_relpath）。
     - include/vfs.h 不引入 ff.h；FIL/DIR 放在 kmalloc 出来的 fat_handle_t 里，
       vfs_file_t 只存一个 void *。
     - 所有 FatFs 调用都在 fs_global_lock() 内；和 lwext4 共用同一把锁。
     - file->offset 是权威的逻辑偏移。FatFs 的 f_lseek 在越过 EOF 时会扩展
       文件（且扩展区内容未定义），所以 seek 只改 offset；真正读写时再懒同步，
       越过 EOF 的写由 fat_zero_fill() 补零。
*/
#include "vfs_internal.h"
#include "kmalloc.h"
#include "string.h"
#include "fs_lock.h"

#include <ff.h>

/* ff.h 只导出 AM_HID/AM_SYS/AM_DIR/AM_ARC；卷标位是 ff.c 的内部宏 */
#ifndef AM_VOL
#define AM_VOL 0x08
#endif

/* FatFs 用 DWORD (32-bit) 的 FSIZE_t / f_lseek；FAT32 单文件上限 4GB-1 */
#define FAT_MAX_SIZE 0xFFFFFFFFu
#define FAT_NAME_MAX (FF_MAX_LFN + 1)

typedef struct {
    bool is_dir;
    bool pending_valid;
    uint8_t pending_attr;
    char pending_name[FAT_NAME_MAX];
    char rel[128]; /* 相对 FAT 卷根的路径，打开时固化 */
    union {
        FIL f;
        DIR d;
    } u;
} fat_handle_t;

/* ── 基础助手 ───────────────────────────────────────────────────── */

static int fat_errno(FRESULT fr)
{
    switch (fr) {
    case FR_OK:
        return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return -VFS_ENOENT;
    case FR_EXIST:
        return -VFS_EEXIST;
    case FR_DENIED:
        return -VFS_EACCES;
    case FR_WRITE_PROTECTED:
        return -VFS_EROFS;
    case FR_INVALID_NAME:
        return -VFS_EINVAL;
    case FR_NOT_ENOUGH_CORE:
        return -VFS_ENOMEM;
    case FR_TOO_MANY_OPEN_FILES:
        return -VFS_EMFILE;
    case FR_INVALID_OBJECT:
        return -VFS_EBADF;
    case FR_NO_FILESYSTEM:
    case FR_NOT_READY:
    case FR_INVALID_DRIVE:
        return -VFS_ENODEV;
    default:
        return -VFS_EIO;
    }
}

/* 写路径上 FatFs 用 FR_DENIED 表示“磁盘满”；普通路径上它更像 EACCES。 */
static int fat_errno_write(FRESULT fr)
{
    return fr == FR_DENIED ? -VFS_ENOSPC : fat_errno(fr);
}

/*
   /boot      -> /
   /boot/a    -> /a
   /bootx     -> EINVAL（VFS 的前缀匹配保证不会发生，这里兜底）
*/
static int fat_relpath(const char *mp, const char *path, char *out, size_t outsz)
{
    if (!mp || !path || !out || outsz < 2)
        return -VFS_EINVAL;

    size_t mplen = strlen(mp);
    if (mplen == 1 && mp[0] == '/') {
        /* 不允许把 FAT 挂在 /，但真发生了也按整路径处理 */
    } else {
        if (strncmp(path, mp, mplen) != 0)
            return -VFS_EINVAL;
        path += mplen;
        if (*path == '\0')
            path = "/";
        else if (*path != '/')
            return -VFS_EINVAL;
    }

    size_t len = strlen(path);
    if (len + 1 > outsz)
        return -VFS_EINVAL;
    memcpy(out, path, len + 1);
    return 0;
}

/*
   stat(rel) 用 fat_ino_pair(rel, "")；getdents 用 fat_ino_pair(h->rel, name)。
   两者必须得到同一个值，所以根目录 "/" 后面不再补一个 '/'。
*/
static uint64_t fat_ino_pair(const char *dir, const char *name)
{
    bool dir_is_root = (dir[0] == '/' && dir[1] == '\0');
    uint64_t h = 1469598103934665603ULL;

    while (*dir) {
        h ^= (uint8_t)*dir++;
        h *= 1099511628211ULL;
    }
    if (!dir_is_root && name[0]) {
        h ^= (uint8_t)'/';
        h *= 1099511628211ULL;
    }
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

static bool fat_is_leap(int y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

static int64_t fat_dos_to_unix(WORD fdate, WORD ftime)
{
    if (fdate == 0)
        return 0;

    int year = 1980 + ((fdate >> 9) & 0x7F);
    int mon = (fdate >> 5) & 0x0F;
    int day = fdate & 0x1F;
    int hour = (ftime >> 11) & 0x1F;
    int min = (ftime >> 5) & 0x3F;
    int sec = (ftime & 0x1F) * 2;

    if (mon < 1 || mon > 12 || day < 1 || day > 31)
        return 0;

    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t days = 0;
    for (int y = 1970; y < year; y++)
        days += 365 + (fat_is_leap(y) ? 1 : 0);
    for (int m = 1; m < mon; m++)
        days += mdays[m - 1] + (m == 2 && fat_is_leap(year) ? 1 : 0);
    days += day - 1;

    return days * 86400 + hour * 3600 + min * 60 + sec;
}

/* 调用方持有 fs_global_lock */
static int fat_stat_locked(const char *rel, FILINFO *fno)
{
    FRESULT fr = f_stat(rel, fno);
    return fr == FR_OK ? 0 : fat_errno(fr);
}

static int fat_fill_stat(const char *rel, struct kernel_stat *st)
{
    FILINFO fno;
    memset(st, 0, sizeof(*st));
    st->st_dev = 2;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_ino = fat_ino_pair(rel, "");

    /*
       FatFs 的 f_stat("/") 返回 FR_INVALID_NAME（NS_NONAME 是“卷根本身”，
       f_stat 不接受）。卷根必须在这里合成，否则 ls /boot 会直接 ENOENT。
    */
    if (strcmp(rel, "/") == 0) {
        st->st_mode = 0040755;
        return 0;
    }

    fs_global_lock();
    int rc = fat_stat_locked(rel, &fno);
    fs_global_unlock();
    if (rc < 0)
        return rc;

    st->st_mode = (fno.fattrib & AM_DIR) ? 0040755 : 0100644;
    st->st_size = (int64_t)fno.fsize;
    st->st_blocks = (st->st_size + 511) / 512;

    int64_t t = fat_dos_to_unix(fno.fdate, fno.ftime);
    st->st_mtime_sec = t;
    st->st_atime_sec = t;
    st->st_ctime_sec = t;
    return 0;
}

/* 调用方持有 fs_global_lock；把 [from, to) 写成零（用于越过 EOF 的写/扩展） */
static int fat_zero_fill(FIL *fp, uint64_t from, uint64_t to)
{
    static const uint8_t zeros[64];

    if (to <= from)
        return 0;
    if (to > FAT_MAX_SIZE)
        return -VFS_EINVAL;

    if ((uint64_t)f_tell(fp) != from) {
        FRESULT sr = f_lseek(fp, (FSIZE_t)from);
        if (sr != FR_OK)
            return fat_errno(sr);
    }

    uint64_t gap = to - from;
    while (gap) {
        UINT chunk = gap > sizeof(zeros) ? (UINT)sizeof(zeros) : (UINT)gap;
        UINT bw = 0;
        FRESULT fr = f_write(fp, zeros, chunk, &bw);
        if (fr != FR_OK)
            return fat_errno_write(fr);
        if (bw != chunk)
            return -VFS_ENOSPC;
        gap -= chunk;
    }
    return 0;
}

static UINT fat_clamp_len(size_t len)
{
    return len > 0xFFFFFFFFu ? 0xFFFFFFFFu : (UINT)len;
}

/* ── 文件操作 ───────────────────────────────────────────────────── */

static int fat_file_read(vfs_file_t *file, void *buf, size_t len)
{
    if (!buf)
        return -VFS_EINVAL;
    if ((file->flags & O_ACCMODE_AVATAR) == O_WRONLY_AVATAR)
        return -VFS_EBADF;

    fat_handle_t *h = file->u.fat.handle;
    if (!h || h->is_dir)
        return -VFS_EBADF;
    if (len == 0)
        return 0;

    fs_global_lock();
    FSIZE_t size = f_size(&h->u.f);
    if (file->offset >= (uint64_t)size) {
        fs_global_unlock();
        return 0;
    }
    if ((uint64_t)f_tell(&h->u.f) != file->offset) {
        FRESULT sr = f_lseek(&h->u.f, (FSIZE_t)file->offset);
        if (sr != FR_OK) {
            fs_global_unlock();
            return fat_errno(sr);
        }
    }

    UINT br = 0;
    FRESULT fr = f_read(&h->u.f, buf, fat_clamp_len(len), &br);
    if (fr == FR_OK)
        file->offset = (uint64_t)f_tell(&h->u.f);
    fs_global_unlock();
    return fr == FR_OK ? (int)br : fat_errno(fr);
}

static int fat_write_common(vfs_file_t *file, const void *buf, size_t len, bool honor_append)
{
    if (!buf)
        return -VFS_EINVAL;
    if ((file->flags & O_ACCMODE_AVATAR) == 0)
        return -VFS_EBADF;

    fat_handle_t *h = file->u.fat.handle;
    if (!h || h->is_dir)
        return -VFS_EBADF;
    if (len == 0)
        return 0;

    fs_global_lock();
    FSIZE_t size = f_size(&h->u.f);
    uint64_t target = (honor_append && (file->flags & O_APPEND_AVATAR)) ? (uint64_t)size : file->offset;
    if (target > FAT_MAX_SIZE) {
        fs_global_unlock();
        return -VFS_EINVAL;
    }

    int rc = 0;
    if (target > (uint64_t)size) {
        rc = fat_zero_fill(&h->u.f, (uint64_t)size, target);
    } else if ((uint64_t)f_tell(&h->u.f) != target) {
        FRESULT sr = f_lseek(&h->u.f, (FSIZE_t)target);
        if (sr != FR_OK)
            rc = fat_errno(sr);
    }
    if (rc < 0) {
        fs_global_unlock();
        return rc;
    }

    UINT bw = 0;
    FRESULT fr = f_write(&h->u.f, buf, fat_clamp_len(len), &bw);
    if (fr == FR_OK)
        file->offset = (uint64_t)f_tell(&h->u.f);
    fs_global_unlock();

    if (fr != FR_OK)
        return fat_errno_write(fr);
    return (int)bw;
}

static int fat_file_write(vfs_file_t *file, const void *buf, size_t len)
{
    return fat_write_common(file, buf, len, true);
}

static int fat_file_seek(vfs_file_t *file, int64_t offset, int whence, uint64_t *new_off)
{
    if (!new_off)
        return -VFS_EINVAL;

    fat_handle_t *h = file->u.fat.handle;
    if (!h || h->is_dir)
        return -VFS_ESPIPE;

    int64_t base;
    if (whence == SEEK_SET_AVATAR) {
        base = 0;
    } else if (whence == SEEK_CUR_AVATAR) {
        base = (int64_t)file->offset;
    } else if (whence == SEEK_END_AVATAR) {
        fs_global_lock();
        base = (int64_t)f_size(&h->u.f);
        fs_global_unlock();
    } else {
        return -VFS_EINVAL;
    }

    int64_t target = base + offset;
    if (target < 0)
        return -VFS_EINVAL;

    /* 关键：不要在这里 f_lseek —— FatFs 会立即扩展文件。 */
    file->offset = (uint64_t)target;
    *new_off = file->offset;
    return 0;
}

static int fat_file_pread(vfs_file_t *file, void *buf, size_t len, int64_t offset)
{
    if (offset < 0)
        return -VFS_EINVAL;

    uint64_t saved = file->offset;
    file->offset = (uint64_t)offset;
    int rc = fat_file_read(file, buf, len);
    file->offset = saved;
    return rc;
}

static int fat_file_pwrite(vfs_file_t *file, const void *buf, size_t len, int64_t offset)
{
    if (offset < 0)
        return -VFS_EINVAL;

    uint64_t saved_off = file->offset;
    file->offset = (uint64_t)offset;
    /* pwrite 不受 O_APPEND 影响（不临时改 file->flags，避免 SMP 下的数据竞争） */
    int rc = fat_write_common(file, buf, len, false);
    file->offset = saved_off;
    return rc;
}

static int fat_file_truncate(vfs_file_t *file, uint64_t length)
{
    if ((file->flags & O_ACCMODE_AVATAR) == 0)
        return -VFS_EBADF;

    fat_handle_t *h = file->u.fat.handle;
    if (!h || h->is_dir)
        return -VFS_EBADF;
    if (length > FAT_MAX_SIZE)
        return -VFS_EINVAL;

    fs_global_lock();
    FSIZE_t size = f_size(&h->u.f);
    int rc = 0;

    if (length > (uint64_t)size) {
        rc = fat_zero_fill(&h->u.f, (uint64_t)size, length);
    } else if (length < (uint64_t)size) {
        FRESULT sr = f_lseek(&h->u.f, (FSIZE_t)length);
        if (sr != FR_OK) {
            rc = fat_errno(sr);
        } else {
            FRESULT tr = f_truncate(&h->u.f);
            if (tr != FR_OK)
                rc = fat_errno(tr);
        }
    }
    fs_global_unlock();
    return rc;
}

static int fat_file_sync(vfs_file_t *file)
{
    fat_handle_t *h = file->u.fat.handle;
    if (!h || h->is_dir)
        return -VFS_EBADF;

    fs_global_lock();
    FRESULT fr = f_sync(&h->u.f);
    fs_global_unlock();
    return fr == FR_OK ? 0 : fat_errno(fr);
}

static int fat_file_stat(vfs_file_t *file, struct kernel_stat *st)
{
    fat_handle_t *h = file->u.fat.handle;
    if (!h)
        return -VFS_EBADF;
    return fat_fill_stat(h->rel, st);
}

static int fat_file_close(vfs_file_t *file)
{
    fat_handle_t *h = file->u.fat.handle;
    if (!h)
        return 0;

    int rc = 0;
    fs_global_lock();
    if (h->is_dir) {
        if (f_closedir(&h->u.d) != FR_OK)
            rc = -VFS_EIO;
    } else {
        if (f_close(&h->u.f) != FR_OK)
            rc = -VFS_EIO;
    }
    fs_global_unlock();

    kfree(h);
    file->u.fat.handle = NULL;
    return rc;
}

static const vfs_file_ops_t fat_file_ops = {
    .read = fat_file_read,
    .write = fat_file_write,
    .pread = fat_file_pread,
    .pwrite = fat_file_pwrite,
    .seek = fat_file_seek,
    .stat = fat_file_stat,
    .truncate = fat_file_truncate,
    .sync = fat_file_sync,
    .close = fat_file_close,
};

/* ── 目录操作 ───────────────────────────────────────────────────── */

static void fat_name_copy(char *dst, const char *src, size_t dstsz)
{
    size_t i = 0;
    if (!dstsz)
        return;
    while (src[i] && i + 1 < dstsz) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int fat_dir_getdents(vfs_file_t *file, void *buf, size_t bufsz)
{
    fat_handle_t *h = file->u.fat.handle;
    if (!h || !h->is_dir)
        return -VFS_ENOTDIR;

    char *dst = (char *)buf;
    uint64_t written = 0;
    fs_global_lock();

    while (written + 32 < bufsz) {
        char name[FAT_NAME_MAX];
        uint8_t attr;

        if (h->pending_valid) {
            fat_name_copy(name, h->pending_name, sizeof(name));
            attr = h->pending_attr;
            h->pending_valid = false;
        } else {
            FILINFO fno;
            FRESULT fr = f_readdir(&h->u.d, &fno);
            if (fr != FR_OK) {
                fs_global_unlock();
                return fat_errno(fr);
            }
            if (fno.fname[0] == '\0')
                break; /* 目录结束 */
            if (fno.fattrib & AM_VOL)
                continue; /* 卷标不是目录项 */
            fat_name_copy(name, fno.fname, sizeof(name));
            attr = fno.fattrib;
        }

        size_t namelen = strlen(name);
        if (namelen > 255)
            namelen = 255;
        uint16_t reclen = (uint16_t)(19 + namelen + 1);
        reclen = (reclen + 7) & ~7;

        if (written + reclen > bufsz) {
            /* 放不下：存起来，下一次先吐它，避免 f_readdir 游标丢失条目 */
            fat_name_copy(h->pending_name, name, sizeof(h->pending_name));
            h->pending_attr = attr;
            h->pending_valid = true;
            if (written == 0) {
                /* 缓冲区连一条记录都放不下 */
                h->pending_valid = true;
                fs_global_unlock();
                return -VFS_EINVAL;
            }
            break;
        }

        struct kernel_dirent64 *kd = (struct kernel_dirent64 *)(dst + written);
        kd->d_ino = fat_ino_pair(h->rel, name);
        kd->d_off = (int64_t)(written + reclen);
        kd->d_reclen = reclen;
        kd->d_type = (attr & AM_DIR) ? 4 : 8;
        memcpy(kd->d_name, name, namelen);
        kd->d_name[namelen] = '\0';

        written += reclen;
        file->offset++;
    }

    fs_global_unlock();
    return (int)written;
}

static int fat_dir_stat(vfs_file_t *file, struct kernel_stat *st)
{
    fat_handle_t *h = file->u.fat.handle;
    if (!h)
        return -VFS_EBADF;
    return fat_fill_stat(h->rel, st);
}

static int fat_dir_close(vfs_file_t *file)
{
    return fat_file_close(file);
}

static const vfs_file_ops_t fat_dir_ops = {
    .getdents = fat_dir_getdents,
    .stat = fat_dir_stat,
    .close = fat_dir_close,
};

/* ── mount 级操作 ───────────────────────────────────────────────── */

static int fat_mount_open(const vfs_mount_t *mnt, const char *path, int flags, int mode, vfs_file_t **out)
{
    (void)mode;
    char rel[128];
    int rr = fat_relpath(mnt->path, path, rel, sizeof(rel));
    if (rr < 0)
        return rr;

    fat_handle_t *h = kmalloc(sizeof(*h));
    if (!h)
        return -VFS_ENOMEM;
    memset(h, 0, sizeof(*h));
    fat_name_copy(h->rel, rel, sizeof(h->rel));

    bool want_dir = (flags & O_DIRECTORY_AVATAR) != 0;
    int acc = flags & O_ACCMODE_AVATAR;
    BYTE fmode = FA_READ;
    if (acc == O_WRONLY_AVATAR)
        fmode = FA_WRITE;
    else if (acc == O_RDWR_AVATAR)
        fmode = FA_READ | FA_WRITE;

    FRESULT fr = FR_NO_FILE;

    if (!want_dir) {
        BYTE open_mode = fmode;
        bool prelocked = false;

        if (flags & O_CREAT_AVATAR) {
            if (flags & O_EXCL_AVATAR)
                open_mode |= FA_CREATE_NEW;
            else if (flags & O_TRUNC_AVATAR)
                open_mode |= FA_CREATE_ALWAYS;
            else
                open_mode |= FA_OPEN_ALWAYS;
        } else if (flags & O_TRUNC_AVATAR) {
            /* O_TRUNC 不能像 FatFs 那样顺手创建不存在的文件。
               f_stat 到 f_open 之间必须一直持锁，否则目标可能被换掉。 */
            fs_global_lock();
            prelocked = true;
            if (strcmp(rel, "/") == 0) {
                fs_global_unlock();
                kfree(h);
                return -VFS_EISDIR;
            }
            FILINFO fno;
            FRESULT sfr = f_stat(rel, &fno);
            if (sfr != FR_OK) {
                fs_global_unlock();
                kfree(h);
                return fat_errno(sfr);
            }
            if (fno.fattrib & AM_DIR) {
                fs_global_unlock();
                kfree(h);
                return -VFS_EISDIR;
            }
            open_mode |= FA_CREATE_ALWAYS;
        }

        if (!prelocked)
            fs_global_lock();
        fr = f_open(&h->u.f, rel, open_mode);
        if (fr == FR_OK && (flags & O_APPEND_AVATAR))
            f_lseek(&h->u.f, f_size(&h->u.f));
        fs_global_unlock();

        if (fr == FR_OK) {
            vfs_file_t *file = vfs_alloc_file();
            if (!file) {
                fs_global_lock();
                f_close(&h->u.f);
                fs_global_unlock();
                kfree(h);
                return -VFS_ENOMEM;
            }
            h->is_dir = false;
            file->kind = VFS_FILE_FAT_FILE;
            file->flags = flags;
            file->ops = &fat_file_ops;
            file->u.fat.handle = h;
            vfs_copy_path(file, path);
            *out = file;
            return 0;
        }
    }

    /* 文件方式打不开：再按目录试一次（和 ext4_mount_open 的行为一致） */
    fs_global_lock();
    FRESULT dr = f_opendir(&h->u.d, rel);
    fs_global_unlock();

    if (dr == FR_OK) {
        vfs_file_t *file = vfs_alloc_file();
        if (!file) {
            fs_global_lock();
            f_closedir(&h->u.d);
            fs_global_unlock();
            kfree(h);
            return -VFS_ENOMEM;
        }
        h->is_dir = true;
        file->kind = VFS_FILE_FAT_DIR;
        file->flags = flags;
        file->ops = &fat_dir_ops;
        file->u.fat.handle = h;
        vfs_copy_path(file, path);
        *out = file;
        return 0;
    }

    if (want_dir) {
        /* 目录打不开：用 f_stat 区分“是个文件”与“不存在” */
        FILINFO fno;
        fs_global_lock();
        FRESULT sfr = f_stat(rel, &fno);
        fs_global_unlock();
        kfree(h);
        if (sfr == FR_OK)
            return -VFS_ENOTDIR;
        return fat_errno(sfr);
    }

    kfree(h);
    return fat_errno(fr);
}

static int fat_mount_stat(const vfs_mount_t *mnt, const char *path, struct kernel_stat *st)
{
    char rel[128];
    int rr = fat_relpath(mnt->path, path, rel, sizeof(rel));
    if (rr < 0)
        return rr;
    return fat_fill_stat(rel, st);
}

static int fat_mount_readlink(const vfs_mount_t *mnt, const char *path, char *buf, size_t bufsz)
{
    (void)mnt;
    (void)path;
    (void)buf;
    (void)bufsz;
    /* FAT 没有符号链接；返回 EINVAL 让 follow_symlinks() 停止 */
    return -VFS_EINVAL;
}

static int fat_mount_unlink(const vfs_mount_t *mnt, const char *path, bool is_dir)
{
    char rel[128];
    int rr = fat_relpath(mnt->path, path, rel, sizeof(rel));
    if (rr < 0)
        return rr;

    fs_global_lock();
    FILINFO fno;
    FRESULT fr = f_stat(rel, &fno);
    if (fr != FR_OK) {
        fs_global_unlock();
        return fat_errno(fr);
    }

    bool is_dir_obj = (fno.fattrib & AM_DIR) != 0;
    if (is_dir && !is_dir_obj) {
        fs_global_unlock();
        return -VFS_ENOTDIR;
    }
    if (!is_dir && is_dir_obj) {
        fs_global_unlock();
        return -VFS_EISDIR;
    }

    fr = f_unlink(rel);
    int rc = 0;
    if (fr == FR_DENIED && is_dir_obj) {
        /* 非空目录也会返回 FR_DENIED，探一下区分 ENOTEMPTY 与 EACCES */
        DIR d;
        if (f_opendir(&d, rel) == FR_OK) {
            FILINFO e;
            if (f_readdir(&d, &e) == FR_OK && e.fname[0] != '\0')
                rc = -VFS_ENOTEMPTY;
            else
                rc = -VFS_EACCES;
            f_closedir(&d);
        } else {
            rc = -VFS_EACCES;
        }
    } else if (fr != FR_OK) {
        rc = fat_errno(fr);
    }
    fs_global_unlock();
    return rc;
}

static int fat_mount_mkdir(const vfs_mount_t *mnt, const char *path, int mode)
{
    (void)mode;
    char rel[128];
    int rr = fat_relpath(mnt->path, path, rel, sizeof(rel));
    if (rr < 0)
        return rr;

    fs_global_lock();
    FRESULT fr = f_mkdir(rel);
    fs_global_unlock();
    return fr == FR_OK ? 0 : fat_errno(fr);
}

static int fat_mount_rename(const vfs_mount_t *mnt, const char *oldpath, const char *newpath)
{
    char oldrel[128], newrel[128];
    int rr = fat_relpath(mnt->path, oldpath, oldrel, sizeof(oldrel));
    if (rr < 0)
        return rr;
    rr = fat_relpath(mnt->path, newpath, newrel, sizeof(newrel));
    if (rr < 0)
        return rr;
    if (strcmp(oldrel, newrel) == 0)
        return 0;

    fs_global_lock();
    FILINFO ofno;
    FRESULT fr = f_stat(oldrel, &ofno);
    if (fr != FR_OK) {
        fs_global_unlock();
        return fat_errno(fr);
    }
    bool old_is_dir = (ofno.fattrib & AM_DIR) != 0;

    FILINFO nfno;
    FRESULT nfr = f_stat(newrel, &nfno);
    int rc = 0;
    if (nfr == FR_OK) {
        bool new_is_dir = (nfno.fattrib & AM_DIR) != 0;
        if (old_is_dir && !new_is_dir) {
            rc = -VFS_ENOTDIR;
        } else if (!old_is_dir && new_is_dir) {
            rc = -VFS_EISDIR;
        } else {
            /* POSIX rename 覆盖目标；FatFs 的 f_rename 不覆盖 */
            FRESULT ufr = f_unlink(newrel);
            if (ufr == FR_DENIED && new_is_dir) {
                DIR d;
                if (f_opendir(&d, newrel) == FR_OK) {
                    FILINFO e;
                    if (f_readdir(&d, &e) == FR_OK && e.fname[0] != '\0')
                        rc = -VFS_ENOTEMPTY;
                    else
                        rc = -VFS_EACCES;
                    f_closedir(&d);
                } else {
                    rc = -VFS_EACCES;
                }
            } else if (ufr != FR_OK) {
                rc = fat_errno(ufr);
            }
        }
    } else if (nfr != FR_NO_FILE && nfr != FR_NO_PATH) {
        rc = fat_errno(nfr);
    }

    if (rc == 0) {
        fr = f_rename(oldrel, newrel);
        rc = fr == FR_OK ? 0 : fat_errno(fr);
    }
    fs_global_unlock();
    return rc;
}

const vfs_mount_ops_t fat_mount_ops = {
    .open = fat_mount_open,
    .stat = fat_mount_stat,
    .readlink = fat_mount_readlink,
    .unlink = fat_mount_unlink,
    .mkdir = fat_mount_mkdir,
    .rename = fat_mount_rename,
};
