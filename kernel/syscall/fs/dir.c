/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   fs/dir.c - getcwd / chdir / renameat / unlinkat

   由 kernel/syscall/syscall.c 拆出。
*/
#include "syscall/syscall_internal.h"
#include "syscall/fs/path.h"
#include "task/task.h"
#include "kernel_stat.h"
#include "string.h"
#include "page.h" /* PAGE_SIZE */
#include "vfs.h"
#include <ext4.h>
#include <ext4_errno.h>

void getcwd_handler(uint64_t regs[6], task_t *current)
{
    char *buf = (char *)regs[0];
    uint64_t size = regs[1];
    uint64_t need = strlen(current->fs->cwd) + 1;

    if (size < need) {
        regs[0] = (uint64_t)(int64_t)-ERANGE;
        return;
    }
    if (!buf) {
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }
    int n = copy_string_to_user(current->fs->cwd, buf, (int)size);
    regs[0] = (n >= 0) ? (uint64_t)(n + 1) : (uint64_t)(int64_t)-EFAULT;
}

#define CHDIR_NAME_MAX 255u  /* Linux NAME_MAX */
#define CHDIR_PATH_MAX 4096u /* Linux PATH_MAX */

/*
   chdir_copy_canonical - 分块读取用户路径并规范化到内核缓冲

   为什么不能简单 copy_string_from_user 后判长度：
     LTP chdir02 会传 1..PATH_MAX-1 个连续的 '/' —— Linux 认为它们
     都是合法的根目录路径，而 128 字节的 cwd 缓冲一截断就误报
     ENAMETOOLONG。反过来，chdir04 里 256 字节的**单个组件**必须报
     ENAMETOOLONG。

   所以按 Linux 规则判定：组件 > NAME_MAX 或总长 > PATH_MAX 才算超长；
   规范化（合并 '/'、处理 '.'/'..'）之后若仍放不进 cwd 缓冲，才按
   本内核的限制返回 ENAMETOOLONG。

   逐页分块拷贝，避免"字符串贴在页尾、下一字节未映射"被整段预检误杀。
*/
static int chdir_copy_canonical(const char *upath, char *out, int outsz)
{
    char comp[CHDIR_NAME_MAX + 1];
    char chunk[128];
    int clen = 0;
    int olen = 0;
    bool abs = false;
    bool started = false;
    uint64_t total = 0;
    uint64_t i = 0;

    for (;;) {
        uint64_t addr = (uint64_t)upath + i;
        uint64_t room = PAGE_SIZE - (addr & (PAGE_SIZE - 1));
        uint64_t want = (room < sizeof(chunk)) ? room : sizeof(chunk);

        if (copy_from_user_bytes(upath + i, chunk, want) < 0)
            return -EFAULT;

        for (uint64_t k = 0; k < want; k++) {
            char c = chunk[k];

            if (!started) {
                started = true;
                if (c == '/')
                    abs = true;
            }

            if (c != '/' && c != '\0') {
                if (clen >= (int)CHDIR_NAME_MAX)
                    return -ENAMETOOLONG; /* 单个组件超过 NAME_MAX */
                comp[clen++] = c;
                if (++total >= CHDIR_PATH_MAX)
                    return -ENAMETOOLONG;
                continue;
            }

            /* 一个组件结束：处理 "." / ".." / 普通名字 */
            if (clen > 0) {
                if (clen == 1 && comp[0] == '.') {
                    /* 当前目录，忽略 */
                } else if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
                    if (olen == 0) {
                        if (!abs) {
                            if (olen + 3 > outsz - 1)
                                return -ENAMETOOLONG;
                            if (olen)
                                out[olen++] = '/';
                            out[olen++] = '.';
                            out[olen++] = '.';
                        }
                    } else {
                        int start = olen;
                        while (start > 0 && out[start - 1] != '/')
                            start--;
                        int last_len = olen - start;
                        if (last_len == 2 && out[start] == '.' && out[start + 1] == '.') {
                            /* 上一个已经是 ".."，再追加一个 */
                            if (olen + 3 > outsz - 1)
                                return -ENAMETOOLONG;
                            out[olen++] = '/';
                            out[olen++] = '.';
                            out[olen++] = '.';
                        } else {
                            olen = start;
                            if (olen > 0)
                                olen--; /* 去掉分隔 '/' */
                        }
                    }
                } else {
                    int sep = ((abs && olen == 0) || olen > 0) ? 1 : 0;
                    if (olen + sep + clen + 1 > outsz)
                        return -ENAMETOOLONG;
                    if (sep)
                        out[olen++] = '/';
                    memcpy(out + olen, comp, (size_t)clen);
                    olen += clen;
                }
                clen = 0;
            }

            if (c == '\0') {
                if (olen == 0) {
                    if (outsz < 2)
                        return -ENAMETOOLONG;
                    out[0] = abs ? '/' : '.';
                    out[1] = '\0';
                } else {
                    out[olen] = '\0';
                }
                return 0;
            }

            if (++total >= CHDIR_PATH_MAX)
                return -ENAMETOOLONG;
        }

        i += want;
    }
}

