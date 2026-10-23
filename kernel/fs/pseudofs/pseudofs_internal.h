#ifndef KERNEL_FS_PSEUDOFS_INTERNAL_H
#define KERNEL_FS_PSEUDOFS_INTERNAL_H

#include "types.h"
#include "kernel_stat.h"

#define PFS_EINVAL 22
#define PFS_ENOENT 2
#define PFS_ENOSYS 38
#define PFS_EIO    5
#define PFS_EMFILE 24
#define PFS_EAGAIN 11
#define PFS_EBUSY  16
#define PFS_ENOMEM 12

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

/*
   节点操作表。

   注意： 新字段追加在末尾, 同时必须把 g_nodes[] 里每一行都补上对应的
   NULL**：那张表用的是位置初始化, 而 -Wextra 带 -Wmissing-field-initializers
   —— 少补一行就是一条警告(曾经以为"补零即可, 不用改表", 实测会报 35 条)。
   只有确实需要新钩子的那一行填真函数。

     poll_fn  — 返回 EPOLL* 位。NULL 表示「总是可读可写」(老行为)。
     close_fn — 最后一个引用关闭时调用。NULL 表示无需清理。
     ioctl_file_fn / close_file_fn — 见下面各自的说明。
*/
typedef struct {
    const char *path;
    pseudo_type_t type;
    uint32_t mode;
    uint32_t rdev;
    int (*read_fn)(int nid, uint64_t off, void *buf, size_t len);
    int (*write_fn)(int nid, const void *buf, size_t len);
    int (*ioctl_fn)(int nid, uint64_t req, void *argp);
    uint32_t (*poll_fn)(int nid);
    int (*close_fn)(int nid);

    /*
       「按打开的文件」版本的回调(同样追加在末尾, 老节点补零即可)。

       只有少数节点需要：DRM 渲染节点的 GEM handle 是按 drm_file 隔离的
       (两个进程各开一次就得各有一张表), 而上面那对回调只拿得到 nid ——
       一个节点号分不出是哪个 open。用全局表的话两个进程的 handle 会串号。

       为 NULL 时自动退回上面那对老回调, 所以现存节点一行都不用改。
       参数是 vfs_file_t*, 但 pseudofs 不 include vfs.h(会拖进 ext4.h),
       所以走 void*, 由实现自己转。
    */
    int (*ioctl_file_fn)(void *file, uint64_t req, void *argp);
    int (*close_file_fn)(void *file);
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
/* /dev/dma_heap/ 各节点 —— 原型在 include/dma_buf.h, 这里只做个索引注释 */
int dma_heap_dev_ioctl(int nid, uint64_t req, void *argp);

/* /dev/vmm —— 宿主 shell 里控制 guest 的字符设备(vmm_dev.c) */
int vmm_dev_write(int nid, const void *buf, size_t len);
int vmm_dev_read(int nid, uint64_t off, void *buf, size_t len);
int vmm_dev_ioctl(int nid, uint64_t req, void *argp);
uint32_t vmm_dev_poll(int nid);
int vmm_dev_close(int nid);
int npu_dev_ioctl(int nid, uint64_t req, void *argp);
int video0_read(int nid, uint64_t off, void *buf, size_t len);
int video0_ioctl(int nid, uint64_t req, void *argp);

#endif /* KERNEL_FS_PSEUDOFS_INTERNAL_H */
