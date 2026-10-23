/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#include "pseudofs.h"
#include "pseudofs_internal.h"
#include "cache.h"
#include "klog.h"
#include "task/task.h"

#include "driver/ion.h"
#include "driver/tpu.h"
#include "dma_buf.h"
#include "syscall/syscall_internal.h" /* copy_from/to_user_bytes */

/*
   出错时把刚刚分配给用户的 fd 收回来。

   为什么需要：ALLOC 是「分配缓冲区 → 拿 fd → 把 fd 写回用户态」三步,
   最后一步会失败(用户传个可读不可写, 或已 munmap 的地址)。失败时不收回,
   缓冲区内存和 fd 池槽位就一起泄漏 —— 而且每次调用漏一个。

   必须走完整的 close 序列(take → close → free)：只调 fd_pool_free 会让
   dma_buf 的引用计数少还一份, 内存永不释放。

   用 extern 局部声明而不是 include fd_pool.h：后者会拉进 vfs.h → ext4.h,
   而 pseudofs 的编译规则没有 lwext4 的 -I(本文件上面 ion_alloc_fd_for_current
   也是同样的写法)。越界检查由 fd_obj_close / fd_pool_free 自己兜。
*/
static void dmabuf_close_fresh_fd(int fd)
{
    extern int task_take_fd(task_t * task, int fd);
    extern void fd_obj_close(int idx);
    extern void fd_pool_free(int idx);

    task_t *cur = task_current();
    if (!cur || fd < 0)
        return;
    int idx = task_take_fd(cur, fd);
    if (idx < 0)
        return;
    fd_obj_close(idx); /* 内部会把 dma_buf 那份引用还掉 */
    fd_pool_free(idx);
}

/*
   /dev/dma_heap/<name> —— 官方 librknnrt 的每一块缓冲区都从这里分配,
   见 docs/npu/RK3588_NPU_DRIVER.md §2。

   注意： 用户指针一律走 copy_*_user(CLAUDE.md 铁律)。本文件里既有的
      ion_dev_ioctl 是裸解引用(`r->size = ...` 直接写用户内存),
      用户传非法地址就是内核态写坏地址 → #PF → 整机挂死。别照抄那一段。
*/
int dma_heap_dev_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid;

    if (!dma_buf_present())
        return -PFS_ENOSYS;

    if (req != DMA_HEAP_IOCTL_ALLOC)
        return -PFS_ENOSYS;

    struct dma_heap_allocation_data args;
    if (!argp || copy_from_user_bytes(argp, &args, sizeof(args)) < 0)
        return -PFS_EINVAL;
    if (args.len == 0U)
        return -PFS_EINVAL;

    dma_buf_t *buf = dma_buf_alloc((size_t)args.len);
    if (!buf)
        return -PFS_ENOMEM;

    size_t size = dma_buf_size(buf);

    /*
       引用计数走法(对齐 ion 的写法)：
         dma_buf_alloc            → 1("创建"那份)
         dma_buf_alloc_fd_current → 2(fd 拿走一份)
         dma_buf_put(下面那句)  → 1 —— 只剩 fd 那份
         fd 关闭时 fd_obj_close    → 0 → 内存还给堆
    */
    int fd = dma_buf_alloc_fd_current(buf);
    if (fd < 0) {
        dma_buf_put(buf);
        return -PFS_EMFILE;
    }
    dma_buf_put(buf);

    args.fd = (uint32_t)fd;
    args.len = (uint64_t)size;
    if (copy_to_user_bytes(&args, argp, sizeof(args)) < 0) {
        dmabuf_close_fresh_fd(fd);
        return -PFS_EINVAL;
    }

    KLOG_DEBUG("[dma_heap] alloc len=%llu -> fd=%d\n", (unsigned long long)size, fd);
    return 0;
}

/*
   打在 dmabuf fd 自身 上的 ioctl。librknnrt 的 DMA_BUF_IOCTL_SYNC 就是这么
   发的 —— 它不走 /dev/ 节点, 所以必须由 ioctl_handler 按 fd 类型分派过来
   (见 kernel/syscall/fs/ioctl.c 里的 FDT_DMABUF 分支)。
*/
int dma_buf_fd_ioctl(uint32_t idx, uint64_t req, void *argp)
{
    if (req != DMA_BUF_IOCTL_SYNC)
        return -PFS_ENOSYS;

    struct dma_buf_sync args;
    if (!argp || copy_from_user_bytes(argp, &args, sizeof(args)) < 0)
        return -PFS_EINVAL;

    if (dma_buf_apply_sync(idx, args.flags) != 0)
        return -PFS_EINVAL;
    return 0;
}

static int ion_alloc_fd_for_current(ion_handle_t handle)
{
    extern int task_alloc_ion_fd(task_t * task, uint32_t handle, const fd_ops_t *ops, void *priv);

    task_t *current = task_current();
    if (!current || !g_ion_ops || !g_ion_ops->fd_ops)
        return -PFS_EINVAL;

    int fd = task_alloc_ion_fd(current, (uint32_t)handle, g_ion_ops->fd_ops, (void *)(uintptr_t)handle);
    return fd >= 0 ? fd : -PFS_EMFILE;
}

