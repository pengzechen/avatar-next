/*
 * fs/pseudofs/pseudofs.c — 虚拟文件系统实现
 *
 * 节点表（g_pseudo_nodes[]）将路径映射到内容生成器回调。
 * syscall.c 通过 API 函数在各系统调用处集成此模块。
 *
 * 依赖（均已在全局 CFLAGS 中）：
 *   -Iinclude  -Idriver  -Ikernel  -Ikernel/mm
 */

#include "pseudofs.h"
#include "types.h"
#include "arch.h"
#include "klog.h"
#include "string.h"
#include "pmm.h"
#include "uart/uart.h"
#include "task/task.h"
#include "cache.h"

/* ── 外部任务池（来自 kernel/task/task.c） ──────────────────────── */
extern task_t   g_task_pool[TASK_MAX];
extern uint8_t  g_stack_used[TASK_MAX];

/* 动态 PID 节点 ID 空间（不与静态 g_nodes[] 索引冲突） */
#define DYNC_PID_DIR_BASE   2000   /* /proc/<pid>        → nid = 2000+pid */
#define DYNC_PID_STAT_BASE  3000   /* /proc/<pid>/status → nid = 3000+pid */

#if DRIVER_ION
#  include "ion/ion.h"
#endif
#if DRIVER_TPU_CVITPU
#  include "tpu/cvi_tpu.h"
#endif

/* ── errno shims（无 libc 头文件） ────────────────────────────── */
#define PFS_EINVAL  22
#define PFS_ENOENT   2
#define PFS_ENOSYS  38
#define PFS_EIO      5

/* ── 简单字符串工具（不依赖 libc string.h 名称冲突） ─────────── */
static size_t pfs_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int pfs_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int pfs_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? ((unsigned char)*a - (unsigned char)*b) : 0;
}

/* ── 简单无符号整数转十进制字符串 ────────────────────────────── */
static int u64_to_dec(char *buf, uint64_t v)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[24];
    int  n = 0;
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

/* 将字符串写到 buf[pos..] 不超过 bufsz，返回写入字节数 */
static int pfs_puts(char *buf, size_t pos, size_t bufsz, const char *s)
{
    int written = 0;
    while (*s && pos < bufsz) { buf[pos++] = *s++; written++; }
    return written;
}

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
static int tty_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid; (void)off;
    char  *dst = (char *)buf;
    size_t n   = 0;
    while (n < len) {
        dst[n] = uart_getc();
        if (dst[n] == '\r') dst[n] = '\n';
        if (dst[n++] == '\n') break;
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

/* ── 动态 /proc/<pid>/status 生成器 ─────────────────────────── */

/* 根据 PID 在任务池中查找活跃任务 */
static task_t *pfs_find_task(uint32_t pid)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (g_stack_used[i] &&
            g_task_pool[i].state != TASK_DEAD &&
            g_task_pool[i].id == pid)
            return &g_task_pool[i];
    }
    return NULL;
}

/* 生成单个任务的 status 文本，写入 tmp[0..bufsz)，返回写入字节数 */
static size_t fmt_task_status(char *tmp, size_t bufsz, task_t *t)
{
    size_t pos = 0;
    char   nbuf[24];

    /* State 字符 */
    char state_c;
    const char *state_s;
    switch (t->state) {
        case TASK_RUNNING: state_c = 'R'; state_s = "running";  break;
        case TASK_READY:   state_c = 'R'; state_s = "running";  break;
        case TASK_BLOCKED: state_c = 'S'; state_s = "sleeping"; break;
        case TASK_DEAD:    state_c = 'Z'; state_s = "zombie";   break;
        default:           state_c = 'S'; state_s = "sleeping"; break;
    }

    pos += (size_t)pfs_puts(tmp, pos, bufsz, "Name:\t");
    pos += (size_t)pfs_puts(tmp, pos, bufsz, t->name);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, "\nState:\t");
    { char sc[2] = {state_c, '\0'};
      pos += (size_t)pfs_puts(tmp, pos, bufsz, sc); }
    pos += (size_t)pfs_puts(tmp, pos, bufsz, " (");
    pos += (size_t)pfs_puts(tmp, pos, bufsz, state_s);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, ")\nPid:\t");
    u64_to_dec(nbuf, t->id);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, nbuf);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, "\nPPid:\t");
    u64_to_dec(nbuf, t->parent_id);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, nbuf);

    /* 内存估算 */
    uint64_t vm_stk_kb  = TASK_STACK_SIZE / 1024u;
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
    pos += (size_t)pfs_puts(tmp, pos, bufsz, nbuf);   /* RSS ≈ VmSize */
    pos += (size_t)pfs_puts(tmp, pos, bufsz, " kB\nVmStk:\t");
    u64_to_dec(nbuf, vm_stk_kb);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, nbuf);
    pos += (size_t)pfs_puts(tmp, pos, bufsz, " kB\nThreads:\t1\n");

    return pos;
}

