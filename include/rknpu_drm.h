/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/rknpu_drm.h — RK3588 NPU 的 DRM 渲染节点用户态 ABI

   目标：让未经修改的官方 librknnrt.so 能连上来。

   ── 这些号不是"设计"出来的 ─────────────────────────────────────────────
   全部是反汇编 librknnrt.so 实测的结果：定位 104 个 `bl ioctl@plt`
   调用点, 回溯 movz/movk 还原 w1, 再用每个请求号的 size 字段与已知
   结构体大小交叉验证。详见 docs/npu/RK3588_NPU_DRIVER.md §3。

   下面每个号都配了 _Static_assert：_IOC 把 sizeof 编进操作码, 结构体
   尺寸写错不是"参数错位"而是换了一个 ioctl 号, 内核会走到 default 分支。
   钉死之后改字段会在编译期被拦下, 而不是等上板才发现。

   ── 兼容范围 ───────────────────────────────────────────────────────────
   我们不实现 DRM, 只实现 librknnrt 用到的那一小撮：
     VERSION / GET_MAGIC / SET_MASTER / DROP_MASTER / AGP_ACQUIRE / AGP_RELEASE
     GEM_FLINK / PRIME_HANDLE_TO_FD / PRIME_FD_TO_HANDLE
     + RKNPU 自己的六个
   不认识的号一律打日志 + 回 -EINVAL —— 表是静态分析出来的下界,
   上板要靠日志补全(见文档 §8 验证策略)。
*/
#ifndef RKNPU_DRM_H
#define RKNPU_DRM_H

#include "types.h"

/* _IOC 宏：与 pseudofs.h / dma_buf.h 里那份逐字相同, `#ifndef` 保证
   谁先到谁生效, 同时包含也不冲突。
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

#define DRM_IOCTL_BASE   'd'
#define DRM_COMMAND_BASE 0x40

/* ── DRM 核心：librknnrt 实测用到的这几个 ─────────────────────────────── */

struct drm_version {
    int32_t version_major;
    int32_t version_minor;
    int32_t version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};
_Static_assert(sizeof(struct drm_version) == 64, "drm_version 必须 64 字节（librknnrt 用 0xc0406400 发它）");

struct drm_gem_flink {
    uint32_t handle; /* [in] */
    uint32_t name;   /* [out] */
};

struct drm_prime_handle {
    uint32_t handle; /* [in/out] */
    uint32_t flags;  /* [in] */
    int32_t fd;      /* [out]/[in] */
};
_Static_assert(sizeof(struct drm_prime_handle) == 12, "drm_prime_handle 必须 12 字节（0xc00c642d/2e）");

#define DRM_IOCTL_VERSION _IOWR(DRM_IOCTL_BASE, 0x00, struct drm_version)

/*
   DRM_IOCTL_GET_UNIQUE —— 取总线的唯一标识串(libdrm 的 drmGetBusid)。
   librknnrt 实测会调它(0xc0106401)；语义与 VERSION 一样是两阶段的,
   见 rknpu_drm.c 里 drm_ioctl_version 的说明。
*/
struct drm_unique {
    size_t unique_len;
    char *unique;
};
_Static_assert(sizeof(struct drm_unique) == 16, "drm_unique 必须 16 字节");
#define DRM_IOCTL_GET_UNIQUE _IOWR(DRM_IOCTL_BASE, 0x01, struct drm_unique)

/*
   DRM_IOCTL_SET_VERSION —— libdrm 的 drmSetVersion(), 协商 DRM 接口版本。

   librknnrt 真机上会发它(实测 0xc0106407, nr=0x7, size=16 正好对上)。
   我们以前落到 default 分支回 -EINVAL；虽然它多半容错, 但在还没跑通
   主流程的阶段, 少一个可疑的失败点是值的。
*/
struct drm_set_version {
    int32_t drm_di_major;
    int32_t drm_di_minor;
    int32_t drm_dd_major; /* [out] 驱动版本 */
    int32_t drm_dd_minor; /* [out] */
};
_Static_assert(sizeof(struct drm_set_version) == 16, "drm_set_version 必须 16 字节（0xc0106407）");
#define DRM_IOCTL_SET_VERSION _IOWR(DRM_IOCTL_BASE, 0x07, struct drm_set_version)

