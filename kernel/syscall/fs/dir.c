/*
 * fs/dir.c - getcwd / chdir / renameat / unlinkat
 *
 * 由 kernel/syscall/syscall.c 拆出。
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/path.h"
#include "task/task.h"
#include "pseudofs.h"
#include "kernel_stat.h"
#include "string.h"
#include <ext4.h>
#include <ext4_errno.h>

void getcwd_handler(uint64_t regs[6], task_t *current)
{
    char    *buf  = (char *)regs[0];
    uint64_t size = regs[1];
    uint64_t need = strlen(current->cwd) + 1;

    if (size < need) { regs[0] = (uint64_t)(int64_t)-ERANGE; return; }
    if (!buf) { regs[0] = (uint64_t)(int64_t)-EFAULT; return; }
    int n = copy_string_to_user(current->cwd, buf, (int)size);
    regs[0] = (n >= 0) ? (uint64_t)(n + 1) : (uint64_t)(int64_t)-EFAULT;
}

void chdir_handler(uint64_t regs[6], task_t *current)
{
    const char *path = (const char *)regs[0];
    if (!path) { regs[0] = (uint64_t)(int64_t)-ENOENT; return; }
    char abspath[128];
    resolve_path(current->cwd, path, abspath, sizeof(abspath));

    struct kernel_stat tmpst;
    if (pseudo_stat_path(abspath, &tmpst) == 0) {
        int k = 0;
        while (abspath[k] && k < (int)TASK_CWD_LEN - 1) {
            current->cwd[k] = abspath[k]; k++;
        }
        current->cwd[k] = '\0';
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
        current->cwd[k] = abspath[k]; k++;
    }
    current->cwd[k] = '\0';
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
    if (ro < 0) { regs[0] = (uint64_t)(int64_t)ro; return; }

    int rn = resolve_path_at(current, newdirfd, newpath, newabs, sizeof(newabs));
    if (rn < 0) { regs[0] = (uint64_t)(int64_t)rn; return; }

    int rc = ext4_frename(oldabs, newabs);
    regs[0] = (rc == EOK) ? 0 : (uint64_t)(int64_t)-ENOENT;
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

    int rc;
    if (flags & AT_REMOVEDIR) {
        rc = ext4_dir_rm(abspath);
    } else {
        rc = ext4_fremove(abspath);
    }
    regs[0] = (rc == EOK) ? 0 : (uint64_t)(int64_t)-ENOENT;
}
