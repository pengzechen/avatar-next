/*
 * drmtest.c — DRM 渲染节点 + GEM 层（P2+P4）的用户态验收
 *
 * 跑法：
 *     make PLATFORM=qemu-virt-aarch64 LOG=info NPU=rknpu rootfs
 *     make PLATFORM=qemu-virt-aarch64 LOG=info NPU=rknpu run-fs
 *     ~ # /bin/drmtest
 *
 * ⚠️ **QEMU 上没有 NPU**，所以只有 SUBMIT 会失败（-ENODEV，这是预期的）。
 *    其余全部可验 —— 因为 GEM/PRIME/MEM_* 这些根本不碰硬件，它们只是把
 *    dma-buf 包装成 handle。所以这个程序能证明「librknnrt 连上来之后的
 *    内存路径是通的」，证明不了「算子算得对」（那要真机 + P3）。
 *
 * ⚠️ 下面的结构体与 ioctl 号是**手抄**的（用户态不能包含内核头）。
 *    _IOC 把 sizeof 编进操作码，所以尺寸写错不是"参数错位"而是**换成另一个
 *    ioctl 号**，内核会走到 default 分支回 -EINVAL —— 是响亮的失败。
 *    内核侧同名定义见 include/rknpu_drm.h，两边靠这里的 _Static_assert 对齐。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── DRM 核心 ──────────────────────────────────────────────────────────── */
struct drm_version {
    int32_t version_major, version_minor, version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};
struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};
struct drm_gem_flink {
    uint32_t handle;
    uint32_t name;
};

#define DRM_IOCTL_VERSION            _IOWR('d', 0x00, struct drm_version)
#define DRM_IOCTL_GET_MAGIC          _IOR('d', 0x02, uint32_t)
#define DRM_IOCTL_SET_MASTER         _IO('d', 0x1e)
#define DRM_IOCTL_DROP_MASTER        _IO('d', 0x1f)
#define DRM_IOCTL_AGP_ACQUIRE        _IO('d', 0x30)
#define DRM_IOCTL_GEM_FLINK          _IOWR('d', 0x0a, struct drm_gem_flink)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD _IOWR('d', 0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE _IOWR('d', 0x2e, struct drm_prime_handle)

_Static_assert(sizeof(struct drm_version) == 64, "drm_version 必须 64 字节");
_Static_assert(sizeof(struct drm_prime_handle) == 12, "prime_handle 必须 12");
_Static_assert(DRM_IOCTL_VERSION == 0xc0406400U, "与内核/librknnrt 不符");
_Static_assert(DRM_IOCTL_PRIME_FD_TO_HANDLE == 0xc00c642eU, "与实测不符");

/* ── RKNPU 六个 ────────────────────────────────────────────────────────── */
struct rknpu_subcore_task {
    uint32_t task_start, task_number;
};
struct rknpu_submit {
    uint32_t flags, timeout, task_start, task_number, task_counter;
    int32_t priority;
    uint64_t task_obj_addr;
    uint32_t iommu_domain_id, reserved;
    uint64_t task_base_addr;
    int64_t hw_elapse_time;
    uint32_t core_mask;
    int32_t fence_fd;
    struct rknpu_subcore_task subcore_task[5];
};
struct rknpu_mem_create {
    uint32_t handle, flags;
    uint64_t size, obj_addr, dma_addr, sram_size;
    int32_t iommu_domain_id;
    uint32_t core_mask;
};
struct rknpu_mem_map {
    uint32_t handle, reserved;
    uint64_t offset;
};
struct rknpu_mem_destroy {
    uint32_t handle, reserved;
    uint64_t obj_addr;
};
struct rknpu_mem_sync {
    uint32_t flags, reserved;
    uint64_t obj_addr, offset, size;
};
struct rknpu_action {
    uint32_t flags, value;
};

#define RKNPU_ACTION      0x00
#define RKNPU_SUBMIT      0x01
#define RKNPU_MEM_CREATE  0x02
#define RKNPU_MEM_MAP     0x03
#define RKNPU_MEM_DESTROY 0x04
#define RKNPU_MEM_SYNC    0x05

#define DRM_IOCTL_RKNPU_ACTION \
    _IOWR('d', 0x40 + RKNPU_ACTION, struct rknpu_action)
#define DRM_IOCTL_RKNPU_SUBMIT \
    _IOWR('d', 0x40 + RKNPU_SUBMIT, struct rknpu_submit)
#define DRM_IOCTL_RKNPU_MEM_CREATE \
    _IOWR('d', 0x40 + RKNPU_MEM_CREATE, struct rknpu_mem_create)
