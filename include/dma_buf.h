/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/dma_buf.h — dma-buf / dma-heap

   背景：官方 `librknnrt.so` 的每个缓冲区都走这条路 ——

       open("/dev/dma_heap/<heap>") → DMA_HEAP_IOCTL_ALLOC  → dmabuf fd
       DMA_BUF_IOCTL_SYNC(打在那个 fd 上)              → 缓存维护
       mmap(dmabuf fd)                                      → CPU 访问

   所以这不是"给 NPU 用的内部工具", 而是一条用户态可见的 ABI。

   ── 分层(与本项目 ion 的写法一致)────────────────────────────────
     driver/dma/dma_heap.c   内存从哪来(P0: pmm_alloc_pages；P1: 预留区)
     driver/dma/dma_buf.c    缓冲区对象 · 引用计数 · 缓存维护 · fd 绑定
     kernel/fs/pseudofs/dev.c  ABI 处理(用户指针 ← 一律走 copy_*_user)

   ── 设备地址只有这一个出口 ────────────────────────────────────────
   `dma_buf_dev_addr()` 是全系统唯一把缓冲区变成"给设备看的地址"的地方。
   现在它返回物理地址(无 IOMMU)；将来上 IOMMU 时改成返回 IOVA 即可,
   上层(NPU 作业描述符, DMA 基址寄存器)一行都不用动。
   绕过它直接用 phys_to_virt / paddr 的地方, 将来都要改两遍。
*/
#ifndef DMA_BUF_H
#define DMA_BUF_H

#include "types.h"
#include "driver/fd_ops.h"

/* ── 并发缓冲区上限 ──────────────────────────────────────────────────────
   与 FD_POOL_SIZE(128) 对齐：fd 池装不下的在这里也放不下。
*/
#define DMA_BUF_MAX 128U

/* ── 内核侧对象句柄(不透明；外部只拿指针, fd 只存索引)──────────────── */
typedef struct dma_buf dma_buf_t;

/* ── 服务操作表 ──────────────────────────────────────────────────────────
   driver/dma/dma_buf.c 在 probe 时 dma_buf_bind(&ops)。未编译 DMA 时
   指针为 NULL，下面所有包装统一返回失败/空；内核据此决定 /dev/dma_heap
   是否可见。
*/
typedef struct {
    dma_buf_t *(*alloc)(size_t size);
    void (*get)(dma_buf_t *buf);
    void (*put)(dma_buf_t *buf);
    uint64_t (*dev_addr)(const dma_buf_t *buf);
    void *(*kva)(const dma_buf_t *buf);
    size_t (*size)(const dma_buf_t *buf);
    dma_buf_t *(*find_by_phys)(uint64_t pa, uint64_t *off);
    dma_buf_t *(*from_index)(uint32_t idx);
    uint32_t (*index)(const dma_buf_t *buf);
    void (*get_index)(uint32_t idx);
    void (*put_index)(uint32_t idx);
    void (*sync_region)(dma_buf_t *buf, uint64_t off, uint64_t len, bool to_device);
    int (*alloc_fd_current)(dma_buf_t *buf);
    dma_buf_t *(*from_user_fd)(int fd);
    int (*apply_sync)(uint32_t idx, uint64_t flags);
} dma_buf_ops_t;

extern const dma_buf_ops_t *g_dma_buf_ops;
void dma_buf_bind(const dma_buf_ops_t *ops);

static inline bool dma_buf_present(void)
{
    return g_dma_buf_ops != NULL;
}

/* ── 分配 / 引用 ───────────────────────────────────────────────────────── */

static inline dma_buf_t *dma_buf_alloc(size_t size)
{
    return g_dma_buf_ops ? g_dma_buf_ops->alloc(size) : NULL;
}

static inline void dma_buf_get(dma_buf_t *buf)
{
    if (g_dma_buf_ops)
        g_dma_buf_ops->get(buf);
}

static inline void dma_buf_put(dma_buf_t *buf)
{
    if (g_dma_buf_ops)
        g_dma_buf_ops->put(buf);
}

/* ── 属性 ──────────────────────────────────────────────────────────────── */

static inline uint64_t dma_buf_dev_addr(const dma_buf_t *buf)
{
    return g_dma_buf_ops ? g_dma_buf_ops->dev_addr(buf) : 0;
}

static inline void *dma_buf_kva(const dma_buf_t *buf)
{
    return g_dma_buf_ops ? g_dma_buf_ops->kva(buf) : NULL;
}

static inline size_t dma_buf_size(const dma_buf_t *buf)
{
    return g_dma_buf_ops ? g_dma_buf_ops->size(buf) : 0;
}

static inline dma_buf_t *dma_buf_find_by_phys(uint64_t pa, uint64_t *off)
{
    return g_dma_buf_ops ? g_dma_buf_ops->find_by_phys(pa, off) : NULL;
}

