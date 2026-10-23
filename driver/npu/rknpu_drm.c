/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/npu/rknpu_drm.c — RK3588 NPU 的 DRM 渲染节点(兼容 shim)

   注意： 这不是 DRM。我们只实现 librknnrt 用到的那一小撮 ioctl 与 GEM 语义,
      让官方运行时能连上来。没有 CRTC/connector/framebuffer, 也不打算有。

   ── 为什么 handle 要按"打开的文件"隔离 ─────────────────────────────────
   DRM 里 GEM handle 是 per-drm_file 的(`drm_file` 就是一次 open)。用一张
   全局表的话, 两个进程各自 open 渲染节点, 各自从 1 开始发 handle, 就会串号
   —— 而且症状是"另一个进程的缓冲区被当成自己的", 极难查。

   所以每文件一份上下文。识别用的键是 `vfs_file_t*`(pseudofs 那对
    _file_fn 回调把它传下来, 见 pseudofs_internal.h)。

   ── 缓冲区本体来自 dma-buf ──────────────────────────────────────────────
   官方运行时的主路径是 dma_heap 分配 → dmabuf fd → PRIME_FD_TO_HANDLE。
   我们只管把 fd 换成 handle, 再换回来, 内存本身归 dma_buf 层。
*/
#include "rknpu_drm.h"

#include "rknpu.h" /* rknpu_submit_task(作业引擎在 rknpu.c) */
#include "dma_buf.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"
#include "syscall/syscall_internal.h" /* copy_from/to_user_bytes */
#include "task/task.h"
#include "types.h"

#define RKNPU_DRM_MAX_FILES 16
#define RKNPU_GEM_MAX       64

/*
   MEM_MAP 返回给用户态, 再由 mmap() 传回来的假偏移。

   注意： 必须按页粒度：mmap 的 ABI 要求 offset 页对齐, musl 的
   `mmap()` 包装在进内核之前就会 `if (off & (PAGE_SIZE-1)) { errno = EINVAL; }`
   直接返回 —— 实测踩过：写成 `BASE + id` 时 id=2 得到一个
   0x10000000002 的偏移, 用户态连内核都没进就 EINVAL 了。
   DRM 真实的假偏移(drm_vma_offset_manager)同样按页走。
*/
#define RKNPU_MMAP_BASE  (1ULL << 40) /* 本身页对齐 */
#define RKNPU_MMAP_SHIFT 12           /* 一页一个 id */

typedef struct {
    bool used;
    uint64_t id;      /* handle 与 obj_addr 共用；全局唯一 */
    uint32_t dma_idx; /* dma_buf 表下标(本对象持有一份引用) */
    size_t size;      /* 用户请求大小 */
} rknpu_gem_t;

typedef struct {
    bool used;
    void *file; /* vfs_file_t*, 只作身份标识 */
    rknpu_gem_t gems[RKNPU_GEM_MAX];
    uint32_t n_gems;
} rknpu_drm_file_t;

static rknpu_drm_file_t g_drm_files[RKNPU_DRM_MAX_FILES];
static spinlock_t g_drm_lock = SPINLOCK_INIT;
static uint64_t g_next_id = 1; /* 0 留作"无效" */

/* ── 每文件上下文 ──────────────────────────────────────────────────────── */

static rknpu_drm_file_t *drm_file_find(void *file)
{
    for (uint32_t i = 0; i < RKNPU_DRM_MAX_FILES; i++) {
        if (g_drm_files[i].used && g_drm_files[i].file == file)
            return &g_drm_files[i];
    }
    return NULL;
}

/* 懒分配：pseudofs 没有 open 钩子, 所以第一次用到时建。 */
static rknpu_drm_file_t *drm_file_get_or_create(void *file)
{
    rknpu_drm_file_t *ctx = drm_file_find(file);
    if (ctx)
        return ctx;

    for (uint32_t i = 0; i < RKNPU_DRM_MAX_FILES; i++) {
        if (!g_drm_files[i].used) {
            memset(&g_drm_files[i], 0, sizeof(g_drm_files[i]));
            g_drm_files[i].used = true;
            g_drm_files[i].file = file;
            return &g_drm_files[i];
        }
    }
    KLOG_ERROR("[rknpu] too many open DRM files (max %d)\n", RKNPU_DRM_MAX_FILES);
    return NULL;
}