void chdir_handler(uint64_t regs[6], task_t *current)
{
    const char *upath = (const char *)regs[0];
    if (!upath) {
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }

    /*
       用户指针必须先拷进内核：resolve_path() 会直接逐字节解引用。
       规范化结果必须放进 TASK_CWD_LEN。
    */
    char kpath[TASK_CWD_LEN];
    int cr = chdir_copy_canonical(upath, kpath, (int)sizeof(kpath));
    if (cr < 0) {
        regs[0] = (uint64_t)(int64_t)cr;
        return;
    }

    char abspath[128];
    resolve_path(current->fs->cwd, kpath, abspath, sizeof(abspath));

    struct kernel_stat tmpst;
    if (vfs_stat_path(abspath, &tmpst) == 0) {
        /* 普通文件不能 chdir —— Linux 返回 ENOTDIR */
        if ((tmpst.st_mode & 0170000) != 0040000) {
            regs[0] = (uint64_t)(int64_t)-ENOTDIR;
            return;
        }
        int k = 0;
        while (abspath[k] && k < (int)TASK_CWD_LEN - 1) {
            current->fs->cwd[k] = abspath[k];
            k++;
        }
        current->fs->cwd[k] = '\0';
        regs[0] = 0;
        return;
    }

    ext4_dir d;
    if (ext4_dir_open(&d, abspath) != EOK) {
        regs[0] = (uint64_t)(int64_t)-ENOENT;
        return;
    }
    ext4_dir_close(&d);

    int k = 0;
    while (abspath[k] && k < (int)TASK_CWD_LEN - 1) {
        current->fs->cwd[k] = abspath[k];
        k++;
    }
    current->fs->cwd[k] = '\0';
    regs[0] = 0;
}

void renameat_handler(uint64_t regs[6], task_t *current)
{
    int olddirfd = (int)regs[0];
    const char *oldpath = (const char *)regs[1];
    int newdirfd = (int)regs[2];
    const char *newpath = (const char *)regs[3];

    if (!oldpath || !newpath) {
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }

    char oldabs[128];
    char newabs[128];

    int ro = resolve_path_at(current, olddirfd, oldpath, oldabs, sizeof(oldabs));
    if (ro < 0) {
        regs[0] = (uint64_t)(int64_t)ro;
        return;
    }

    int rn = resolve_path_at(current, newdirfd, newpath, newabs, sizeof(newabs));
    if (rn < 0) {
        regs[0] = (uint64_t)(int64_t)rn;
        return;
    }

    /* 走 VFS：同一挂载内 rename；跨挂载返回 -EXDEV，让 busybox 走 copy+unlink */
    int rc = vfs_rename(oldabs, newabs);
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
}

void renameat2_handler(uint64_t regs[6], task_t *current)
{
    if (regs[4] != 0) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }
    renameat_handler(regs, current);
}

void unlinkat_handler(uint64_t regs[6], task_t *current)
{
    int dirfd = (int)regs[0];
    const char *pathname = (const char *)regs[1];
    int flags = (int)regs[2];

    if (!pathname) {
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }

    char abspath[128];
    int rpa = resolve_path_at(current, dirfd, pathname, abspath, sizeof(abspath));
    if (rpa < 0) {
        regs[0] = (uint64_t)(int64_t)rpa;
        return;
    }

    int rc = vfs_unlink(abspath, (flags & AT_REMOVEDIR) != 0);
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
}
