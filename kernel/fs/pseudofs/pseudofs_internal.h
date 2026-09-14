#ifndef KERNEL_FS_PSEUDOFS_INTERNAL_H
#define KERNEL_FS_PSEUDOFS_INTERNAL_H

#include "types.h"
#include "kernel_stat.h"

#define PFS_EINVAL  22
#define PFS_ENOENT   2
#define PFS_ENOSYS  38
#define PFS_EIO      5
#define PFS_EMFILE  24

#define DYNC_PID_DIR_BASE   2000
#define DYNC_PID_STAT_BASE  3000
#define DYNC_PID_PSTAT_BASE 4000

#define MODE_DIR  0040555U
#define MODE_REG  0100444U
#define MODE_CHRW 0020666U
#define MODE_LNK  0120777U

typedef enum {
    PSEUDO_DIR = 0,
    PSEUDO_REG,
    PSEUDO_CHR,
    PSEUDO_LNK,
} pseudo_type_t;

typedef struct {
    const char *path;
    pseudo_type_t type;
    uint32_t mode;
    uint32_t rdev;
    int (*read_fn)(int nid, uint64_t off, void *buf, size_t len);
    int (*write_fn)(int nid, const void *buf, size_t len);
    int (*ioctl_fn)(int nid, uint64_t req, void *argp);
} pseudo_node_t;

size_t pfs_strlen(const char *s);
int pfs_strcmp(const char *a, const char *b);
int pfs_strncmp(const char *a, const char *b, size_t n);
int u64_to_dec(char *buf, uint64_t v);
int pfs_puts(char *buf, size_t pos, size_t bufsz, const char *s);
int pfs_copy_out(uint64_t off, void *buf, size_t len, const char *src, size_t total);

bool pfs_parse_proc_pid(const char *path, uint32_t *pid_out, const char **rest_out);
int pfs_pid_status_read(uint32_t pid, uint64_t off, void *buf, size_t len);
int pfs_pid_stat_read(uint32_t pid, uint64_t off, void *buf, size_t len);
int pfs_self_status_read(uint64_t off, void *buf, size_t len);
int pfs_self_stat_read(uint64_t off, void *buf, size_t len);
int pfs_stat_pid_path(uint32_t pid, const char *rest, struct kernel_stat *st);
bool pfs_task_alive(uint32_t slot);
uint32_t pfs_task_pid(uint32_t slot);

int pfs_node_count(void);
const pseudo_node_t *pfs_node_at(int nid);
const char *pfs_node_name(const pseudo_node_t *node);
int pfs_getdents_node(int nid, uint64_t *off, void *buf, size_t bufsz);

int ion_dev_ioctl(int nid, uint64_t req, void *argp);
int tpu_dev_ioctl(int nid, uint64_t req, void *argp);
int npu_dev_ioctl(int nid, uint64_t req, void *argp);
int video0_read(int nid, uint64_t off, void *buf, size_t len);
int video0_ioctl(int nid, uint64_t req, void *argp);

#endif /* KERNEL_FS_PSEUDOFS_INTERNAL_H */