int rknpu_drm_close_file(void *file)
{
    spin_lock(&g_drm_lock);
    rknpu_drm_file_t *ctx = drm_file_find(file);
    if (ctx) {
        /* 进程退出时没显式 DESTROY 的 handle 在这里回收 —— 否则每开一次
           就漏一批缓冲区(fd 池只有 128 槽, 很快耗尽)。
        */
        for (uint32_t i = 0; i < RKNPU_GEM_MAX; i++) {
            if (ctx->gems[i].used) {
                ctx->gems[i].used = false;
                spin_unlock(&g_drm_lock);
                dma_buf_put_index(ctx->gems[i].dma_idx);
                spin_lock(&g_drm_lock);
            }
        }
        memset(ctx, 0, sizeof(*ctx));
    }
    spin_unlock(&g_drm_lock);
    return 0;
}

/* ── GEM 对象 ──────────────────────────────────────────────────────────── */

static rknpu_gem_t *gem_find(rknpu_drm_file_t *ctx, uint64_t handle)
{
    for (uint32_t i = 0; i < RKNPU_GEM_MAX; i++) {
        if (ctx->gems[i].used && ctx->gems[i].id == handle)
            return &ctx->gems[i];
    }
    return NULL;
}

/* 新建一个引用 dma_idx 的对象。失败返回 NULL。 */
static rknpu_gem_t *gem_create(rknpu_drm_file_t *ctx, uint32_t dma_idx, size_t size)
{
    for (uint32_t i = 0; i < RKNPU_GEM_MAX; i++) {
        if (!ctx->gems[i].used) {
            ctx->gems[i].used = true;
            ctx->gems[i].id = g_next_id++;
            ctx->gems[i].dma_idx = dma_idx;
            ctx->gems[i].size = size;
            ctx->n_gems++;
            dma_buf_get_index(dma_idx); /* 对象自己持一份引用 */
            return &ctx->gems[i];
        }
    }
    KLOG_ERROR("[rknpu] per-file GEM table full (max %d)\n", RKNPU_GEM_MAX);
    return NULL;
}

/* ── DRM 核心 ioctl ────────────────────────────────────────────────────── */

static int drm_ioctl_version(void *argp)
{
    struct drm_version v;
    if (!argp || copy_from_user_bytes(argp, &v, sizeof(v)) < 0)
        return -22;

    /*
       注意： 驱动名必须是 "rknpu", 不是我们自己的什么名字 —— librknnrt
          拿它(和 /proc/dri/0/name)做身份匹配, 名字不对就当"不是 RKNPU
          设备"。
    */
    static const char drv_name[] = "rknpu";
    static const char drv_date[] = "20260101";
    static const char drv_desc[] = "Avatar OS RK3588 NPU (DRM-compat shim)";

    size_t name_full = sizeof(drv_name) - 1;
    size_t date_full = sizeof(drv_date) - 1;
    size_t desc_full = sizeof(drv_desc) - 1;

    if (v.name && v.name_len > 0) {
        size_t n = name_full < v.name_len ? name_full : v.name_len;
        if (copy_to_user_bytes(drv_name, v.name, n) < 0)
            return -22;
    }
    if (v.date && v.date_len > 0) {
        size_t n = date_full < v.date_len ? date_full : v.date_len;
        if (copy_to_user_bytes(drv_date, v.date, n) < 0)
            return -22;
    }
    if (v.desc && v.desc_len > 0) {
        size_t n = desc_full < v.desc_len ? desc_full : v.desc_len;
        if (copy_to_user_bytes(drv_desc, v.desc, n) < 0)
            return -22;
    }

    /*
       长度一律回串的真实长度, 不是这次实际写入的长度。

         这是 libdrm 的"两阶段"协议：drmGetVersion() 第一次传
         name=NULL / name_len=0 只为问长度, 拿到之后再 malloc 缓冲,
         第二次调用取串。要是这里回 0("没写进去所以长度是 0"), libdrm
         就不分配, 调用方拿到 name=NULL 还去 strstr —— 空指针崩。

         实测：改之前 librknnrt 连打两次 DRM_IOCTL_VERSION 都成功, 紧接着
         就在自己代码里 SIGSEGV(va=0x10), 而日志只显示 ioctl 全部返回 0。
    */
    v.version_major = 1;
    v.version_minor = 0;
    v.version_patchlevel = 0;
    v.name_len = name_full;
    v.date_len = date_full;
    v.desc_len = desc_full;
    if (copy_to_user_bytes(&v, argp, sizeof(v)) < 0)
        return -22;

    KLOG_INFO("[rknpu] DRM_IOCTL_VERSION -> %s %d.%d.%d\n", drv_name, 1, 0, 0);
    return 0;
}