/* 读取 /proc/<pid>/status（也供 /proc/self/status 调用） */
static int pid_status_read(uint32_t pid, uint64_t off, void *buf, size_t len)
{
    task_t *t = pfs_find_task(pid);
    if (!t) return 0;

    char   tmp[640];
    size_t total = fmt_task_status(tmp, sizeof(tmp), t);
    if ((size_t)off >= total) return 0;
    size_t avail = total - (size_t)off;
    size_t copy  = avail < len ? avail : len;
    memcpy(buf, tmp + off, copy);
    return (int)copy;
}

/* /proc/self/status — 使用当前任务的真实信息 */
static int status_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    task_t *t = task_current();
    if (!t) return 0;
    return pid_status_read(t->id, off, buf, len);
}

/* /proc/version */
static int version_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    static const char content[] =
        "Linux version 6.1.0-avatar (kernel@avatarOS) "
        "(gcc version 12.0) #1 SMP\n";
    size_t total = sizeof(content) - 1;
    if ((size_t)off >= total) return 0;
    size_t avail = total - (size_t)off;
    size_t copy  = avail < len ? avail : len;
    memcpy(buf, content + off, copy);
    return (int)copy;
}

/* /proc/uptime */
static int uptime_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    static const char content[] = "0.00 0.00\n";
    size_t total = sizeof(content) - 1;
    if ((size_t)off >= total) return 0;
    size_t avail = total - (size_t)off;
    size_t copy  = avail < len ? avail : len;
    memcpy(buf, content + off, copy);
    return (int)copy;
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
    size_t total = sizeof(content) - 1;
    if ((size_t)off >= total) return 0;
    size_t avail = total - (size_t)off;
    size_t copy  = avail < len ? avail : len;
    memcpy(buf, content + off, copy);
    return (int)copy;
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

    size_t total = pos;
    if ((size_t)off >= total) return 0;
    size_t avail = total - (size_t)off;
    size_t copy  = avail < len ? avail : len;
    memcpy(buf, tmp + off, copy);
    return (int)copy;
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
    size_t total = pos;
    if ((size_t)off >= total) return 0;
    size_t avail = total - (size_t)off;
    size_t copy  = avail < len ? avail : len;
    memcpy(buf, tmp + off, copy);
    return (int)copy;
}

/* ── 设备 ioctl 处理器 ────────────────────────────────────────── */

static int ion_dev_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid;
#if DRIVER_ION
    if (req == ION_IOC_ALLOC) {
        struct ion_alloc_req *r = (struct ion_alloc_req *)argp;
        if (!r) return -PFS_EINVAL;
        void *va; uint64_t pa; ion_handle_t h;
        if (ion_alloc((size_t)r->size, &va, &pa, &h) != 0) return -PFS_EIO;
        r->paddr  = pa;
        r->vaddr  = (uint64_t)(uintptr_t)va;
        r->handle = (uint32_t)h;
        KLOG_DEBUG("[pseudofs] /dev/ion alloc size=%llu pa=0x%llx h=%u\n",
                   (unsigned long long)r->size, (unsigned long long)pa, h);
        return 0;
    }
    if (req == ION_IOC_FREE) {
        if (!argp) return -PFS_EINVAL;
        uint32_t h = *(uint32_t *)argp;
        return ion_free((ion_handle_t)h) == 0 ? 0 : -PFS_EINVAL;
    }
    if (req == ION_IOC_GET) {
        struct ion_get_req *r = (struct ion_get_req *)argp;
        if (!r) return -PFS_EINVAL;
        void *va; uint64_t pa;
        if (ion_get_buf((ion_handle_t)r->handle, &va, &pa) != 0) return -PFS_EINVAL;
        r->paddr = pa;
        r->vaddr = (uint64_t)(uintptr_t)va;
        return 0;
    }
    if (req == ION_IOC_SIZE) {
        struct ion_size_req *r = (struct ion_size_req *)argp;
        if (!r) return -PFS_EINVAL;
        r->size = (uint64_t)ion_get_size((ion_handle_t)r->handle);
        return 0;
    }
    if (req == ION_IOC_IMPORT) {
        struct ion_fd_data *r = (struct ion_fd_data *)argp;
        if (!r) return -PFS_EINVAL;
        /* Avatar OS: fd 与 ion handle 使用相同编号 */
        r->handle = (uint32_t)r->fd;
        return 0;
    }
    if (req == ION_IOC_HEAP_QUERY) {
        struct ion_heap_query *q = (struct ion_heap_query *)argp;
        if (!q) return -PFS_EINVAL;
        static const struct ion_heap_data heaps[3] = {
            { "System",      0, 0, 0, 0, 0 },
            { "DmaCoherent", 1, 1, 0, 0, 0 },
            { "Carveout",    2, 2, 0, 0, 0 },
        };
        uint32_t n = (q->cnt < 3) ? q->cnt : 3;
        if (q->heaps && n > 0) {
            struct ion_heap_data *dst =
                (struct ion_heap_data *)(uintptr_t)q->heaps;
            for (uint32_t i = 0; i < n; i++) dst[i] = heaps[i];
        }
        q->cnt = 3;
        return 0;
    }
    return -PFS_ENOSYS;