static ion_handle_t ion_handle_from_user_fd(int fd)
{
    extern int task_get_ion_handle(task_t * task, int fd, uint32_t *handle);

    task_t *current = task_current();
    uint32_t handle = 0;
    if (current && task_get_ion_handle(current, fd, &handle) == 0)
        return (ion_handle_t)handle;
    return (ion_handle_t)fd;
}

static int tpu_cache_fd_op(int32_t fd, int invalidate)
{
    void *va;
    uint64_t pa;
    ion_handle_t handle = ion_handle_from_user_fd(fd);

    if (ion_get_buf(handle, &va, &pa) != 0)
        return -PFS_EINVAL;

    size_t sz = ion_get_size(handle);
    if (invalidate)
        invalidate_dcache_range(va, sz);
    else
        clean_and_invalidate_dcache_range(va, sz);

    KLOG_DEBUG("[pseudofs] cvi-tpu0 cache_%s fd=%d h=%u pa=0x%llx size=0x%llx\n", invalidate ? "invld" : "flush", fd,
               handle, (unsigned long long)pa, (unsigned long long)sz);
    return 0;
}

static int tpu_cache_range_op(const struct cvitpu_legacy_cache_op_arg *op, int invalidate)
{
    if (!op || op->size == 0)
        return -PFS_EINVAL;

    void *va = (void *)(uintptr_t)op->paddr;
    uint64_t pa = op->paddr;
    ion_handle_t handle = ION_HANDLE_INVALID;

    if (op->fd >= 0) {
        void *base_va;
        uint64_t base_pa;
        handle = ion_handle_from_user_fd(op->fd);
        if (ion_get_buf(handle, &base_va, &base_pa) == 0) {
            size_t ion_size = ion_get_size(handle);
            if (op->paddr >= base_pa && op->paddr < base_pa + ion_size) {
                uint64_t off = op->paddr - base_pa;
                if (off + op->size <= ion_size)
                    va = (void *)((uintptr_t)base_va + off);
            }
        }
    }

    if (invalidate)
        invalidate_dcache_range(va, (size_t)op->size);
    else
        clean_and_invalidate_dcache_range(va, (size_t)op->size);

    KLOG_DEBUG("[pseudofs] cvi-tpu0 cache_%s_range fd=%d h=%u pa=0x%llx va=%p size=0x%llx\n",
               invalidate ? "invld" : "flush", op->fd, handle, (unsigned long long)pa, va,
               (unsigned long long)op->size);
    return 0;
}

int ion_dev_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid;

    if (!ion_present())
        return -PFS_ENOSYS;

    if (req == ION_IOC_ALLOC || req == ION_IOC_CVI_ALLOC) {
        struct ion_alloc_req *r = (struct ion_alloc_req *)argp;
        if (!r)
            return -PFS_EINVAL;
        void *va;
        uint64_t pa;
        ion_handle_t h;
        if (ion_alloc((size_t)r->size, &va, &pa, &h) != 0)
            return -PFS_EIO;
        int fd = ion_alloc_fd_for_current(h);
        if (fd < 0) {
            ion_free(h);
            return fd;
        }
        r->size = (uint64_t)ion_get_size(h);
        r->fd = fd;
        r->paddr = pa;
        KLOG_DEBUG("[pseudofs] /dev/ion alloc size=%llu pa=0x%llx fd=%d h=%u\n", (unsigned long long)r->size,
                   (unsigned long long)pa, fd, h);
        return 0;
    }
    if (req == ION_IOC_FREE) {
        if (!argp)
            return -PFS_EINVAL;
        uint32_t h = *(uint32_t *)argp;
        if (ion_get_size((ion_handle_t)h) == 0)
            h = (uint32_t)ion_handle_from_user_fd((int)h);
        return ion_free((ion_handle_t)h) == 0 ? 0 : -PFS_EINVAL;
    }
    if (req == ION_IOC_GET) {
        struct ion_get_req *r = (struct ion_get_req *)argp;
        if (!r)
            return -PFS_EINVAL;
        void *va;
        uint64_t pa;
        ion_handle_t handle = (ion_handle_t)r->handle;
        if (ion_get_size(handle) == 0)
            handle = ion_handle_from_user_fd((int)r->handle);
        if (ion_get_buf(handle, &va, &pa) != 0)
            return -PFS_EINVAL;
        r->handle = (uint32_t)handle;
        r->paddr = pa;
        r->vaddr = (uint64_t)(uintptr_t)va;
        return 0;
    }
    if (req == ION_IOC_SIZE) {
        struct ion_size_req *r = (struct ion_size_req *)argp;
        if (!r)
            return -PFS_EINVAL;
        ion_handle_t handle = (ion_handle_t)r->handle;
        size_t sz = ion_get_size(handle);
        if (sz == 0) {
            handle = ion_handle_from_user_fd((int)r->handle);
            sz = ion_get_size(handle);
        }
        r->handle = (uint32_t)handle;
        r->size = (uint64_t)sz;
        return 0;
    }
    if (req == ION_IOC_IMPORT) {
        struct ion_fd_data *r = (struct ion_fd_data *)argp;
        if (!r)
            return -PFS_EINVAL;
        ion_handle_t handle = ion_handle_from_user_fd(r->fd);
        if (ion_get_size(handle) == 0)
            return -PFS_EINVAL;
        r->handle = (uint32_t)handle;
        return 0;
    }
    if (req == ION_IOC_HEAP_QUERY) {
        struct ion_heap_query *q = (struct ion_heap_query *)argp;
        if (!q)
            return -PFS_EINVAL;
        static const struct ion_heap_data heaps[3] = {
            { "System", 0, 0, 0, 0, 0 },
            { "DmaCoherent", 1, 1, 0, 0, 0 },
            { "Carveout", 2, 2, 0, 0, 0 },
        };
        uint32_t n = (q->cnt < 3) ? q->cnt : 3;
        if (q->heaps && n > 0) {
            struct ion_heap_data *dst = (struct ion_heap_data *)(uintptr_t)q->heaps;
            for (uint32_t i = 0; i < n; i++)
                dst[i] = heaps[i];
        }
        q->cnt = 3;
        return 0;
    }
    KLOG_WARN("[pseudofs] /dev/ion unsupported ioctl req=0x%llx\n", (unsigned long long)req);
    return -PFS_ENOSYS;
}