/*
   DRM_IOCTL_SET_VERSION —— libdrm 的 drmSetVersion()：用户态声明它按哪个
   DRM 接口版本说话, 驱动回自己的版本。

   内核真实实现(drm_setversion)会在 drm_di_major 不是 DRM_IF_MAJOR 时回
   -EINVAL。我们故意不做这个校验：这是兼容层, 不是 DRM, 拒绝一个我们
   根本不在意的字段只会给"还没跑通主流程"的阶段多造一个失败点。

   只填 dd_*(驱动版本), di_* 原样回显。与 DRM_IOCTL_VERSION 报的
   1.0.0 保持一致 —— 两处对不上会让 libdrm 认为驱动在说谎。
*/
static int drm_ioctl_set_version(void *argp)
{
    struct drm_set_version sv;
    if (!argp || copy_from_user_bytes(argp, &sv, sizeof(sv)) < 0)
        return -22;

    sv.drm_dd_major = 1; /* 与 drm_ioctl_version 的 1.0.0 对齐 */
    sv.drm_dd_minor = 0;

    if (copy_to_user_bytes(&sv, argp, sizeof(sv)) < 0)
        return -22;

    KLOG_DEBUG("[rknpu] DRM_IOCTL_SET_VERSION di=%d.%d -> dd=1.0\n", sv.drm_di_major, sv.drm_di_minor);
    return 0;
}

/*
   DRM_IOCTL_GET_UNIQUE —— 与 VERSION 同一套"两阶段"协议：第一次传
   unique=NULL/unique_len=0 只为问长度, 第二次才带缓冲取串。
   librknnrt 实测会调它(0xc0106401)。
*/
static int drm_ioctl_get_unique(void *argp)
{
    struct drm_unique u;
    if (!argp || copy_from_user_bytes(argp, &u, sizeof(u)) < 0)
        return -22;

    static const char busid[] = "platform:rknpu";
    size_t full = sizeof(busid) - 1;

    if (u.unique && u.unique_len > 0) {
        size_t n = full < u.unique_len ? full : u.unique_len;
        if (copy_to_user_bytes(busid, u.unique, n) < 0)
            return -22;
    }
    u.unique_len = full; /* 同 VERSION：回真实长度, 不是写入长度 */
    if (copy_to_user_bytes(&u, argp, sizeof(u)) < 0)
        return -22;
    return 0;
}

static int drm_ioctl_get_magic(void *argp)
{
    uint32_t magic = 0x52414B41U; /* "AKAR" —— 只是回一个稳定值 */
    if (!argp || copy_to_user_bytes(&magic, argp, sizeof(magic)) < 0)
        return -22;
    return 0;
}

/* ── GEM / PRIME ───────────────────────────────────────────────────────── */

static int drm_ioctl_gem_flink(void *file, void *argp)
{
    /*
       GEM_FLINK 给对象起一个全局名, 用于跨进程共享。
       我们的缓冲区已经能通过 dmabuf fd 共享(PRIME), flink 是更老的机制。
       librknnrt 会在某些版本里探测它 —— 明确回 -EINVAL 表示"本驱动不支持
       全局名", 比返回一个假名字安全(假名字会让对端拿到错误的缓冲区)。
    */
    (void)file;
    (void)argp;
    KLOG_WARN_ONCE("[rknpu] GEM_FLINK not supported (use PRIME/dmabuf)\n");
    return -22;
}

