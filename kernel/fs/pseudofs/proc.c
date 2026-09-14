#include "pseudofs_internal.h"
#include "task/task.h"
#include "timer/timer.h"
#include "string.h"

extern task_t g_task_pool[TASK_MAX];
extern uint8_t g_stack_used[TASK_MAX];

bool pfs_task_alive(uint32_t slot)
{
    return slot < TASK_MAX && g_stack_used[slot] &&
           g_task_pool[slot].state != TASK_DEAD;
}

uint32_t pfs_task_pid(uint32_t slot)
{
    return g_task_pool[slot].id;
}

bool pfs_parse_proc_pid(const char *path, uint32_t *pid_out, const char **rest_out)
{
    if (pfs_strncmp(path, "/proc/", 6) != 0)
        return false;
    const char *p = path + 6;
    if (pfs_strncmp(p, "self", 4) == 0)
        return false;

    uint32_t pid = 0;
    bool has = false;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10u + (uint32_t)(*p - '0');
        p++;
        has = true;
    }
    if (!has)
        return false;
    *pid_out = pid;
    *rest_out = p;
    return true;
}

static task_t *find_task(uint32_t pid)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (pfs_task_alive(i) && g_task_pool[i].id == pid)
            return &g_task_pool[i];
    }
    return NULL;
}

static size_t format_status(char *tmp, size_t bufsz, task_t *t)
{
    size_t pos = 0;
    char nbuf[24];
    char state_c;
    const char *state_s;

    switch (t->state) {
    case TASK_RUNNING: state_c = 'R'; state_s = "running"; break;
    case TASK_READY:   state_c = 'R'; state_s = "running"; break;
    case TASK_BLOCKED: state_c = 'S'; state_s = "sleeping"; break;
    case TASK_DEAD:    state_c = 'Z'; state_s = "zombie"; break;
    default:           state_c = 'S'; state_s = "sleeping"; break;
    }

    pos += (size_t)pfs_puts(tmp, pos, bufsz, "Name:\t");
    pos += (size_t)pfs_puts(tmp, pos, bufsz, t->name);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, "\nState:\t");
    char sc[2] = { state_c, '\0' };
    pos += (size_t)pfs_puts(tmp, pos, bufsz, sc);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, " (");
    pos += (size_t)pfs_puts(tmp, pos, bufsz, state_s);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, ")\nPid:\t");
    u64_to_dec(nbuf, t->id);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, nbuf);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, "\nPPid:\t");
    u64_to_dec(nbuf, t->parent_id);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, nbuf);

    uint64_t vm_stk_kb = TASK_STACK_SIZE / 1024u;
    uint64_t vm_size_kb = vm_stk_kb;
    if (t->is_user_process) {
        if (t->user_stack_size > 0)
            vm_size_kb += t->user_stack_size / 1024u;
        if (t->heap_end > t->user_entry)
            vm_size_kb += (t->heap_end - t->user_entry) / 1024u;
    }

    pos += (size_t)pfs_puts(tmp, pos, bufsz, "\nVmSize:\t");
    u64_to_dec(nbuf, vm_size_kb);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, nbuf);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, " kB\nVmRSS:\t");
    pos += (size_t)pfs_puts(tmp, pos, bufsz, nbuf);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, " kB\nVmStk:\t");
    u64_to_dec(nbuf, vm_stk_kb);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, nbuf);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, " kB\nThreads:\t1\n");
    return pos;
}

int pfs_pid_status_read(uint32_t pid, uint64_t off, void *buf, size_t len)
{
    task_t *t = find_task(pid);
    if (!t)
        return 0;

    char tmp[640];
    size_t total = format_status(tmp, sizeof(tmp), t);
    return pfs_copy_out(off, buf, len, tmp, total);
}

static int stat_read_task(task_t *t, uint64_t off, void *buf, size_t len)
{
    if (!t)
        return 0;

    char tmp[512];
    size_t pos = 0;
    char nbuf[24];

    u64_to_dec(nbuf, t->id);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), nbuf);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " (");
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), t->name);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), ")");
    char sc = (t->state == TASK_RUNNING || t->state == TASK_READY) ? 'R' : 'S';
    tmp[pos++] = ' ';
    tmp[pos++] = sc;
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
    u64_to_dec(nbuf, t->parent_id);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), nbuf);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " 0 0 0 0 0 0 0 0 0");

    uint64_t now = timer_get_ns();
    uint64_t live_stime = t->stime_ns;
    if (t->sc_entry_ns != 0)
        live_stime += now - t->sc_entry_ns;
    uint64_t wall = (now > t->create_ns) ? now - t->create_ns : 0;
    uint64_t live_utime = (wall > live_stime) ? wall - live_stime : 0;
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
    u64_to_dec(nbuf, live_utime / 10000000ULL);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), nbuf);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " ");
    u64_to_dec(nbuf, live_stime / 10000000ULL);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), nbuf);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " 0 0 20 0 1 0 0");
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " 0 0");
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp),
        " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");

    return pfs_copy_out(off, buf, len, tmp, pos);
}

int pfs_pid_stat_read(uint32_t pid, uint64_t off, void *buf, size_t len)
{
    task_t *t = find_task(pid);
    return t ? stat_read_task(t, off, buf, len) : -PFS_ENOENT;
}

int pfs_self_status_read(uint64_t off, void *buf, size_t len)
{
    task_t *t = task_current();
    return t ? pfs_pid_status_read(t->id, off, buf, len) : 0;
}

int pfs_self_stat_read(uint64_t off, void *buf, size_t len)
{
    return stat_read_task(task_current(), off, buf, len);
}

int pfs_stat_pid_path(uint32_t pid, const char *rest, struct kernel_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_dev = 5;
    st->st_nlink = 1;
    st->st_blksize = 4096;

    if (*rest == '\0') {
        st->st_ino = (uint64_t)(DYNC_PID_DIR_BASE + pid);
        st->st_mode = MODE_DIR;
        st->st_size = 4096;
        return 0;
    }
    if (pfs_strcmp(rest, "/status") == 0) {
        st->st_ino = (uint64_t)(DYNC_PID_STAT_BASE + pid);
        st->st_mode = MODE_REG;
        st->st_size = 512;
        return 0;
    }
    if (pfs_strcmp(rest, "/stat") == 0) {
        st->st_ino = (uint64_t)(DYNC_PID_PSTAT_BASE + pid);
        st->st_mode = MODE_REG;
        st->st_size = 512;
        return 0;
    }
    return -PFS_ENOENT;
}
