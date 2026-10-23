/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/dma/dma_buf.c — dma-buf 缓冲区对象

   一个 dma_buf 就是「一段物理连续内存 + 一个引用计数」, 用户态通过 fd 持有它。
   内存来源全部交给 dma_heap.c(见那里的说明), 本文件不碰 PMM。

   本文件不接触用户指针 —— ABI 层在 kernel/fs/pseudofs/dev.c,
   那边负责 copy_from/to_user_bytes。这样 driver/ 不需要依赖 syscall 层。
*/
#include "dma_buf.h"

#include "dma_heap.h" /* 同目录 */
#include "cache.h"
#include "klog.h"
#include "mm_vm.h"
#include "spinlock.h"
#include "string.h"
#include "task/task.h"
#include "driver/driver.h"

/*
   fd 池的两个入口用 extern 局部声明, 不 include fd_pool.h：
   后者会拉进 vfs.h → ext4.h, 而 driver/dma 的编译规则只有
   `-Idriver/dma -Ikernel/mm`, 没有 lwext4 的 -I, include 进去直接编不过。
   (kernel/fs/pseudofs/dev.c 里对 task_alloc_ion_fd 也是同样的写法。)
*/
extern int task_alloc_dmabuf_fd(task_t *task, uint32_t idx, const fd_ops_t *ops, void *priv);
extern int task_get_dmabuf_idx(task_t *task, int fd, uint32_t *idx);

struct dma_buf {
    bool used;
    uint32_t refcount;
    uint64_t paddr;     /* 物理基址(页对齐) */
    size_t bytes;       /* 实际大小(页对齐) */
    size_t size;        /* 请求大小(原样返回给用户) */
    uint32_t owner_pid; /* 调试用 */
};

static struct dma_buf g_dma_bufs[DMA_BUF_MAX];
static spinlock_t g_dma_buf_lock = SPINLOCK_INIT;

/* ── 表操作 ────────────────────────────────────────────────────────────── */

dma_buf_t *dma_buf_impl_from_index(uint32_t idx)
{
    if (idx >= DMA_BUF_MAX)
        return NULL;
    dma_buf_t *buf = &g_dma_bufs[idx];
    return buf->used ? buf : NULL;
}

uint32_t dma_buf_impl_index(const dma_buf_t *buf)
{
    if (!buf)
        return DMA_BUF_MAX; /* 哨兵：越界值, from_index 会拒 */
    return (uint32_t)(buf - g_dma_bufs);
}

/* ── 分配 / 引用计数 ───────────────────────────────────────────────────── */