static int drm_ioctl_prime_handle_to_fd(void *file, void *argp)
{
    struct drm_prime_handle p;
    if (!argp || copy_from_user_bytes(argp, &p, sizeof(p)) < 0)
        return -22;

    /* 只在锁里查表；建 fd 是别的子系统的活, 别抱着自旋锁做
       (那会在关中断的情况下走 fd 池的锁)。
    */
    spin_lock(&g_drm_lock);
    rknpu_drm_file_t *ctx = drm_file_find(file);
    rknpu_gem_t *gem = ctx ? gem_find(ctx, p.handle) : NULL;
    uint32_t dma_idx = gem ? gem->dma_idx : 0;
    spin_unlock(&g_drm_lock);

    dma_buf_t *buf = gem ? dma_buf_from_index(dma_idx) : NULL;
    int fd = buf ? dma_buf_alloc_fd_current(buf) : -1;

    if (!gem || fd < 0) {
        KLOG_WARN("[rknpu] PRIME_HANDLE_TO_FD: bad handle=%u\n", p.handle);
        return -22;
    }

    p.fd = fd;
    if (copy_to_user_bytes(&p, argp, sizeof(p)) < 0) {
        KLOG_ERROR("[rknpu] PRIME_HANDLE_TO_FD: failed to write back to user space (fd=%d leaked)\n", fd);
        return -22;
    }
    KLOG_DEBUG("[rknpu] PRIME handle %u -> fd %d\n", p.handle, fd);
    return 0;
}

static int drm_ioctl_prime_fd_to_handle(void *file, void *argp)
{
    struct drm_prime_handle p;
    if (!argp || copy_from_user_bytes(argp, &p, sizeof(p)) < 0)
        return -22;

    dma_buf_t *buf = dma_buf_from_user_fd(p.fd);
    if (!buf) {
        KLOG_WARN("[rknpu] PRIME_FD_TO_HANDLE: fd %d is not a dmabuf\n", p.fd);
        return -22;
    }

    spin_lock(&g_drm_lock);
    rknpu_drm_file_t *ctx = drm_file_get_or_create(file);
    rknpu_gem_t *gem = ctx ? gem_create(ctx, dma_buf_index(buf), dma_buf_size(buf)) : NULL;
    uint32_t handle = gem ? (uint32_t)gem->id : 0;
    spin_unlock(&g_drm_lock);

    if (!gem) {
        KLOG_ERROR("[rknpu] PRIME_FD_TO_HANDLE: failed to create GEM object\n");
        return -12; /* ENOMEM */
    }

    p.handle = handle;
    if (copy_to_user_bytes(&p, argp, sizeof(p)) < 0)
        return -22;
    KLOG_DEBUG("[rknpu] PRIME fd %d -> handle %u\n", p.fd, handle);
    return 0;
}

/* ── RKNPU ioctl ───────────────────────────────────────────────────────── */

static int rknpu_ioctl_action(void *argp)
{
    struct rknpu_action a;
    if (!argp || copy_from_user_bytes(argp, &a, sizeof(a)) < 0)
        return -22;

    int rc = 0;
    switch (a.flags) {
    case RKNPU_GET_IOMMU_EN:
        /*
           必须回 0。librknnrt 先问这个再决定内存策略(有 IOMMU 才可能
             用非连续缓冲)。我们没实现 SMMU, 谎报 1 会让它按 IOVA 走,
             之后每次提交都是错的地址。
        */
        a.value = 0;
        break;
    case RKNPU_GET_HW_VERSION:
        a.value = 0x46495245U; /* "FIRE", 与版本寄存器一致 */
        break;
    case RKNPU_GET_DRV_VERSION:
        a.value = 1000000U; /* 1.00.00.00, vendor 的编码方式 */
        break;
    case RKNPU_GET_TOTAL_SRAM_SIZE:
    case RKNPU_GET_FREE_SRAM_SIZE:
        a.value = 0; /* 不支持片上 SRAM */
        break;
    case RKNPU_POWER_ON:
    case RKNPU_POWER_OFF:
        /* 电源域在 rknpu_init() 里已经常开, 这里当作幂等成功 */
        a.value = 0;
        break;
    default:
        /* 认不出来的 action：不要回 -EINVAL。很多是查询类
           (频率/带宽/带宽优先级), librknnrt 拿到 0 会走保守路径；
           回错误可能让它在初始化阶段直接失败。打一条日志, 等上板看
           日志补齐语义。
        */
        KLOG_WARN_SAMPLE("[rknpu] ACTION flags=%u not implemented, returning 0\n", a.flags, _klog_seq_);
        a.value = 0;
        break;
    }

    if (copy_to_user_bytes(&a, argp, sizeof(a)) < 0)
        return -22;
    (void)rc;
    return 0;
}

