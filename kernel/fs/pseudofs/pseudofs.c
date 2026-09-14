/*
 * kernel/fs/pseudofs/pseudofs.c — 虚拟文件系统实现
 *
 * 节点表（g_pseudo_nodes[]）将路径映射到内容生成器回调。
 * syscall.c 通过 API 函数在各系统调用处集成此模块。
 *
 * 依赖（均已在全局 CFLAGS 中）：
 *   -Iinclude  -Idriver  -Ikernel  -Ikernel/mm
 */

#include "pseudofs.h"
#include "pseudofs_internal.h"
#include "types.h"
#include "arch.h"
#include "klog.h"
#include "string.h"
#include "pmm.h"
#include "uart/uart.h"
#include "task/task.h"

/* ── Urandom LFSR ─────────────────────────────────────────────── */
static uint64_t g_lfsr = 0xDEADBEEFCAFEBABEULL;

static uint8_t lfsr_byte(void)
{
    g_lfsr ^= g_lfsr >> 7;
    g_lfsr ^= g_lfsr << 9;
    g_lfsr ^= g_lfsr >> 13;
    return (uint8_t)(g_lfsr & 0xFFU);
}

/* ── 内容生成器 ───────────────────────────────────────────────── */

/* /dev/null */
static int null_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid; (void)off; (void)buf; (void)len;
    return 0;
}
static int null_write(int nid, const void *buf, size_t len)
{
    (void)nid; (void)buf;
    return (int)len;
}

/* /dev/zero */
static int zero_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid; (void)off;
    memset(buf, 0, len);
    return (int)len;
}

/* /dev/tty, /dev/console */
extern int tty_getchar_nb(char *c);  /* syscall.c: drain UART + pop ring buf */
extern int termios_is_raw(void);     /* syscall.c: g_termios ICANON check */
extern int termios_do_icrnl(void);   /* syscall.c: g_termios ICRNL check */

static int tty_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid; (void)off;
    int raw   = termios_is_raw();
    int do_cr = termios_do_icrnl();
    char  *dst = (char *)buf;
    size_t n   = 0;
    while (n < len) {
        char c;
        if (tty_getchar_nb(&c)) {
            if (do_cr && c == '\r') c = '\n';
            dst[n++] = c;
            if (raw || c == '\n') break; /* raw: 单字符; canonical: 换行截止 */
        } else {
            task_yield();
        }
    }
    return (int)n;
}
static int tty_write(int nid, const void *buf, size_t len)
{
    (void)nid;
    const char *src = (const char *)buf;
    for (size_t i = 0; i < len; i++)
        uart_putc(src[i]);
    return (int)len;
}

/* /dev/urandom, /dev/random */
static int rand_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid; (void)off;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        dst[i] = lfsr_byte();
    return (int)len;
}

/* /proc/self/maps — musl 启动时读取，空文件即可 */
static int maps_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid; (void)off; (void)buf; (void)len;
    return 0;
}

static int status_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    return pfs_self_status_read(off, buf, len);
}

static int stat_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    return pfs_self_stat_read(off, buf, len);
}

/* /proc/version */
static int version_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    static const char content[] =
        "Linux version 6.1.0-avatar (kernel@avatarOS) "
        "(gcc version 12.0) #1 SMP\n";
    return pfs_copy_out(off, buf, len, content, sizeof(content) - 1);
}

/* /proc/uptime */
static int uptime_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    static const char content[] = "0.00 0.00\n";
    return pfs_copy_out(off, buf, len, content, sizeof(content) - 1);
}

/* /proc/mounts */
static int mounts_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    static const char content[] =
        "rootfs / ext4 rw,relatime 0 0\n"
        "devtmpfs /dev devtmpfs rw 0 0\n"
        "proc /proc proc rw 0 0\n"
        "sysfs /sys sysfs rw 0 0\n";
    return pfs_copy_out(off, buf, len, content, sizeof(content) - 1);
}

