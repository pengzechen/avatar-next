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

/* ── 并发缓冲区上限 ──────────────────────────────────────────────────────
   与 FD_POOL_SIZE(128) 对齐：fd 池装不下的在这里也放不下。
*/
#define DMA_BUF_MAX 128U

/* ── 内核侧对象句柄(不透明；外部只拿指针, fd 只存索引)──────────────── */
typedef struct dma_buf dma_buf_t;

/* ── 分配 / 引用 ───────────────────────────────────────────────────────── */

/* 从默认堆分配一块物理连续的缓冲区；失败返回 NULL。 */
dma_buf_t *dma_buf_alloc(size_t size);

/* 引用计数 +1 / -1。归零时归还内存并释放表项。 */
void dma_buf_get(dma_buf_t *buf);
void dma_buf_put(dma_buf_t *buf);

/* ── 属性 ──────────────────────────────────────────────────────────────── */

/*
   设备可见地址 —— 全系统唯一出口, 见文件头。
   无 IOMMU 时即物理地址；缓冲区为连续分配, 所以一个基址就够,
   不需要 sg_table 那套。
*/
uint64_t dma_buf_dev_addr(const dma_buf_t *buf);

/* 内核虚拟地址(phys_to_virt 直接映射) */
void *dma_buf_kva(const dma_buf_t *buf);

/* 分配时请求的字节数(不是页对齐后的值) */
size_t dma_buf_size(const dma_buf_t *buf);

/*
   按物理地址反查缓冲区(排障用)：用户态填在描述符里的是设备地址,
   拿它查一下就能 dump 出内容, 否则只能对着一个裸地址猜。
   命中时把页内偏移写回 *off。不增加引用计数 —— 只给诊断路径用。
*/
dma_buf_t *dma_buf_find_by_phys(uint64_t pa, uint64_t *off);

/* ── 索引 ↔ 指针(fd 里存的是索引, 不是指针)────────────────────────── */
dma_buf_t *dma_buf_from_index(uint32_t idx);
uint32_t dma_buf_index(const dma_buf_t *buf);
/* 按索引增减引用(fd 关闭 / GEM 对象持有等路径用；会重做索引校验) */
void dma_buf_get_index(uint32_t idx);
void dma_buf_put_index(uint32_t idx);

/* ── 缓存维护 ──────────────────────────────────────────────────────────── */

/*
   to_device=true  → CPU 写完, 交给设备前清理(clean)
   to_device=false → 设备写完, CPU 读之前作废(invalidate)

   off/len 由调用方保证在缓冲区范围内(内部仍会夹一次)。
*/
void dma_buf_sync_region(dma_buf_t *buf, uint64_t off, uint64_t len, bool to_device);

/* ── fd ────────────────────────────────────────────────────────────────── */

/* 给当前任务分配一个引用 buf 的 fd；失败返回负 errno。 */
int dma_buf_alloc_fd_current(dma_buf_t *buf);

/* fd → buf(按当前任务解析)；不是 dma-buf fd 返回 NULL。 */
dma_buf_t *dma_buf_from_user_fd(int fd);

/*
   执行一次整块 DMA_BUF_SYNC。flags 是已经从用户态拷好的值。
   返回 0 成功, -1 索引无效。
*/
int dma_buf_apply_sync(uint32_t idx, uint64_t flags);

/* ── ABI 入口 ────────────────────────────────────────────────────────────

   实现放在 kernel/fs/pseudofs/dev.c —— 本项目所有设备 ioctl 都在那儿,
   而且只有那一层碰用户指针(CLAUDE.md 的 copy_*_user 规则)。
*/

/* /dev/dma_heap/ 下各节点的 ioctl_fn。
   (写这个注释时踩过 CLAUDE.md 记的坑：路径里的"斜杠加星号"会提前
     开一层嵌套注释, -Wcomment 就是这么来的。)
*/
int dma_heap_dev_ioctl(int nid, uint64_t req, void *argp);

/* 打在 dmabuf fd 本身 上的 ioctl(DMA_BUF_IOCTL_SYNC 就是这么发的) */
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