#define DRM_IOCTL_GET_MAGIC          _IOR(DRM_IOCTL_BASE, 0x02, uint32_t)
#define DRM_IOCTL_SET_MASTER         _IO(DRM_IOCTL_BASE, 0x1e)
#define DRM_IOCTL_DROP_MASTER        _IO(DRM_IOCTL_BASE, 0x1f)
#define DRM_IOCTL_AGP_ACQUIRE        _IO(DRM_IOCTL_BASE, 0x30)
#define DRM_IOCTL_AGP_RELEASE        _IO(DRM_IOCTL_BASE, 0x31)
#define DRM_IOCTL_GEM_FLINK          _IOWR(DRM_IOCTL_BASE, 0x0a, struct drm_gem_flink)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD _IOWR(DRM_IOCTL_BASE, 0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE _IOWR(DRM_IOCTL_BASE, 0x2e, struct drm_prime_handle)

/* 与实测值绑定：改了上面任何一个定义, 这里先炸 */
_Static_assert(DRM_IOCTL_VERSION == 0xc0406400U, "与 librknnrt 实测不符");
_Static_assert(DRM_IOCTL_SET_VERSION == 0xc0106407U, "与 librknnrt 实测不符");
_Static_assert(DRM_IOCTL_GET_MAGIC == 0x80046402U, "与 librknnrt 实测不符");
_Static_assert(DRM_IOCTL_SET_MASTER == 0x0000641eU, "与 librknnrt 实测不符");
_Static_assert(DRM_IOCTL_DROP_MASTER == 0x0000641fU, "与 librknnrt 实测不符");
_Static_assert(DRM_IOCTL_AGP_ACQUIRE == 0x00006430U, "与 librknnrt 实测不符");
_Static_assert(DRM_IOCTL_AGP_RELEASE == 0x00006431U, "与 librknnrt 实测不符");
_Static_assert(DRM_IOCTL_GEM_FLINK == 0xc008640aU, "与 librknnrt 实测不符");
_Static_assert(DRM_IOCTL_PRIME_HANDLE_TO_FD == 0xc00c642dU, "与 librknnrt 实测不符");
_Static_assert(DRM_IOCTL_PRIME_FD_TO_HANDLE == 0xc00c642eU, "与 librknnrt 实测不符");

/* ── RKNPU 自己的六个(DRM_COMMAND_BASE + n)───────────────────────────── */

#define RKNPU_ACTION      0x00
#define RKNPU_SUBMIT      0x01
#define RKNPU_MEM_CREATE  0x02
#define RKNPU_MEM_MAP     0x03
#define RKNPU_MEM_DESTROY 0x04
#define RKNPU_MEM_SYNC    0x05

#define DRM_IOCTL_RKNPU_ACTION     _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + RKNPU_ACTION, struct rknpu_action)
#define DRM_IOCTL_RKNPU_SUBMIT     _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + RKNPU_SUBMIT, struct rknpu_submit)
#define DRM_IOCTL_RKNPU_MEM_CREATE _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + RKNPU_MEM_CREATE, struct rknpu_mem_create)
#define DRM_IOCTL_RKNPU_MEM_MAP    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + RKNPU_MEM_MAP, struct rknpu_mem_map)
#define DRM_IOCTL_RKNPU_MEM_DESTROY \
    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + RKNPU_MEM_DESTROY, struct rknpu_mem_destroy)
#define DRM_IOCTL_RKNPU_MEM_SYNC _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + RKNPU_MEM_SYNC, struct rknpu_mem_sync)

/* ── RKNPU 结构体(照 vendor 的 include/rknpu_ioctl.h, 尺寸实测核对过)── */

/*
   任务描述符 `rknpu_task_t` 不在这里 —— 它已经在 driver/npu/rknpu.h 里
   (本驱动内部用)。硬件直接读它, 必须 packed。
*/

struct rknpu_subcore_task {
    uint32_t task_start;
    uint32_t task_number;
};

/*
   core_mask 的取值 —— 与 vendor 的 include/rknpu_job.h 逐字一致。

   AUTO(0) 是"交给驱动挑一个核", vendor 在这种情况下强制单核
   (`use_core_num = 1`)。见 rknpu_submit_task() 里任务切片的说明。
*/
#define RKNPU_CORE_AUTO_MASK 0x00
#define RKNPU_CORE0_MASK     0x01
#define RKNPU_CORE1_MASK     0x02
#define RKNPU_CORE2_MASK     0x04