/* /proc/meminfo — 从 PMM 动态生成 */
static int meminfo_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    char tmp[512];
    size_t pos = 0;

    uint64_t total_kb = 0, free_kb = 0;
    if (g_pmm) {
        total_kb = (g_pmm->total_pages * g_pmm->page_size) / 1024ULL;
        free_kb  = (g_pmm->free_pages  * g_pmm->page_size) / 1024ULL;
    }

    char nbuf[24];
    /* MemTotal */
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "MemTotal:       ");
    u64_to_dec(nbuf, total_kb);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), nbuf);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " kB\n");
    /* MemFree */
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "MemFree:        ");
    u64_to_dec(nbuf, free_kb);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), nbuf);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " kB\n");
    /* MemAvailable */
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "MemAvailable:   ");
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), nbuf);
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), " kB\n");
    /* Stubs */
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp),
        "Buffers:               0 kB\n"
        "Cached:                0 kB\n"
        "SwapTotal:             0 kB\n"
        "SwapFree:              0 kB\n");

    return pfs_copy_out(off, buf, len, tmp, pos);
}

/* /proc/cpuinfo */
static int cpuinfo_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    char tmp[512];
    size_t pos = 0;
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "processor\t: 0\nBogoMIPS\t: 100.00\n");
#if ARCH_AARCH64
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "CPU architecture: AArch64\nHardware\t: ARM Cortex-A\n");
#elif ARCH_RISCV64
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "CPU architecture: riscv64\nHardware\t: RISC-V\n");
#else
    pos += (size_t)pfs_puts(tmp, pos, sizeof(tmp), "CPU architecture: x86_64\nHardware\t: x86_64\n");
#endif
    return pfs_copy_out(off, buf, len, tmp, pos);
}

/* ── 节点表 ───────────────────────────────────────────────────── */

/* /proc/sys/kernel/pid_max */
static int pid_max_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    const char *s = "4096\n";
    size_t slen = 5;
    if ((size_t)off >= slen) return 0;
    size_t avail = slen - (size_t)off;
    size_t copy  = avail < len ? avail : len;
    memcpy(buf, s + off, copy);
    return (int)copy;
}