#else
    (void)req; (void)argp;
    return -PFS_ENOSYS;
#endif
}

static int tpu_dev_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid;
#if DRIVER_TPU_CVITPU
    if (req == CVITPU_SUBMIT_DMABUF) {
        struct cvitpu_submit_dma_arg *r = (struct cvitpu_submit_dma_arg *)argp;
        if (!r) return -PFS_EINVAL;
        void *va; uint64_t pa;
        if (ion_get_buf((ion_handle_t)r->fd, &va, &pa) != 0)
            return -PFS_EINVAL;
        KLOG_DEBUG("[pseudofs] cvi-tpu0 submit fd=%d pa=0x%llx\n",
                   r->fd, (unsigned long long)pa);
        return cvi_tpu_run_dmabuf(va, pa);
    }
    if (req == CVITPU_WAIT_DMABUF) {
        /* 同步驱动：submit 完成时任务已结束 */
        struct cvitpu_wait_dma_arg *r = (struct cvitpu_wait_dma_arg *)argp;
        if (!r) return -PFS_EINVAL;
        r->ret = 0;
        return 0;
    }
    if (req == CVITPU_DMABUF_FLUSH) {
        struct cvitpu_cache_op_arg *r = (struct cvitpu_cache_op_arg *)argp;
        if (!r) return -PFS_EINVAL;
        /* DMA 区域 identity-mapped：物理地址即虚拟地址 */
        clean_and_invalidate_dcache_range(
            (const void *)(uintptr_t)r->paddr, (size_t)r->size);
        return 0;
    }
    if (req == CVITPU_DMABUF_INVLD) {
        struct cvitpu_cache_op_arg *r = (struct cvitpu_cache_op_arg *)argp;
        if (!r) return -PFS_EINVAL;
        invalidate_dcache_range(
            (const void *)(uintptr_t)r->paddr, (size_t)r->size);
        return 0;
    }
    if (req == CVITPU_DMABUF_FLUSH_FD) {
        if (!argp) return -PFS_EINVAL;
        int32_t fd = *(int32_t *)argp;
        void *va; uint64_t pa;
        if (ion_get_buf((ion_handle_t)fd, &va, &pa) != 0) return -PFS_EINVAL;
        size_t sz = ion_get_size((ion_handle_t)fd);
        clean_and_invalidate_dcache_range(va, sz);
        return 0;
    }
    if (req == CVITPU_DMABUF_INVLD_FD) {
        if (!argp) return -PFS_EINVAL;
        int32_t fd = *(int32_t *)argp;
        void *va; uint64_t pa;
        if (ion_get_buf((ion_handle_t)fd, &va, &pa) != 0) return -PFS_EINVAL;
        size_t sz = ion_get_size((ion_handle_t)fd);
        invalidate_dcache_range(va, sz);
        return 0;
    }
    if (req == CVITPU_PIO_MODE)    return 0;
    if (req == CVITPU_LOAD_TEE  ||
        req == CVITPU_SUBMIT_TEE ||
        req == CVITPU_UNLOAD_TEE)  return -PFS_ENOSYS;
    return -PFS_ENOSYS;
#else
    (void)req; (void)argp;
    return -PFS_ENOSYS;
#endif
}

static int npu_dev_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid; (void)req; (void)argp;
    KLOG_WARN("[pseudofs] /dev/npu ioctl: not yet implemented\n");
    return -PFS_ENOSYS;
}

/* ── 节点表 ───────────────────────────────────────────────────── */

typedef enum {
    PSEUDO_DIR = 0,
    PSEUDO_REG,
    PSEUDO_CHR,
    PSEUDO_LNK,
} pseudo_type_t;