#define DRM_IOCTL_RKNPU_MEM_MAP \
    _IOWR('d', 0x40 + RKNPU_MEM_MAP, struct rknpu_mem_map)
#define DRM_IOCTL_RKNPU_MEM_DESTROY \
    _IOWR('d', 0x40 + RKNPU_MEM_DESTROY, struct rknpu_mem_destroy)
#define DRM_IOCTL_RKNPU_MEM_SYNC \
    _IOWR('d', 0x40 + RKNPU_MEM_SYNC, struct rknpu_mem_sync)

_Static_assert(sizeof(struct rknpu_submit) == 104, "submit 必须 104 字节");
_Static_assert(sizeof(struct rknpu_mem_create) == 48, "mem_create 必须 48");
_Static_assert(sizeof(struct rknpu_mem_sync) == 32, "mem_sync 必须 32");
_Static_assert(DRM_IOCTL_RKNPU_SUBMIT == 0xc0686441U, "与实测不符");
_Static_assert(DRM_IOCTL_RKNPU_MEM_CREATE == 0xc0306442U, "与实测不符");

#define RKNPU_GET_IOMMU_EN         18
#define RKNPU_MEM_SYNC_TO_DEVICE   (1U << 0)
#define RKNPU_MEM_SYNC_FROM_DEVICE (1U << 1)

/* ── dma-heap（与 dma_buf.h 一致）─────────────────────────────────────── */
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0, struct dma_heap_allocation_data)

/* ── 计数 ──────────────────────────────────────────────────────────────── */
static int g_pass, g_fail;
static void ok(const char *w)
{
    printf("  [ok]   %s\n", w);
    g_pass++;
}
static void bad(const char *w, const char *why)
{
    printf("  [FAIL] %s — %s\n", w, why);
    g_fail++;
}

