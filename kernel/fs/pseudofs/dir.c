#include "pseudofs_internal.h"
#include "kernel_stat.h"
#include "task/task.h"
#include "string.h"

static uint8_t dirent_type(pseudo_type_t type)
{
    switch (type) {
    case PSEUDO_DIR: return 4;
    case PSEUDO_REG: return 8;
    case PSEUDO_CHR: return 2;
    case PSEUDO_LNK: return 10;
    default: return 0;
    }
}

static size_t emit_dirent(void *buf, size_t written, size_t bufsz,
                          uint64_t ino, uint64_t seq,
                          uint8_t dtype, const char *name)
{
    size_t namelen = pfs_strlen(name);
    uint16_t reclen = (uint16_t)(19u + namelen + 1u);
    reclen = (reclen + 7u) & ~7u;
    if (written + reclen > bufsz)
        return 0;

    struct kernel_dirent64 *kd =
        (struct kernel_dirent64 *)((char *)buf + written);
    kd->d_ino = ino;
    kd->d_off = (int64_t)(seq + 1u);
    kd->d_reclen = reclen;
    kd->d_type = dtype;
    memcpy(kd->d_name, name, namelen + 1);
    return reclen;
}

static bool is_direct_child(const char *dir_path, const char *child_path)
{
    size_t dir_len = pfs_strlen(dir_path);
    bool is_root = (dir_len == 1 && dir_path[0] == '/');

    if (is_root) {
        if (child_path[0] != '/')
            return false;
        const char *rest = child_path + 1;
        if (!*rest)
            return false;
        for (const char *p = rest; *p; p++) {
            if (*p == '/')
                return false;
        }
        return true;
    }

    if (pfs_strncmp(child_path, dir_path, dir_len) != 0)
        return false;
    if (child_path[dir_len] != '/')
        return false;

    const char *rest = child_path + dir_len + 1;
    for (const char *p = rest; *p; p++) {
        if (*p == '/')
            return false;
    }
    return true;
}

static int emit_proc_pid_dirs(uint64_t *off, void *buf, size_t bufsz,
                              size_t *written, uint64_t idx)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!pfs_task_alive(i))
            continue;
        if (idx < *off) {
            idx++;
            continue;
        }

        uint32_t pid = pfs_task_pid(i);
        char pidbuf[12];
        u64_to_dec(pidbuf, (uint64_t)pid);
        size_t r = emit_dirent(buf, *written, bufsz,
                               (uint64_t)(DYNC_PID_DIR_BASE + pid),
                               idx, 4, pidbuf);
        if (r == 0)
            break;
        *written += r;
        idx++;
        (*off)++;
    }
    return (int)*written;
}

int pfs_getdents_node(int nid, uint64_t *off, void *buf, size_t bufsz)
{
    if (!buf || !bufsz || !off)
        return -PFS_EINVAL;

    if (nid >= (int)DYNC_PID_DIR_BASE && nid < (int)DYNC_PID_STAT_BASE) {
        uint32_t pid = (uint32_t)(nid - DYNC_PID_DIR_BASE);
        size_t written = 0;
        uint64_t idx = 0;
        if (idx >= *off) {
            size_t r = emit_dirent(buf, written, bufsz,
                                   (uint64_t)(DYNC_PID_STAT_BASE + pid),
                                   idx, 8, "status");
            if (r == 0)
                return (int)written;
            written += r;
            (*off)++;
        }
        return (int)written;
    }

    const pseudo_node_t *dir = pfs_node_at(nid);
    if (!dir || dir->type != PSEUDO_DIR)
        return -PFS_EINVAL;

    size_t written = 0;
    uint64_t idx = 0;
    int count = pfs_node_count();
    for (int i = 0; i < count; i++) {
        const pseudo_node_t *child = pfs_node_at(i);
        if (!child || child == dir || !is_direct_child(dir->path, child->path))
            continue;
        if (idx < *off) {
            idx++;
            continue;
        }

        size_t r = emit_dirent(buf, written, bufsz,
                               (uint64_t)(unsigned)i + 1u, idx,
                               dirent_type(child->type),
                               pfs_node_name(child));
        if (r == 0)
            break;
        written += r;
        idx++;
        (*off)++;
    }

    if (pfs_strcmp(dir->path, "/proc") == 0)
        return emit_proc_pid_dirs(off, buf, bufsz, &written, idx);
    return (int)written;
}
