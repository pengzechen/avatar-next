/*
 * include/pseudofs.h — 虚拟文件系统 API
 *
 * 为 /dev、/proc、/sys 提供内核内建的虚拟路径。
 * syscall.c 在 openat/read/write/ioctl/stat/readlinkat/getdents64 中调用。
 *
 * 设备节点：
 *   /dev/null    — 丢弃写，读返回 EOF
 *   /dev/zero    — 读全零，写丢弃
 *   /dev/tty     — UART 读写
 *   /dev/console — 同 tty
 *   /dev/urandom — 伪随机字节流（LFSR）
 *   /dev/tpu     — CVI TPU 驱动（ioctl 接口）
 *   /dev/ion     — ION DMA 分配器（ioctl 接口）
 *   /dev/npu     — NPU stub（保留）
 *
 * proc 条目：
 *   /proc/self/exe      — 当前进程可执行路径（readlink）
 *   /proc/self/maps     — 空 stub
 *   /proc/self/status   — 简短 stub
 *   /proc/self/fd/N     — readlink → /dev/stdin 等
 *   /proc/mounts        — rootfs 挂载表
 *   /proc/meminfo       — 从 PMM 动态生成
 *   /proc/version       — 静态内核版本字符串
 *   /proc/uptime        — 静态 "0.00 0.00\n"
 *   /proc/cpuinfo       — 架构信息
 */
#ifndef PSEUDOFS_H
#define PSEUDOFS_H

#include "types.h"
#include "kernel_stat.h"

/* ── _IOC 宏（freestanding 环境无 glibc） ────────────────────── */
#ifndef _IOC
#define _IOC(dir, t, nr, sz)  \
    (((uint32_t)(dir) << 30) | ((uint32_t)(t) << 8) | \
     (uint32_t)(nr)          | ((uint32_t)(sz) << 16))
#define _IOC_WRITE  1U
#define _IOC_READ   2U
#define _IOW(t, nr, T)   _IOC(_IOC_WRITE,            (t), (nr), sizeof(T))
#define _IOR(t, nr, T)   _IOC(_IOC_READ,              (t), (nr), sizeof(T))
#define _IOWR(t, nr, T)  _IOC(_IOC_READ|_IOC_WRITE,   (t), (nr), sizeof(T))
#endif

/* ── /dev/ion ioctl 结构体 & 请求码 ──────────────────────────── */
struct ion_alloc_req {
    uint64_t size;      /* [in]  分配字节数                     */
    uint64_t paddr;     /* [out] 物理地址                       */
    uint64_t vaddr;     /* [out] 内核虚拟地址                   */
    uint32_t handle;    /* [out] 句柄（1-based, 0=无效）        */
    uint32_t _pad;
};
struct ion_get_req {
    uint32_t handle;    /* [in]  句柄                           */
    uint32_t _pad;
    uint64_t paddr;     /* [out] 物理地址                       */
    uint64_t vaddr;     /* [out] 内核虚拟地址                   */
};
struct ion_size_req {
    uint32_t handle;    /* [in]  句柄                           */
    uint32_t _pad;
    uint64_t size;      /* [out] 分配大小（字节）               */
};

#define ION_IOC_ALLOC   _IOWR('I', 0, struct ion_alloc_req)
#define ION_IOC_FREE    _IOW ('I', 1, uint32_t)
#define ION_IOC_GET     _IOWR('I', 2, struct ion_get_req)
#define ION_IOC_SIZE    _IOWR('I', 3, struct ion_size_req)

/* ── /dev/tpu ioctl 结构体 & 请求码 ──────────────────────────── */
struct tpu_run_req {
    uint64_t dmabuf_paddr;  /* DmaHeader 的物理地址             */
};

#define TPU_IOC_RUN     _IOW ('T', 0, struct tpu_run_req)
#define TPU_IOC_READY   _IOR ('T', 1, int)

/* ── /dev/npu ioctl（保留，暂无实现） ────────────────────────── */
/* 未来在此定义 NPU_IOC_* */

/* ── API ─────────────────────────────────────────────────────── */

/*
 * pseudo_open: 如果 abspath 是虚拟路径返回节点 ID（≥0），否则返回 -1。
 * 调用者收到 ≥0 时应使用 FDT_PSEUDO 类型打开 fd pool 槽。
 */
int pseudo_open(const char *abspath);

/*
 * pseudo_read: 从节点 nid 的 *off 处读取 len 字节到 buf。
 * 成功时推进 *off，返回读取字节数；EOF 返回 0；错误返回负 errno。
 */
int pseudo_read(int nid, uint64_t *off, void *buf, size_t len);

/*
 * pseudo_write: 向节点 nid 写入 len 字节。
 * 返回写入字节数，或负 errno。
 */
int pseudo_write(int nid, const void *buf, size_t len);

/*
 * pseudo_ioctl: 向设备节点 nid 发送 ioctl 请求。
 * 返回 0 或负 errno。
 */
int pseudo_ioctl(int nid, uint64_t req, void *argp);

/*
 * pseudo_stat_path: 对路径 abspath 填充 *st。
 * 返回 0 表示成功；-ENOENT 表示非虚拟路径。
 */
int pseudo_stat_path(const char *abspath, struct kernel_stat *st);

/*
 * pseudo_fill_stat: 对已知节点 nid 填充 *st。
 */
void pseudo_fill_stat(int nid, struct kernel_stat *st);

/*
 * pseudo_readlink: 解析 abspath 的符号链接目标，写入 buf（最多 bufsz 字节）。
 * 返回写入字节数，或负 errno（非链接路径返回 -EINVAL）。
 */
int pseudo_readlink(const char *abspath, char *buf, size_t bufsz);

/*
 * pseudo_getdents: 枚举伪目录节点 nid 下的条目。
 * *off 为已跳过条目计数，写入 buf，返回写入字节数。
 */
int pseudo_getdents(int nid, uint64_t *off, void *buf, size_t bufsz);

#endif /* PSEUDOFS_H */