static int rknpu_ioctl_mem_create(void *file, void *argp)
{
    struct rknpu_mem_create m;
    if (!argp || copy_from_user_bytes(argp, &m, sizeof(m)) < 0)
        return -22;
    if (m.size == 0)
        return -22;

    /*
       没有 IOMMU 时非连续缓冲直接拒绝 —— 与 vendor 驱动
       (rknpu_gem.c:707) 的语义一致。我们的堆本来就只给物理连续内存,
       但必须在这里明确拒绝：静默降级成连续会让 librknnrt 以为拿到了
       非连续缓冲, 之后按 sg 列表算地址就全错了。
    */
    if (m.flags & RKNPU_MEM_NON_CONTIGUOUS) {
        KLOG_WARN_ONCE("[rknpu] MEM_CREATE requested non-contiguous memory, but this driver has no IOMMU - rejected\n");
        return -22;
    }

    dma_buf_t *buf = dma_buf_alloc((size_t)m.size);
    if (!buf)
        return -12; /* ENOMEM */

    size_t size = dma_buf_size(buf);
    uint64_t pa = dma_buf_dev_addr(buf); /* 无 IOMMU 时即物理地址 */

    spin_lock(&g_drm_lock);
    rknpu_drm_file_t *ctx = drm_file_get_or_create(file);
    rknpu_gem_t *gem = ctx ? gem_create(ctx, dma_buf_index(buf), size) : NULL;
    uint64_t id = gem ? gem->id : 0;
    spin_unlock(&g_drm_lock);

    dma_buf_put(buf); /* 创建那份引用交给 gem 了 */

    if (!gem)
        return -12;

    m.handle = (uint32_t)id;
    m.obj_addr = id; /* 用户态会原样回传给 SYNC/DESTROY */
    m.dma_addr = pa; /* 设备可见地址：唯一出口 dma_buf_dev_addr() */
    m.sram_size = 0;
    if (copy_to_user_bytes(&m, argp, sizeof(m)) < 0)
        return -22;

    KLOG_DEBUG("[rknpu] MEM_CREATE size=%llu -> handle=%u pa=0x%llx\n", (unsigned long long)size, (uint32_t)id,
               (unsigned long long)pa);
    return 0;
}

static int rknpu_ioctl_mem_map(void *file, void *argp)
{
    struct rknpu_mem_map m;
    if (!argp || copy_from_user_bytes(argp, &m, sizeof(m)) < 0)
        return -22;

    spin_lock(&g_drm_lock);
    rknpu_drm_file_t *ctx = drm_file_find(file);
    rknpu_gem_t *gem = ctx ? gem_find(ctx, m.handle) : NULL;
    uint64_t id = gem ? gem->id : 0;
    spin_unlock(&g_drm_lock);

    if (!gem) {
        KLOG_WARN("[rknpu] MEM_MAP: bad handle=%u\n", m.handle);
        return -22;
    }

    /*
       DRM 的约定：这里回一个不透明偏移, 用户态拿它去
       mmap(fd, ..., offset)。sys_mmap 再经 rknpu_drm_mmap_lookup() 解回来。
       页粒度是硬要求, 见 RKNPU_MMAP_BASE 的说明。
    */
    m.offset = RKNPU_MMAP_BASE + (id << RKNPU_MMAP_SHIFT);
    if (copy_to_user_bytes(&m, argp, sizeof(m)) < 0)
        return -22;
    return 0;
}

