/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/npu.h — NPU 平台配置与公共接口

   这里只放 VMM 透传和通用内核路径需要的东西：核数、寄存器偏移、
   宿主让出/收回、地址翻译。具体驱动的任务描述符/提交路径仍在
   driver/npu/rknpu.h（私有实现细节）。
*/

#ifndef DRIVER_NPU_H
#define DRIVER_NPU_H

#include "types.h"

typedef struct {
    uintptr_t base[3]; /* NPU core0/1/2 原始物理基址 */
    uint32_t irq[3];
    uintptr_t pmu; /* PMU1 基址 */
} npu_cfg_t;

/* ── 硬件常量(VMM 透传窗口内的寄存器偏移)─────────────────────────── */
#define RKNPU_MAX_CORES 3

#define RKNPU_VERSION              0x0000       /* 版本魔数, 应为 0x46495245 */
#define RKNPU_VERSION_NUM          0x0004       /* 子版本号 */
#define RKNPU_PC_OP_EN             0x0008       /* 写 1 提交, 写 0 清除 */
#define RKNPU_PC_DATA_ADDR         0x0010       /* DMA 描述符地址 */
#define RKNPU_PC_DATA_AMOUNT       0x0014       /* 描述符数量 */
#define RKNPU_INT_MASK             0x0020       /* 中断掩码 */
#define RKNPU_INT_CLEAR            0x0024       /* 写 1 清除中断 */
#define RKNPU_INT_STATUS           0x0028       /* 中断状态(含任务计数) */
#define RKNPU_INT_RAW_STATUS       0x002C       /* 原始中断状态 */
#define RKNPU_PC_TASK_CONTROL      0x0030       /* 任务控制寄存器 */
#define RKNPU_PC_DMA_BASE_ADDR     0x0034       /* DMA 基址 */
#define RKNPU_PC_TASK_STATUS       0x003C       /* 任务执行状态 */
#define RKNPU_VERSION_MAGIC        0x46495245UL /* "FIRE" */
#define RKNPU_INT_CLEAR_ALL        0x1FFFFU
#define RKNPU_PC_DATA_EXTRA_AMOUNT 4U

/* ── 服务操作表 ──────────────────────────────────────────────────────
   driver/npu/rknpu.c 的 probe 调 npu_bind(&ops)。未编译 NPU 时指针为
   NULL：内核/VMM 据此决定 /dev/dri、/proc/dri 是否可见，所有调用是
   空操作/失败，不需要逐函数弱符号。
   `struct vm` 保持不透明，避免公共头依赖 VMM 内部结构。 */
struct vm;

typedef struct {
    void (*host_release)(void);
    void (*host_reclaim)(void);
    int (*mmu_init)(void);
    int (*mmu_enable)(uint64_t ipa_base, uint64_t ipa_size);
    void (*mmu_disable)(void);
    int (*mmu_sync)(struct vm *vm);
    void (*mmu_clear_fault)(void);
    void (*mmu_dump)(void);
    bool (*mmu_any_fault)(void);
    void (*mmu_probe)(struct vm *vm, uint64_t ipa);
    int (*drm_ioctl_file)(void *file, uint64_t req, void *argp);
    int (*drm_close_file)(void *file);
    int (*drm_mmap_lookup)(void *file, uint64_t offset, uint64_t *paddr_out, uint64_t *size_out);
} npu_ops_t;

extern const npu_ops_t *g_npu_ops;
void npu_bind(const npu_ops_t *ops);

static inline bool npu_present(void)
{
    return g_npu_ops != NULL;
}

static inline void rknpu_host_release(void)
{
    if (g_npu_ops)
        g_npu_ops->host_release();
}

static inline void rknpu_host_reclaim(void)
{
    if (g_npu_ops)
        g_npu_ops->host_reclaim();
}

static inline int npu_mmu_init(void)
{
    return g_npu_ops ? g_npu_ops->mmu_init() : -1;
}

static inline int npu_mmu_enable(uint64_t ipa_base, uint64_t ipa_size)
{
    return g_npu_ops ? g_npu_ops->mmu_enable(ipa_base, ipa_size) : -1;
}

static inline void npu_mmu_disable(void)
{
    if (g_npu_ops)
        g_npu_ops->mmu_disable();
}

static inline int npu_mmu_sync(struct vm *vm)
{
    return g_npu_ops ? g_npu_ops->mmu_sync(vm) : 0;
}

static inline void npu_mmu_clear_fault(void)
{
    if (g_npu_ops)
        g_npu_ops->mmu_clear_fault();
}

static inline void npu_mmu_dump(void)
{
    if (g_npu_ops)
        g_npu_ops->mmu_dump();
}

static inline bool npu_mmu_any_fault(void)
{
    return g_npu_ops ? g_npu_ops->mmu_any_fault() : false;
}

static inline void npu_mmu_probe(struct vm *vm, uint64_t ipa)
{
    if (g_npu_ops)
        g_npu_ops->mmu_probe(vm, ipa);
}

/*
   DRM 兼容入口。pseudofs 的静态节点表和 sys_mmap 直接引用这三个名字；
   没编 NPU 时它们是普通内联函数（返回 ENOSYS/-1），没有额外符号。
*/
static inline int rknpu_drm_ioctl_file(void *file, uint64_t req, void *argp)
{
    return g_npu_ops ? g_npu_ops->drm_ioctl_file(file, req, argp) : -1;
}

static inline int rknpu_drm_close_file(void *file)
{
    return g_npu_ops ? g_npu_ops->drm_close_file(file) : 0;
}

static inline int rknpu_drm_mmap_lookup(void *file, uint64_t offset, uint64_t *paddr_out, uint64_t *size_out)
{
    return g_npu_ops ? g_npu_ops->drm_mmap_lookup(file, offset, paddr_out, size_out) : -1;
}

#endif /* DRIVER_NPU_H */