dma_buf_t *dma_buf_impl_alloc(size_t size)
{
    if (size == 0U)
        return NULL;

    uint64_t pa = 0;
    void *va = NULL;
    size_t bytes = 0;
    if (dma_heap_alloc(size, &pa, &va, &bytes) != 0)
        return NULL;

    spin_lock(&g_dma_buf_lock);

    int32_t slot = -1;
    for (uint32_t i = 0U; i < DMA_BUF_MAX; i++) {
        if (!g_dma_bufs[i].used) {
            slot = (int32_t)i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock(&g_dma_buf_lock);
        dma_heap_free(pa, bytes);
        KLOG_ERROR("[dma_buf] table full (max %u)\n", DMA_BUF_MAX);
        return NULL;
    }

    dma_buf_t *buf = &g_dma_bufs[slot];
    buf->used = true;
    buf->refcount = 1U;
    buf->paddr = pa;
    buf->bytes = bytes;
    buf->size = size;
    task_t *cur = task_current();
    buf->owner_pid = cur ? cur->id : 0U;

    spin_unlock(&g_dma_buf_lock);

    KLOG_DEBUG("[dma_buf] alloc idx=%d size=%zu bytes=%zu pa=0x%llx\n", slot, size, bytes, (unsigned long long)pa);
    return buf;
}

void dma_buf_impl_get(dma_buf_t *buf)
{
    if (!buf)
        return;
    spin_lock(&g_dma_buf_lock);
    if (buf->used)
        buf->refcount++;
    spin_unlock(&g_dma_buf_lock);
}

void dma_buf_impl_put(dma_buf_t *buf)
{
    if (!buf)
        return;

    uint64_t pa = 0;
    size_t bytes = 0;

    spin_lock(&g_dma_buf_lock);
    if (!buf->used) {
        /* 重复 put：表项已被释放。当错误吞掉比跟着崩好 —— 这不是内存安全问题,
           只是引用计数纪律被破坏, 日志足够定位。
        */
        spin_unlock(&g_dma_buf_lock);
        KLOG_ERROR("[dma_buf] put on unused slot (double put?)\n");
        return;
    }
    if (buf->refcount > 1U) {
        buf->refcount--;
        spin_unlock(&g_dma_buf_lock);
        return;
    }
    pa = buf->paddr;
    bytes = buf->bytes;
    memset(buf, 0, sizeof(*buf));
    spin_unlock(&g_dma_buf_lock);

    dma_heap_free(pa, bytes);
}

/*
   按物理地址反查缓冲区 —— 排障专用。

   用途：用户态(librknnrt)在任务描述符里填的是设备地址, 我们只拿到
   那个数字, 没法直接读它指向的内容。但它通常指向我们自己发出去的某个
   dma-buf；查出来就能把内容 dump 出来, 否则只能看着一个裸地址猜。

   返回命中的缓冲区(不增加引用计数 —— 只在调用方持锁或用完即走的
   排障路径上使用), 并把页内偏移写回 *off。
*/
dma_buf_t *dma_buf_impl_find_by_phys(uint64_t pa, uint64_t *off)
{
    dma_buf_t *hit = NULL;

    spin_lock(&g_dma_buf_lock);
    for (uint32_t i = 0U; i < DMA_BUF_MAX; i++) {
        dma_buf_t *b = &g_dma_bufs[i];

        if (b->used && pa >= b->paddr && pa < b->paddr + b->bytes) {
            if (off)
                *off = pa - b->paddr;
            hit = b;
            break;
        }
    }
    spin_unlock(&g_dma_buf_lock);

    return hit;
}

void dma_buf_impl_get_index(uint32_t idx)
{
    dma_buf_impl_get(dma_buf_impl_from_index(idx));
}

void dma_buf_impl_put_index(uint32_t idx)
{
    dma_buf_impl_put(dma_buf_impl_from_index(idx));
}

/* ── 属性 ──────────────────────────────────────────────────────────────── */

uint64_t dma_buf_impl_dev_addr(const dma_buf_t *buf)
{
    /*
       全系统唯一的「缓冲区 → 设备地址」出口(见 dma_buf.h 文件头)。
         现在无 IOMMU, 设备看到的就是物理地址；将来接上 SMMU 之后,
         这里改成返回 IOVA, 上层一行都不用动。
    */
    return buf ? buf->paddr : 0U;
}

void *dma_buf_impl_kva(const dma_buf_t *buf)
{
    return buf ? phys_to_virt(buf->paddr) : NULL;
}

size_t dma_buf_impl_size(const dma_buf_t *buf)
{
    return buf ? buf->size : 0U;
}

/* ── 缓存维护 ──────────────────────────────────────────────────────────── */

void dma_buf_impl_sync_region(dma_buf_t *buf, uint64_t off, uint64_t len, bool to_device)
{
    if (!buf || !buf->used || len == 0U)
        return;
    if (off >= buf->bytes)
        return;
    if (len > buf->bytes - off)
        len = buf->bytes - off; /* 夹一次, 调用方算错也不越界 */

    uint8_t *base = (uint8_t *)phys_to_virt(buf->paddr);
    if (to_device)
        clean_dcache_range(base + off, (size_t)len);
    else
        invalidate_dcache_range(base + off, (size_t)len);
}

int dma_buf_impl_apply_sync(uint32_t idx, uint64_t flags)
{
    dma_buf_t *buf = dma_buf_impl_from_index(idx);
    if (!buf)
        return -1;

    /*
       语义照 Linux 的 dma_buf_sync：

         START|READ   CPU 要读设备写的数据 → invalidate(丢掉 CPU 里的旧行)
         END|WRITE    CPU 写完了要给设备看 → clean(写回内存)

       另外两种组合(START|WRITE / END|READ)是空操作, 这是对的：
         - CPU 要写, 不需要先把设备的数据拉进来(整块会被覆盖)；
         - CPU 读完了, 不需要回写(就算有脏行, 它们本来就该回写,
           但那由「CPU 写过」那条路径负责)。

       注意： 把这里实现成空操作 = 静默数据损坏：模型输出会随机出错,
          而且复现不了。这是本阶段最不能省的一行。
    */
    uint64_t op = flags & DMA_BUF_SYNC_RW;
    uint64_t phase = flags & DMA_BUF_SYNC_END;

    if (phase == DMA_BUF_SYNC_START && (op & DMA_BUF_SYNC_READ))
        dma_buf_impl_sync_region(buf, 0, buf->bytes, false /* invalidate */);
    else if (phase == DMA_BUF_SYNC_END && (op & DMA_BUF_SYNC_WRITE))
        dma_buf_impl_sync_region(buf, 0, buf->bytes, true /* clean */);

    KLOG_DEBUG("[dma_buf] sync idx=%u flags=0x%llx (op=%llu phase=%llu)\n", idx, (unsigned long long)flags,
               (unsigned long long)op, (unsigned long long)phase);
    return 0;
}

/* ── fd 操作表 ──────────────────────────────────────────────────────────── */

static int dma_buf_fd_get_phys(void *priv, uint64_t *paddr, uint64_t *size)
{
    dma_buf_t *buf = dma_buf_impl_from_index((uint32_t)(uintptr_t)priv);
    if (!buf)
        return -1;
    *paddr = dma_buf_impl_dev_addr(buf);
    *size = dma_buf_impl_size(buf);
    return 0;
}

static int dma_buf_fd_dup_cb(void *priv)
{
    dma_buf_impl_get_index((uint32_t)(uintptr_t)priv);
    return 0;
}

static void dma_buf_fd_close_cb(void *priv)
{
    dma_buf_impl_put_index((uint32_t)(uintptr_t)priv);
}

static const fd_ops_t dma_buf_fd_ops = {
    .name = "dmabuf",
    .get_phys = dma_buf_fd_get_phys,
    .dup = dma_buf_fd_dup_cb,
    .close = dma_buf_fd_close_cb,
};

/* ── fd 绑定 ───────────────────────────────────────────────────────────── */

int dma_buf_impl_alloc_fd_current(dma_buf_t *buf)
{
    if (!buf)
        return -1;
    task_t *cur = task_current();
    if (!cur)
        return -1;

    uint32_t idx = dma_buf_impl_index(buf);
    int fd = task_alloc_dmabuf_fd(cur, idx, &dma_buf_fd_ops, (void *)(uintptr_t)idx);
    if (fd < 0)
        return -1;

    /* fd 自己持有一份引用；fd 关闭时(fd_obj_close)还回来。 */
    dma_buf_impl_get(buf);
    return fd;
}

dma_buf_t *dma_buf_impl_from_user_fd(int fd)
{
    task_t *cur = task_current();
    if (!cur)
        return NULL;

    uint32_t idx = 0;
    if (task_get_dmabuf_idx(cur, fd, &idx) != 0)
        return NULL;
    return dma_buf_impl_from_index(idx);
}

static const dma_buf_ops_t dma_buf_driver_ops = {
    .alloc = dma_buf_impl_alloc,
    .get = dma_buf_impl_get,
    .put = dma_buf_impl_put,
    .dev_addr = dma_buf_impl_dev_addr,
    .kva = dma_buf_impl_kva,
    .size = dma_buf_impl_size,
    .find_by_phys = dma_buf_impl_find_by_phys,
    .from_index = dma_buf_impl_from_index,
    .index = dma_buf_impl_index,
    .get_index = dma_buf_impl_get_index,
    .put_index = dma_buf_impl_put_index,
    .sync_region = dma_buf_impl_sync_region,
    .alloc_fd_current = dma_buf_impl_alloc_fd_current,
    .from_user_fd = dma_buf_impl_from_user_fd,
    .apply_sync = dma_buf_impl_apply_sync,
};

static int dma_buf_probe(const device_t *dev)
{
    (void)dev; /* service：无设备；分配在首次调用时惰性完成 */
    dma_buf_bind(&dma_buf_driver_ops);
    return 0;
}

DRIVER_ENTRY(driver_dma_buf) = {
    .name = "dma-buf",
    .cls = DRV_DMA,
    .phase = DRV_PHASE_CORE,
    .start_phase = DRV_PHASE_COUNT,
    .probe = dma_buf_probe,
};