/* ── 索引 ↔ 指针(fd 里存的是索引, 不是指针)────────────────────────── */
static inline dma_buf_t *dma_buf_from_index(uint32_t idx)
{
    return g_dma_buf_ops ? g_dma_buf_ops->from_index(idx) : NULL;
}

static inline uint32_t dma_buf_index(const dma_buf_t *buf)
{
    return g_dma_buf_ops ? g_dma_buf_ops->index(buf) : DMA_BUF_MAX;
}

static inline void dma_buf_get_index(uint32_t idx)
{
    if (g_dma_buf_ops)
        g_dma_buf_ops->get_index(idx);
}

static inline void dma_buf_put_index(uint32_t idx)
{
    if (g_dma_buf_ops)
        g_dma_buf_ops->put_index(idx);
}

/* ── 缓存维护 ──────────────────────────────────────────────────────────── */

static inline void dma_buf_sync_region(dma_buf_t *buf, uint64_t off, uint64_t len, bool to_device)
{
    if (g_dma_buf_ops)
        g_dma_buf_ops->sync_region(buf, off, len, to_device);
}

/* ── fd ────────────────────────────────────────────────────────────────── */

static inline int dma_buf_alloc_fd_current(dma_buf_t *buf)
{
    return g_dma_buf_ops ? g_dma_buf_ops->alloc_fd_current(buf) : -1;
}

static inline dma_buf_t *dma_buf_from_user_fd(int fd)
{
    return g_dma_buf_ops ? g_dma_buf_ops->from_user_fd(fd) : NULL;
}

static inline int dma_buf_apply_sync(uint32_t idx, uint64_t flags)
{
    return g_dma_buf_ops ? g_dma_buf_ops->apply_sync(idx, flags) : -1;
}

/* ── ABI 入口（实现在 kernel/fs/pseudofs/dev.c）──────────────────────── */
int dma_heap_dev_ioctl(int nid, uint64_t req, void *argp);
int dma_buf_fd_ioctl(uint32_t idx, uint64_t req, void *argp);

/* ── 用户态 ABI ──────────────────────────────────────────────────────────

   下面这些必须与 librknnrt 逐字节一致。数值不是"设计"出来的, 是反汇编
   librknnrt.so 实测的(详见 docs/npu/RK3588_NPU_DRIVER.md §3)：
     DMA_HEAP_IOCTL_ALLOC = 0xc0184800  → size 字段 0x18 = 24
     DMA_BUF_IOCTL_SYNC   = 0x40086200  → size 字段 0x08 = 8
   size 字段被编进操作码, 所以结构体尺寸写错不是"错位"而是换了个 ioctl 号,
   内核会走到 default 分支。下面 _Static_assert 再钉一道。
*/

/* _IOC 宏：与 pseudofs.h 里那份逐字相同, `#ifndef` 保证谁先到谁生效,
   两边同时包含也不冲突。这里重一份是为了让 driver/ 不依赖 VFS 层的头。
*/
#ifndef _IOC
#define _IOC(dir, t, nr, sz) (((uint32_t)(dir) << 30) | ((uint32_t)(t) << 8) | (uint32_t)(nr) | ((uint32_t)(sz) << 16))
#define _IOC_WRITE           1U
#define _IOC_READ            2U
#define _IO(t, nr)           _IOC(0, (t), (nr), 0)
#define _IOW(t, nr, T)       _IOC(_IOC_WRITE, (t), (nr), sizeof(T))
#define _IOR(t, nr, T)       _IOC(_IOC_READ, (t), (nr), sizeof(T))
#define _IOWR(t, nr, T)      _IOC(_IOC_READ | _IOC_WRITE, (t), (nr), sizeof(T))
#endif

/* struct dma_heap_allocation_data(Linux include/uapi/linux/dma-heap.h) */
struct dma_heap_allocation_data {
    uint64_t len;        /* [in]  请求字节数 */
    uint32_t fd;         /* [out] dmabuf fd */
    uint32_t fd_flags;   /* [in]  给 fd 的 flag(O_RDWR / O_CLOEXEC) */
    uint64_t heap_flags; /* [in]  堆特有参数(我们忽略) */
};
_Static_assert(sizeof(struct dma_heap_allocation_data) == 24,
               "dma_heap_allocation_data 必须是 24 字节 —— librknnrt 用它"
               "算 ioctl 号（0xc0184800），改尺寸等于改 ABI");

#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0, struct dma_heap_allocation_data)

/* struct dma_buf_sync(Linux include/uapi/linux/dma-buf.h) */
#define DMA_BUF_SYNC_READ  (1U << 0)
#define DMA_BUF_SYNC_WRITE (1U << 1)
#define DMA_BUF_SYNC_RW    (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START (0U << 2) /* CPU 访问开始 */
#define DMA_BUF_SYNC_END   (1U << 2) /* CPU 访问结束 */

struct dma_buf_sync {
    uint64_t flags;
};
_Static_assert(sizeof(struct dma_buf_sync) == 8, "dma_buf_sync 必须是 8 字节 —— 理由同上（0x40086200）");

#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

#endif /* DMA_BUF_H */