struct rknpu_submit {
    uint32_t flags;
    uint32_t timeout;
    uint32_t task_start;
    uint32_t task_number;
    uint32_t task_counter;
    int32_t priority;
    uint64_t task_obj_addr;
    uint32_t iommu_domain_id;
    uint32_t reserved;
    uint64_t task_base_addr;
    int64_t hw_elapse_time;
    uint32_t core_mask;
    int32_t fence_fd;
    struct rknpu_subcore_task subcore_task[5];
};
_Static_assert(sizeof(struct rknpu_submit) == 104, "rknpu_submit 必须 104 字节（0xc0686441）");

struct rknpu_mem_create {
    uint32_t handle;
    uint32_t flags;
    uint64_t size;
    uint64_t obj_addr;
    uint64_t dma_addr;
    uint64_t sram_size;
    int32_t iommu_domain_id;
    uint32_t core_mask;
};
_Static_assert(sizeof(struct rknpu_mem_create) == 48, "rknpu_mem_create 必须 48 字节（0xc0306442）");

struct rknpu_mem_map {
    uint32_t handle;
    uint32_t reserved;
    uint64_t offset; /* [out] 给 mmap() 用的假偏移 */
};
_Static_assert(sizeof(struct rknpu_mem_map) == 16, "mem_map 必须 16 字节");

struct rknpu_mem_destroy {
    uint32_t handle;
    uint32_t reserved;
    uint64_t obj_addr;
};
_Static_assert(sizeof(struct rknpu_mem_destroy) == 16, "mem_destroy 必须 16 字节");

struct rknpu_mem_sync {
    uint32_t flags;
    uint32_t reserved;
    uint64_t obj_addr;
    uint64_t offset;
    uint64_t size;
};
_Static_assert(sizeof(struct rknpu_mem_sync) == 32, "mem_sync 必须 32 字节");

struct rknpu_action {
    uint32_t flags;
    uint32_t value;
};
_Static_assert(sizeof(struct rknpu_action) == 8, "action 必须 8 字节");

_Static_assert(DRM_IOCTL_RKNPU_ACTION == 0xc0086440U, "与实测不符");
_Static_assert(DRM_IOCTL_RKNPU_SUBMIT == 0xc0686441U, "与实测不符");
_Static_assert(DRM_IOCTL_RKNPU_MEM_CREATE == 0xc0306442U, "与实测不符");
_Static_assert(DRM_IOCTL_RKNPU_MEM_MAP == 0xc0106443U, "与实测不符");
_Static_assert(DRM_IOCTL_RKNPU_MEM_DESTROY == 0xc0106444U, "与实测不符");
_Static_assert(DRM_IOCTL_RKNPU_MEM_SYNC == 0xc0206445U, "与实测不符");

/* rknpu_action.flags —— 只列我们认识/需要应答的(全表见 vendor 头) */
#define RKNPU_GET_HW_VERSION      0
#define RKNPU_GET_DRV_VERSION     1
#define RKNPU_GET_FREQ            2
#define RKNPU_SET_FREQ            3
#define RKNPU_ACT_RESET           6
#define RKNPU_GET_IOMMU_EN        18
#define RKNPU_POWER_ON            20
#define RKNPU_POWER_OFF           21
#define RKNPU_GET_TOTAL_SRAM_SIZE 22
#define RKNPU_GET_FREE_SRAM_SIZE  23

/* rknpu_submit.flags */
#define RKNPU_JOB_BLOCK    (0 << 1)
#define RKNPU_JOB_PINGPONG (1 << 2)

/* rknpu_mem_create.flags */
#define RKNPU_MEM_NON_CONTIGUOUS (1U << 0)
#define RKNPU_MEM_CACHEABLE      (1U << 1)
#define RKNPU_MEM_IOMMU          (1U << 4)
#define RKNPU_MEM_ZEROING        (1U << 5)

/* rknpu_mem_sync.flags */
#define RKNPU_MEM_SYNC_TO_DEVICE   (1U << 0)
#define RKNPU_MEM_SYNC_FROM_DEVICE (1U << 1)

/*
   内核侧入口(rknpu_drm_ioctl_file / rknpu_drm_close_file /
   rknpu_drm_mmap_lookup)现在由 include/driver/npu.h 以内联包装暴露，
   经 g_npu_ops 分发；本头只保留用户态 ABI。
*/

#endif /* RKNPU_DRM_H */