static int rknpu_ioctl_mem_destroy(void *file, void *argp)
{
    struct rknpu_mem_destroy m;
    if (!argp || copy_from_user_bytes(argp, &m, sizeof(m)) < 0)
        return -22;

    spin_lock(&g_drm_lock);
    rknpu_drm_file_t *ctx = drm_file_find(file);
    rknpu_gem_t *gem = ctx ? gem_find(ctx, m.handle ? m.handle : m.obj_addr) : NULL;
    uint32_t dma_idx = 0;
    if (gem) {
        dma_idx = gem->dma_idx;
        gem->used = false;
        if (ctx->n_gems > 0)
            ctx->n_gems--;
    }
    spin_unlock(&g_drm_lock);

    if (!gem) {
        KLOG_WARN("[rknpu] MEM_DESTROY: bad handle=%u\n", m.handle);
        return -22;
    }
    dma_buf_put_index(dma_idx); /* 还掉 gem 那份引用 */
    return 0;
}

static int rknpu_ioctl_mem_sync(void *file, void *argp)
{
    struct rknpu_mem_sync m;
    if (!argp || copy_from_user_bytes(argp, &m, sizeof(m)) < 0)
        return -22;

    /* 同样：查表在锁里, 缓存维护在锁外 —— 一个几十 MB 的缓冲区
       clean 一遍不能抱着自旋锁做(那是全程关中断)。
    */
    spin_lock(&g_drm_lock);
    rknpu_drm_file_t *ctx = drm_file_find(file);
    rknpu_gem_t *gem = ctx ? gem_find(ctx, m.obj_addr) : NULL;
    uint32_t dma_idx = gem ? gem->dma_idx : 0;
    spin_unlock(&g_drm_lock);

    dma_buf_t *buf = gem ? dma_buf_from_index(dma_idx) : NULL;
    if (!buf) {
        KLOG_WARN("[rknpu] MEM_SYNC: bad obj_addr=0x%llx\n", (unsigned long long)m.obj_addr);
        return -22;
    }

    uint64_t len = m.size ? m.size : dma_buf_size(buf);
    if (m.flags & RKNPU_MEM_SYNC_TO_DEVICE)
        dma_buf_sync_region(buf, m.offset, len, true);
    if (m.flags & RKNPU_MEM_SYNC_FROM_DEVICE)
        dma_buf_sync_region(buf, m.offset, len, false);
    return 0;
}

static int rknpu_ioctl_submit(void *file, void *argp)
{
    struct rknpu_submit s;
    if (!argp || copy_from_user_bytes(argp, &s, sizeof(s)) < 0)
        return -22;

    /*
       task_obj_addr 是用户态从 MEM_CREATE / PRIME 拿到的 obj_addr
       (我们回的是 GEM 对象 id)。vendor 驱动这里存的也是这种不透明令牌
       —— 不是设备地址, 别搞混。
    */
    spin_lock(&g_drm_lock);
    rknpu_drm_file_t *ctx = drm_file_find(file);
    rknpu_gem_t *gem = ctx ? gem_find(ctx, s.task_obj_addr) : NULL;
    uint32_t dma_idx = gem ? gem->dma_idx : 0;
    spin_unlock(&g_drm_lock);

    dma_buf_t *buf = gem ? dma_buf_from_index(dma_idx) : NULL;
    if (!buf) {
        KLOG_WARN("[rknpu] SUBMIT: bad task_obj_addr=0x%llx\n", (unsigned long long)s.task_obj_addr);
        return -22;
    }

    void *kva = dma_buf_kva(buf);
    size_t bytes = dma_buf_size(buf);

    /*
       任务描述符是设备直接读的：提交前必须 clean 一遍, 否则 NPU 可能
       读到 Cache 里的旧内容。用户态一般会自己先 MEM_SYNC, 但那不是我们能
       依赖的前提 —— 描述符读错的症状是设备行为完全跑偏, 而日志一声不吭。
    */
    dma_buf_sync_region(buf, 0, bytes, true);

    int rc = rknpu_submit_task(kva, bytes, &s);

    s.task_counter = (rc == 0) ? s.task_number : 0;
    if (copy_to_user_bytes(&s, argp, sizeof(s)) < 0)
        return -22;

    KLOG_DEBUG("[rknpu] SUBMIT tasks=%u..%u core_mask=0x%x -> %d\n", s.task_start, s.task_start + s.task_number - 1,
               s.core_mask, rc);
    return rc;
}

/* ── mmap 假偏移解析 ───────────────────────────────────────────────────── */