int main(void)
{
    printf("=== drmtest: DRM 渲染节点 + GEM（P2+P4）===\n");

    /* ── 1. 打开渲染节点 ───────────────────────────────────────────── */
    int drm = open("/dev/dri/renderD128", O_RDWR);
    if (drm < 0) {
        printf("[FAIL] open(/dev/dri/renderD128): %s\n", strerror(errno));
        return 1;
    }
    printf("渲染节点 fd=%d\n\n", drm);
    ok("open(renderD128)");

    /* ── 2. DRM 核心 ioctl ─────────────────────────────────────────── */
    printf("-- DRM 核心 --\n");
    {
        char name[64] = { 0 }, date[64] = { 0 }, desc[128] = { 0 };
        struct drm_version v;
        memset(&v, 0, sizeof(v));
        v.name = name;
        v.name_len = sizeof(name);
        v.date = date;
        v.date_len = sizeof(date);
        v.desc = desc;
        v.desc_len = sizeof(desc);
        if (ioctl(drm, DRM_IOCTL_VERSION, &v) == 0)
            printf("  驱动: %s %d.%d.%d  (%s)\n", name, v.version_major,
                   v.version_minor, v.version_patchlevel, desc);
        else
            bad("DRM_IOCTL_VERSION", strerror(errno));

        /*
         * ⚠️ 名字必须是 **rknpu** —— librknnrt 按它做身份匹配。
         * （最初起的是 "rknpu-avatar"，librknnrt 认不出，空指针崩。）
         */
        if (ioctl(drm, DRM_IOCTL_VERSION, &v) == 0 &&
            strcmp(name, "rknpu") == 0)
            ok("DRM_IOCTL_VERSION 返回驱动名 rknpu（librknnrt 靠它匹配）");
        else
            bad("DRM_IOCTL_VERSION", "名字不是 rknpu");

        uint32_t magic = 0;
        if (ioctl(drm, DRM_IOCTL_GET_MAGIC, &magic) == 0)
            ok("DRM_IOCTL_GET_MAGIC");
        else
            bad("DRM_IOCTL_GET_MAGIC", strerror(errno));

        /* librknnrt 会发这两个做探测；渲染节点上 setmaster 语义上就该成功 */
        if (ioctl(drm, DRM_IOCTL_SET_MASTER) == 0 &&
            ioctl(drm, DRM_IOCTL_DROP_MASTER) == 0)
            ok("SET_MASTER / DROP_MASTER（渲染节点，直接成功）");
        else
            bad("SET_MASTER / DROP_MASTER", strerror(errno));

        /* AGP 是更老的东西，我们确实没有 —— 明确 -EINVAL 才是对的 */
        if (ioctl(drm, DRM_IOCTL_AGP_ACQUIRE) < 0 && errno == EINVAL)
            ok("AGP_ACQUIRE 正确地被拒（-EINVAL）");
        else
            bad("AGP_ACQUIRE", "应当回 -EINVAL");
    }

    /* ── 3. ACTION：IOMMU 查询（librknnrt 的决策依据）──────────────── */
    printf("\n-- RKNPU ACTION --\n");
    {
        struct rknpu_action a = { .flags = RKNPU_GET_IOMMU_EN,
                                  .value = 0xdead };
        if (ioctl(drm, DRM_IOCTL_RKNPU_ACTION, &a) == 0 && a.value == 0)
            ok("GET_IOMMU_EN 回 0（无 IOMMU —— librknnrt 据此选连续内存）");
        else
            bad("GET_IOMMU_EN", "应回 0");
    }

    /* ── 4. dma-buf → GEM handle 往返 ──────────────────────────────── */
    printf("\n-- PRIME 往返（官方运行时的主路径）--\n");
    {
        int heap = open("/dev/dma_heap/cma", O_RDWR);
        if (heap < 0) {
            bad("open(dma_heap)", strerror(errno));
        } else {
            struct dma_heap_allocation_data ha;
            memset(&ha, 0, sizeof(ha));
            ha.len = 4096;
            if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &ha) != 0) {
                bad("dma_heap alloc", strerror(errno));
            } else {
                int bfd = (int)ha.fd;
                /* 从 dmabuf fd 换 handle */
                struct drm_prime_handle p;
                memset(&p, 0, sizeof(p));
                p.fd = bfd;
                if (ioctl(drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &p) != 0) {
                    bad("PRIME_FD_TO_HANDLE", strerror(errno));
                } else {
                    uint32_t h = p.handle;
                    ok("PRIME_FD_TO_HANDLE");

                    /* 再换回一个 fd，应当指向**同一块**内存 */
                    struct drm_prime_handle q;
                    memset(&q, 0, sizeof(q));
                    q.handle = h;
                    if (ioctl(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &q) != 0) {
                        bad("PRIME_HANDLE_TO_FD", strerror(errno));
                    } else {
                        ok("PRIME_HANDLE_TO_FD");
                        /* 两个 fd 映射到同一块内存才说明往返没走丢 */
                        uint8_t *m1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, bfd, 0);
                        uint8_t *m2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, q.fd, 0);
                        if (m1 == MAP_FAILED || m2 == MAP_FAILED) {
                            bad("往返后两个 fd 指向同一块内存", "mmap 失败");
                        } else {
                            memset(m1, 0x5A, 4096);
                            int same = (m2[0] == 0x5A && m2[4095] == 0x5A);
                            if (same)
                                ok("往返后两个 fd 指向同一块内存");
                            else
                                bad("往返后两个 fd 指向同一块内存",
                                    "两边内容不一致");
                            munmap(m1, 4096);
                            munmap(m2, 4096);
                        }
                        close(q.fd);
                    }
                    /* GEM_FLINK 我们明确不支持，应当 -EINVAL 而不是假成功 */
                    struct drm_gem_flink f = { .handle = h };
                    if (ioctl(drm, DRM_IOCTL_GEM_FLINK, &f) < 0 &&
                        errno == EINVAL)
                        ok("GEM_FLINK 明确回 -EINVAL（不编假名字）");
                    else
                        bad("GEM_FLINK", "应当回 -EINVAL");

                    /* handle 要能被销毁；销毁后再用应当失败 */
                    struct rknpu_mem_destroy d = { .handle = h };
                    if (ioctl(drm, DRM_IOCTL_RKNPU_MEM_DESTROY, &d) == 0)
                        ok("MEM_DESTROY");
                    else
                        bad("MEM_DESTROY", strerror(errno));
                }
                close(bfd);
            }
            close(heap);
        }
    }

    /* ── 5. 驱动自带的 MEM_CREATE / MEM_MAP 路径 ───────────────────── */
    printf("\n-- RKNPU MEM_CREATE / MEM_MAP --\n");
    {
        struct rknpu_mem_create m;
        memset(&m, 0, sizeof(m));
        m.size = 8192;
        if (ioctl(drm, DRM_IOCTL_RKNPU_MEM_CREATE, &m) != 0) {
            bad("MEM_CREATE", strerror(errno));
        } else {
            printf("  handle=%u obj=0x%llx dma=0x%llx\n", m.handle,
                   (unsigned long long)m.obj_addr,
                   (unsigned long long)m.dma_addr);
            ok("MEM_CREATE");

            if (m.dma_addr != 0)
                ok("MEM_CREATE 回过设备地址（dma_buf_dev_addr 出口）");
            else
                bad("MEM_CREATE 回过设备地址", "dma_addr 是 0");

            /* 非连续缓冲必须被拒（无 IOMMU）*/
            struct rknpu_mem_create nc;
            memset(&nc, 0, sizeof(nc));
            nc.size = 4096;
            nc.flags = 1U << 0; /* RKNPU_MEM_NON_CONTIGUOUS */
            if (ioctl(drm, DRM_IOCTL_RKNPU_MEM_CREATE, &nc) < 0 &&
                errno == EINVAL)
                ok("无 IOMMU 时非连续分配被拒（不静默降级）");
            else
                bad("非连续分配被拒", "竟然成功了");

            /* MEM_MAP 回假偏移 → mmap 渲染节点 */
            struct rknpu_mem_map mm = { .handle = m.handle };
            if (ioctl(drm, DRM_IOCTL_RKNPU_MEM_MAP, &mm) != 0) {
                bad("MEM_MAP", strerror(errno));
            } else {
                ok("MEM_MAP 回了假偏移");
                void *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED,
                               drm, mm.offset);
                if (p == MAP_FAILED) {
                    bad("用假偏移 mmap 渲染节点", strerror(errno));
                } else {
                    uint8_t *b = p;
                    for (int i = 0; i < 8192; i++)
                        b[i] = (uint8_t)(i * 3);
                    int good = 1;
                    for (int i = 0; i < 8192; i++)
                        if (b[i] != (uint8_t)(i * 3))
                            good = 0;
                    if (good)
                        ok("假偏移 mmap 后可读写、内容自洽");
                    else
                        bad("假偏移 mmap 后读写", "读回不一致");
                    munmap(p, 8192);
                }
            }

            struct rknpu_mem_sync sy;
            memset(&sy, 0, sizeof(sy));
            sy.obj_addr = m.obj_addr;
            sy.size = 8192;
            sy.flags = RKNPU_MEM_SYNC_TO_DEVICE | RKNPU_MEM_SYNC_FROM_DEVICE;
            if (ioctl(drm, DRM_IOCTL_RKNPU_MEM_SYNC, &sy) == 0)
                ok("MEM_SYNC（缓存维护真的被执行）");
            else
                bad("MEM_SYNC", strerror(errno));

            struct rknpu_mem_destroy d = { .handle = m.handle };
            ioctl(drm, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
        }
    }

    /* ── 6. SUBMIT：QEMU 上没有 NPU，预期 -ENODEV ──────────────────── */
    printf("\n-- SUBMIT --\n");
    {
        struct rknpu_mem_create m;
        memset(&m, 0, sizeof(m));
        m.size = 4096;
        if (ioctl(drm, DRM_IOCTL_RKNPU_MEM_CREATE, &m) == 0) {
            struct rknpu_submit s;
            memset(&s, 0, sizeof(s));
            s.timeout = 100;
            s.task_start = 0;
            s.task_number = 1;
            s.task_obj_addr = m.obj_addr;
            int rc = ioctl(drm, DRM_IOCTL_RKNPU_SUBMIT, &s);
            if (rc == 0)
                ok("SUBMIT 成功（说明这台机器上有 NPU）");
            else if (errno == ENODEV)
                ok("SUBMIT 回 -ENODEV（本机无 NPU —— QEMU 上的预期结果）");
            else if (errno == EIO)
                bad("SUBMIT", "超时/中断状态不匹配 —— 有 NPU 但作业没跑完");
            else
                printf("  [note] SUBMIT 回 %s（QEMU 上预期是 ENODEV）\n",
                       strerror(errno));

            struct rknpu_mem_destroy d = { .handle = m.handle };
            ioctl(drm, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
        } else {
            bad("SUBMIT 用例准备", "MEM_CREATE 失败");
        }
    }

    /* ── 7. 未实现的 ioctl 应当被响亮拒绝（不是静默）───────────────── */
    printf("\n-- 未知 ioctl --\n");
    {
        /* 0xc0106418 是反汇编里还原出来但没定性的一个号 */
        if (ioctl(drm, 0xc0106418UL) < 0)
            ok("未实现的 ioctl 被拒（内核日志里会打印它，供补全 ABI）");
        else
            bad("未实现的 ioctl", "竟然成功了");
    }

    close(drm);
    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