int tpu_dev_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid;

    if (!tpu_present())
        return -PFS_ENOSYS;

    if (req == CVITPU_SUBMIT_DMABUF || req == CVITPU_LEGACY_SUBMIT_DMABUF) {
        struct cvitpu_submit_dma_arg *r = (struct cvitpu_submit_dma_arg *)argp;
        if (!r)
            return -PFS_EINVAL;
        void *va;
        uint64_t pa;
        ion_handle_t handle = ion_handle_from_user_fd(r->fd);
        if (ion_get_buf(handle, &va, &pa) != 0)
            return -PFS_EINVAL;
        KLOG_DEBUG("[pseudofs] cvi-tpu0 submit fd=%d h=%u seq=%u pa=0x%llx\n", r->fd, handle, r->seq_no,
                   (unsigned long long)pa);
        return cvi_tpu_run_dmabuf(va, pa);
    }
    if (req == CVITPU_WAIT_DMABUF || req == CVITPU_LEGACY_WAIT_DMABUF) {
        struct cvitpu_wait_dma_arg *r = (struct cvitpu_wait_dma_arg *)argp;
        if (!r)
            return -PFS_EINVAL;
        r->ret = 0;
        return 0;
    }
    if (req == CVITPU_DMABUF_FLUSH) {
        struct cvitpu_cache_op_arg *r = (struct cvitpu_cache_op_arg *)argp;
        if (!r)
            return -PFS_EINVAL;
        clean_and_invalidate_dcache_range((const void *)(uintptr_t)r->paddr, (size_t)r->size);
        return 0;
    }
    if (req == CVITPU_DMABUF_INVLD) {
        struct cvitpu_cache_op_arg *r = (struct cvitpu_cache_op_arg *)argp;
        if (!r)
            return -PFS_EINVAL;
        invalidate_dcache_range((const void *)(uintptr_t)r->paddr, (size_t)r->size);
        return 0;
    }
    if (req == CVITPU_DMABUF_FLUSH_FD || req == CVITPU_LEGACY_DMABUF_FLUSH_FD) {
        if (!argp)
            return -PFS_EINVAL;
        return tpu_cache_fd_op(*(int32_t *)argp, 0);
    }
    if (req == CVITPU_DMABUF_INVLD_FD || req == CVITPU_LEGACY_DMABUF_INVLD_FD) {
        if (!argp)
            return -PFS_EINVAL;
        return tpu_cache_fd_op(*(int32_t *)argp, 1);
    }
    if (req == CVITPU_LEGACY_DMABUF_FLUSH)
        return tpu_cache_range_op((const struct cvitpu_legacy_cache_op_arg *)argp, 0);
    if (req == CVITPU_LEGACY_DMABUF_INVLD)
        return tpu_cache_range_op((const struct cvitpu_legacy_cache_op_arg *)argp, 1);
    if (req == CVITPU_PIO_MODE)
        return 0;
    return -PFS_ENOSYS;
}

int npu_dev_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid;
    (void)req;
    (void)argp;
    KLOG_WARN("[pseudofs] /dev/npu ioctl: not yet implemented\n");
    return -PFS_ENOSYS;
}

int video0_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    (void)off;
    (void)buf;
    (void)len;
    return -PFS_ENOSYS;
}

int video0_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid;
    (void)req;
    (void)argp;
    return -PFS_ENOSYS;
}