int rknpu_drm_mmap_lookup(void *file, uint64_t offset, uint64_t *paddr_out, uint64_t *size_out)
{
    if (!paddr_out || !size_out || offset < RKNPU_MMAP_BASE)
        return -1;
    if (offset & ((1ULL << RKNPU_MMAP_SHIFT) - 1))
        return -1; /* 非页对齐的不是我们的假偏移 */

    uint64_t id = (offset - RKNPU_MMAP_BASE) >> RKNPU_MMAP_SHIFT;

    spin_lock(&g_drm_lock);
    rknpu_drm_file_t *ctx = drm_file_find(file);
    rknpu_gem_t *gem = ctx ? gem_find(ctx, id) : NULL;
    uint32_t dma_idx = gem ? gem->dma_idx : 0;
    dma_buf_t *buf = gem ? dma_buf_from_index(dma_idx) : NULL;
    uint64_t pa = buf ? dma_buf_dev_addr(buf) : 0;
    uint64_t sz = buf ? (uint64_t)dma_buf_size(buf) : 0;
    spin_unlock(&g_drm_lock);

    if (!buf)
        return -1;
    *paddr_out = pa;
    *size_out = sz;
    return 0;
}

/* ── ioctl 总入口 ──────────────────────────────────────────────────────── */

int rknpu_drm_ioctl_file(void *file, uint64_t req, void *argp)
{
    switch (req) {
    case DRM_IOCTL_VERSION:
        return drm_ioctl_version(argp);
    case DRM_IOCTL_SET_VERSION:
        return drm_ioctl_set_version(argp);
    case DRM_IOCTL_GET_UNIQUE:
        return drm_ioctl_get_unique(argp);
    case DRM_IOCTL_GET_MAGIC:
        return drm_ioctl_get_magic(argp);

    /*
       master 与 AGP 都是老 DRM 惯例, 现代渲染节点不需要。
       但 librknnrt 会发它们做探测(实测 0x641e/0x641f 带 EINTR/EAGAIN
       重试循环, 0x6430/0x6431 是 AGP)—— 认不出来就回 -EINVAL 是最省事的,
       但如果它把失败当成"设备不可用", 就会在初始化阶段直接放弃。

       渲染节点本来就不需要 master, 回 0 是语义正确的(DRM 里 render
       node 上 setmaster 直接成功)。AGP 我们确实没有, 回 -EINVAL。
    */
    case DRM_IOCTL_SET_MASTER:
    case DRM_IOCTL_DROP_MASTER:
        return 0;
    case DRM_IOCTL_AGP_ACQUIRE:
    case DRM_IOCTL_AGP_RELEASE:
        return -22;

    case DRM_IOCTL_GEM_FLINK:
        return drm_ioctl_gem_flink(file, argp);
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:
        return drm_ioctl_prime_handle_to_fd(file, argp);
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:
        return drm_ioctl_prime_fd_to_handle(file, argp);

    case DRM_IOCTL_RKNPU_ACTION:
        return rknpu_ioctl_action(argp);
    case DRM_IOCTL_RKNPU_SUBMIT:
        return rknpu_ioctl_submit(file, argp);
    case DRM_IOCTL_RKNPU_MEM_CREATE:
        return rknpu_ioctl_mem_create(file, argp);
    case DRM_IOCTL_RKNPU_MEM_MAP:
        return rknpu_ioctl_mem_map(file, argp);
    case DRM_IOCTL_RKNPU_MEM_DESTROY:
        return rknpu_ioctl_mem_destroy(file, argp);
    case DRM_IOCTL_RKNPU_MEM_SYNC:
        return rknpu_ioctl_mem_sync(file, argp);

    default:
        /*
           注意： 这张表是静态分析出来的下界(104 个调用点里只还原了 61 个),
           上板后总会有没见过的号打到这里。所以：打日志 + 回 -EINVAL,
           而不是静默。日志就是补全 ABI 的唯一手段 —— 见
           docs/npu/RK3588_NPU_DRIVER.md §8 验证策略。
        */
        KLOG_WARN("[rknpu] unimplemented DRM ioctl req=0x%08llx (_IOC: type='%c' nr=0x%llx size=%llu)\n",
                  (unsigned long long)req, (char)((req >> 8) & 0xFF), (unsigned long long)(req & 0xFF),
                  (unsigned long long)((req >> 16) & 0x3FFF));
        return -22;
    }
}