typedef struct {
    const char    *path;
    pseudo_type_t  type;
    uint32_t       mode;   /* st_mode 值 */
    uint32_t       rdev;   /* (major<<8)|minor，仅 CHR 有效 */
    int (*read_fn) (int nid, uint64_t off, void *buf, size_t len);
    int (*write_fn)(int nid, const void *buf, size_t len);
    int (*ioctl_fn)(int nid, uint64_t req, void *argp);
} pseudo_node_t;

#define MODE_DIR  0040555U  /* drwxr-xr-x */
#define MODE_REG  0100444U  /* -r--r--r-- */
#define MODE_CHRW 0020666U  /* crw-rw-rw- */
#define MODE_LNK  0120777U  /* lrwxrwxrwx */

static const pseudo_node_t g_nodes[] = {
    /* ── 目录 ──────────────────────────────────────────── */
    { "/dev",              PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
    { "/proc",             PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
    { "/proc/self",        PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
    { "/proc/self/fd",     PSEUDO_DIR, MODE_DIR,  0,              NULL,         NULL,       NULL           },
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

    /* ── /proc 条目 ────────────────────────────────────── */
    { "/proc/self/exe",    PSEUDO_LNK, MODE_LNK,  0,              NULL,         NULL,        NULL          },
    { "/proc/self/maps",   PSEUDO_REG, MODE_REG,  0,              maps_read,    NULL,        NULL          },
    { "/proc/self/status", PSEUDO_REG, MODE_REG,  0,              status_read,  NULL,        NULL          },
    { "/proc/version",     PSEUDO_REG, MODE_REG,  0,              version_read, NULL,        NULL          },
    { "/proc/uptime",      PSEUDO_REG, MODE_REG,  0,              uptime_read,  NULL,        NULL          },
    { "/proc/mounts",      PSEUDO_REG, MODE_REG,  0,              mounts_read,  NULL,        NULL          },
    { "/proc/meminfo",     PSEUDO_REG, MODE_REG,  0,              meminfo_read, NULL,        NULL          },
    { "/proc/cpuinfo",     PSEUDO_REG, MODE_REG,  0,              cpuinfo_read, NULL,        NULL          },
};

#define NODE_COUNT  ((int)(sizeof(g_nodes) / sizeof(g_nodes[0])))

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
static const char *node_name(const pseudo_node_t *n)
{
    const char *p = n->path, *last = n->path;
    while (*p) { if (*p == '/') last = p + 1; p++; }
    return last;
}

/* ── 公共 API ─────────────────────────────────────────────────── */

/* 从 "/proc/<N>" 或 "/proc/<N>/..." 解析数字 PID，成功返回 true */
static bool pfs_parse_proc_pid(const char *path, uint32_t *pid_out,
                               const char **rest_out)
{
    if (pfs_strncmp(path, "/proc/", 6) != 0) return false;
    const char *p = path + 6;
    /* 跳过 "self" */
    if (pfs_strncmp(p, "self", 4) == 0) return false;
    uint32_t pid = 0;
    bool has = false;
    while (*p >= '0' && *p <= '9') { pid = pid * 10u + (uint32_t)(*p - '0'); p++; has = true; }
    if (!has) return false;
    *pid_out  = pid;
    *rest_out = p;   /* '\0' 表示目录本身，"/status" 等表示子文件 */
    return true;
}

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
    }

    return find_node(abspath);
}

int pseudo_read(int nid, uint64_t *off, void *buf, size_t len)
{
    if (!buf || !off) return -PFS_EINVAL;

    /* 动态 /proc/<pid>/status */
    if (nid >= DYNC_PID_STAT_BASE) {
        uint32_t pid = (uint32_t)(nid - DYNC_PID_STAT_BASE);
        int rc = pid_status_read(pid, *off, buf, len);
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
    return n->ioctl_fn(nid, req, argp);
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
        memset(st, 0, sizeof(*st));
        st->st_dev     = 5;
        st->st_nlink   = 1;
        st->st_blksize = 4096;
        if (*rest == '\0') {                          /* 目录 */
            st->st_ino  = (uint64_t)(DYNC_PID_DIR_BASE  + pid);
            st->st_mode = MODE_DIR;
            st->st_size = 4096;
        } else if (pfs_strcmp(rest, "/status") == 0) { /* 文件 */
            st->st_ino  = (uint64_t)(DYNC_PID_STAT_BASE + pid);
            st->st_mode = MODE_REG;
            st->st_size = 512;
        } else {
            return -PFS_ENOENT;
        }
        return 0;
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

/* 往 buf 写一条 dirent64，成功返回 reclen，空间不足返回 0 */
static size_t emit_dirent(void *buf, size_t written, size_t bufsz,
                          uint64_t ino, uint64_t seq,
                          uint8_t dtype, const char *name)
{
    size_t namelen = pfs_strlen(name);
    uint16_t reclen = (uint16_t)(19u + namelen + 1u);
    reclen = (reclen + 7u) & ~7u;
    if (written + reclen > bufsz) return 0;
    struct kernel_dirent64 *kd =
        (struct kernel_dirent64 *)((char *)buf + written);
    kd->d_ino    = ino;
    kd->d_off    = (int64_t)(seq + 1u);
    kd->d_reclen = reclen;
    kd->d_type   = dtype;
    memcpy(kd->d_name, name, namelen + 1);
    return reclen;
}

int pseudo_getdents(int nid, uint64_t *off, void *buf, size_t bufsz)
{
    if (!buf || !bufsz || !off) return -PFS_EINVAL;

    /* ── 动态 /proc/<pid> 目录：只有 "status" 一个子项 ────────── */
    if (nid >= (int)DYNC_PID_DIR_BASE && nid < (int)DYNC_PID_STAT_BASE) {
        uint32_t pid = (uint32_t)(nid - DYNC_PID_DIR_BASE);
        size_t written = 0;
        uint64_t idx   = 0;
        if (idx >= *off) {
            size_t r = emit_dirent(buf, written, bufsz,
                                   (uint64_t)(DYNC_PID_STAT_BASE + pid),
                                   idx, 8 /* REG */, "status");
            if (r == 0) return (int)written;
            written += r;
            (*off)++;
        }
        return (int)written;
    }

    if (nid < 0 || nid >= NODE_COUNT) return -PFS_EINVAL;
    if (g_nodes[nid].type != PSEUDO_DIR) return -PFS_EINVAL;

    const char *dir_path = g_nodes[nid].path;
    size_t      dir_len  = pfs_strlen(dir_path);
    bool        is_root  = (dir_len == 1 && dir_path[0] == '/');
    bool        is_proc  = (pfs_strcmp(dir_path, "/proc") == 0);

    size_t   written = 0;
    uint64_t idx     = 0;   /* 全局条目序号，用于定位 *off */

    for (int i = 0; i < NODE_COUNT; i++) {
        const pseudo_node_t *child = &g_nodes[i];
        if (child == &g_nodes[nid]) continue;

        /* 判断是否为直接子节点 */
        const char *cp = child->path;
        if (!is_root) {
            if (pfs_strncmp(cp, dir_path, dir_len) != 0) continue;
            if (cp[dir_len] != '/') continue;
            /* 检查 dir_path/ 之后没有更多 '/' */
            const char *rest = cp + dir_len + 1;
            bool direct = true;
            for (const char *r = rest; *r; r++) {
                if (*r == '/') { direct = false; break; }
            }
            if (!direct) continue;
        } else {
            /* 根目录子节点：cp 必须为 /xxx（一层） */
            if (cp[0] != '/') continue;
            const char *rest = cp + 1;
            if (!*rest) continue;
            bool direct = true;
            for (const char *r = rest; *r; r++) {
                if (*r == '/') { direct = false; break; }
            }
            if (!direct) continue;
        }

        if (idx < *off) { idx++; continue; }

        uint8_t dtype;
        switch (child->type) {
            case PSEUDO_DIR: dtype = 4;  break;
            case PSEUDO_REG: dtype = 8;  break;
            case PSEUDO_CHR: dtype = 2;  break;
            case PSEUDO_LNK: dtype = 10; break;
            default:         dtype = 0;  break;
        }
        size_t r = emit_dirent(buf, written, bufsz,
                               (uint64_t)(unsigned)i + 1u, idx, dtype,
                               node_name(child));
        if (r == 0) break;
        written += r;
        idx++;
        (*off)++;
    }

    /* ── /proc 额外枚举活跃进程 PID 目录 ──────────────────────── */
    if (is_proc) {
        for (uint32_t i = 0; i < TASK_MAX; i++) {
            if (!g_stack_used[i] || g_task_pool[i].state == TASK_DEAD)
                continue;

            if (idx < *off) { idx++; continue; }

            uint32_t pid = g_task_pool[i].id;
            char     pidbuf[12];
            u64_to_dec(pidbuf, (uint64_t)pid);
            size_t r = emit_dirent(buf, written, bufsz,
                                   (uint64_t)(DYNC_PID_DIR_BASE + pid),
                                   idx, 4 /* DIR */, pidbuf);
            if (r == 0) break;
            written += r;
            idx++;
            (*off)++;
        }
    }

    return (int)written;
}