static const pseudo_node_t g_nodes[] = {
    /* ── 目录 ──────────────────────────────────────────── */
    { "/dev",              PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
    { "/proc",             PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
    { "/proc/self",        PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
    { "/proc/self/fd",     PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
    { "/proc/sys",         PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
    { "/proc/sys/kernel",  PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
    { "/sys",              PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },

    /* ── /dev 字符设备 ─────────────────────────────────── */
    { "/dev/null",         PSEUDO_CHR, MODE_CHRW, (1U<<8)|3U,    null_read,   null_write,  NULL           },
    { "/dev/zero",         PSEUDO_CHR, MODE_CHRW, (1U<<8)|5U,    zero_read,   null_write,  NULL           },
    { "/dev/tty",          PSEUDO_CHR, MODE_CHRW, (5U<<8)|0U,    tty_read,    tty_write,   NULL           },
    { "/dev/console",      PSEUDO_CHR, MODE_CHRW, (5U<<8)|1U,    tty_read,    tty_write,   NULL           },
    { "/dev/urandom",      PSEUDO_CHR, MODE_CHRW, (1U<<8)|9U,    rand_read,   null_write,  NULL           },
    { "/dev/random",       PSEUDO_CHR, MODE_CHRW, (1U<<8)|8U,    rand_read,   null_write,  NULL           },
    /* 加速器设备 */
    { "/dev/cvi-tpu0",     PSEUDO_CHR, MODE_CHRW, (240U<<8)|0U,   NULL,         null_write,  tpu_dev_ioctl },
    { "/dev/ion",          PSEUDO_CHR, MODE_CHRW, (10U <<8)|56U,  NULL,         null_write,  ion_dev_ioctl },
    { "/dev/npu",          PSEUDO_CHR, MODE_CHRW, (10U <<8)|242U, NULL,         null_write,  npu_dev_ioctl },
    { "/dev/video0",       PSEUDO_CHR, MODE_CHRW, (81U <<8)|0U,   video0_read,  null_write,  video0_ioctl  },

    /* ── /proc 条目 ────────────────────────────────────── */
    { "/proc/self/exe",    PSEUDO_LNK, MODE_LNK,  0,              NULL,         NULL,        NULL          },
    { "/proc/self/maps",   PSEUDO_REG, MODE_REG,  0,              maps_read,    NULL,        NULL          },
    { "/proc/self/stat",   PSEUDO_REG, MODE_REG,  0,              stat_read,    NULL,        NULL          },
    { "/proc/self/status", PSEUDO_REG, MODE_REG,  0,              status_read,  NULL,        NULL          },
    { "/proc/version",     PSEUDO_REG, MODE_REG,  0,              version_read, NULL,        NULL          },
    { "/proc/uptime",      PSEUDO_REG, MODE_REG,  0,              uptime_read,  NULL,        NULL          },
    { "/proc/mounts",      PSEUDO_REG, MODE_REG,  0,              mounts_read,  NULL,        NULL          },
    { "/proc/meminfo",     PSEUDO_REG, MODE_REG,  0,              meminfo_read, NULL,        NULL          },
    { "/proc/cpuinfo",     PSEUDO_REG, MODE_REG,  0,              cpuinfo_read, NULL,        NULL          },
    { "/proc/sys/kernel/pid_max", PSEUDO_REG, MODE_REG, 0,      pid_max_read, NULL,        NULL          },
};

#define NODE_COUNT  ((int)(sizeof(g_nodes) / sizeof(g_nodes[0])))

int pfs_node_count(void)
{
    return NODE_COUNT;
}

const pseudo_node_t *pfs_node_at(int nid)
{
    if (nid < 0 || nid >= NODE_COUNT)
        return NULL;
    return &g_nodes[nid];
}

/* ── 内部辅助 ─────────────────────────────────────────────────── */

static int find_node(const char *path)
{
    for (int i = 0; i < NODE_COUNT; i++) {
        if (pfs_strcmp(g_nodes[i].path, path) == 0)
            return i;
    }
    return -1;
}

/* 节点路径的最后一个分量（即名称）*/
const char *pfs_node_name(const pseudo_node_t *n)
{
    const char *p = n->path, *last = n->path;
    while (*p) { if (*p == '/') last = p + 1; p++; }
    return last;
}

/* ── 公共 API ─────────────────────────────────────────────────── */

int pseudo_open(const char *abspath)
{
    if (!abspath) return -1;
    /* /proc/self/fd/<N> 归并到 /proc/self/fd 目录节点 */
    if (pfs_strncmp(abspath, "/proc/self/fd/", 14) == 0)
        return find_node("/proc/self/fd");

    /* /proc/<pid>  或  /proc/<pid>/status */
    uint32_t pid; const char *rest;
    if (pfs_parse_proc_pid(abspath, &pid, &rest)) {
        if (*rest == '\0')                            /* /proc/<pid> */
            return (int)(DYNC_PID_DIR_BASE  + pid);
        if (pfs_strcmp(rest, "/status") == 0)         /* /proc/<pid>/status */
            return (int)(DYNC_PID_STAT_BASE + pid);
        if (pfs_strcmp(rest, "/stat") == 0)           /* /proc/<pid>/stat */
            return (int)(DYNC_PID_PSTAT_BASE + pid);
    }

    return find_node(abspath);
}

int pseudo_read(int nid, uint64_t *off, void *buf, size_t len)
{
    if (!buf || !off) return -PFS_EINVAL;

    /* 动态 /proc/<pid>/stat */
    if (nid >= DYNC_PID_PSTAT_BASE) {
        uint32_t pid = (uint32_t)(nid - DYNC_PID_PSTAT_BASE);
        int rc = pfs_pid_stat_read(pid, *off, buf, len);
        if (rc > 0) *off += (uint64_t)rc;
        return rc;
    }

    /* 动态 /proc/<pid>/status */
    if (nid >= DYNC_PID_STAT_BASE) {
        uint32_t pid = (uint32_t)(nid - DYNC_PID_STAT_BASE);
        int rc = pfs_pid_status_read(pid, *off, buf, len);
        if (rc > 0) *off += (uint64_t)rc;
        return rc;
    }
    /* 动态 /proc/<pid> 目录：无内容可读 */
    if (nid >= DYNC_PID_DIR_BASE) return 0;

    if (nid < 0 || nid >= NODE_COUNT) return -PFS_EINVAL;
    const pseudo_node_t *n = &g_nodes[nid];
    if (!n->read_fn) return 0;  /* 空文件 */
    int rc = n->read_fn(nid, *off, buf, len);
    if (rc > 0) *off += (uint64_t)rc;
    return rc;
}

int pseudo_write(int nid, const void *buf, size_t len)
{
    if (nid < 0 || nid >= NODE_COUNT || !buf) return -PFS_EINVAL;
    const pseudo_node_t *n = &g_nodes[nid];
    if (!n->write_fn) return -PFS_EINVAL;
    return n->write_fn(nid, buf, len);
}

int pseudo_ioctl(int nid, uint64_t req, void *argp)
{
    if (nid < 0 || nid >= NODE_COUNT) return -PFS_EINVAL;
    const pseudo_node_t *n = &g_nodes[nid];
    if (!n->ioctl_fn) return -PFS_ENOSYS;
    return n->ioctl_fn(nid, (uint32_t)req, argp);
}

int pseudo_stat_path(const char *abspath, struct kernel_stat *st)
{
    if (!abspath || !st) return -PFS_EINVAL;
    /* /proc/self/fd/<N> */
    if (pfs_strncmp(abspath, "/proc/self/fd/", 14) == 0) {
        int nid = find_node("/proc/self/fd");
        if (nid >= 0) { pseudo_fill_stat(nid, st); return 0; }
    }

    /* /proc/<pid>  或  /proc/<pid>/status */
    uint32_t pid; const char *rest;
    if (pfs_parse_proc_pid(abspath, &pid, &rest)) {
        return pfs_stat_pid_path(pid, rest, st);
    }

    int nid = find_node(abspath);
    if (nid < 0) return -PFS_ENOENT;
    pseudo_fill_stat(nid, st);
    return 0;
}

void pseudo_fill_stat(int nid, struct kernel_stat *st)
{
    memset(st, 0, sizeof(*st));
    if (nid < 0 || nid >= NODE_COUNT) return;
    const pseudo_node_t *n = &g_nodes[nid];
    st->st_dev     = 5;                    /* 虚拟设备号 */
    st->st_ino     = (uint64_t)(unsigned)nid + 1U;
    st->st_mode    = n->mode;
    st->st_nlink   = 1;
    st->st_blksize = 4096;
    st->st_rdev    = (uint64_t)n->rdev;
    /* 目录和符号链接给个合理的 size */
    if (n->type == PSEUDO_LNK) st->st_size = 64;
    if (n->type == PSEUDO_DIR) st->st_size = 4096;
}

int pseudo_readlink(const char *abspath, char *buf, size_t bufsz)
{
    if (!abspath || !buf || !bufsz) return -PFS_EINVAL;

    /* /proc/self/exe → 当前进程的可执行路径 */
    if (pfs_strcmp(abspath, "/proc/self/exe") == 0) {
        task_t     *t   = task_current();
        const char *exe = (t && t->exe_path[0]) ? t->exe_path : "/unknown";
        size_t n   = pfs_strlen(exe);
        size_t copy = n < bufsz ? n : bufsz;
        memcpy(buf, exe, copy);
        return (int)copy;
    }

    /* /proc/self/fd/<N> → 合成路径（仅 0/1/2） */
    if (pfs_strncmp(abspath, "/proc/self/fd/", 14) == 0) {
        int fdnum = 0;
        const char *p = abspath + 14;
        while (*p >= '0' && *p <= '9')
            fdnum = fdnum * 10 + (*p++ - '0');

        const char *target = NULL;
        if      (fdnum == 0) target = "/dev/stdin";
        else if (fdnum == 1) target = "/dev/stdout";
        else if (fdnum == 2) target = "/dev/stderr";

        if (target) {
            size_t n    = pfs_strlen(target);
            size_t copy = n < bufsz ? n : bufsz;
            memcpy(buf, target, copy);
            return (int)copy;
        }
        return -PFS_ENOENT;
    }

    return -PFS_EINVAL;
}

int pseudo_getdents(int nid, uint64_t *off, void *buf, size_t bufsz)
{
    return pfs_getdents_node(nid, off, buf, bufsz);
}
