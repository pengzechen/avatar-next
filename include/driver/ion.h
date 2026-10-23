/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/* include/driver/ion.h — 裸机 Ion 内存分配器公共接口

   参考：Android ION + ref: rcore-os/tgoskits sg2002-tpu/src/ion/

   功能：从 PMM 分配物理连续页, 映射到内核虚拟地址, 返回句柄。
   典型用途：为 TPU DMA 缓冲区分配物理连续内存。

   使用：
     void *va; uint64_t pa; ion_handle_t h;
     if (ion_alloc(size, &va, &pa, &h) == 0) {
         // DMA 操作使用 pa, CPU 访问使用 va
         cvi_tpu_run_dmabuf(va, pa);
         ion_free(h);
     }
*/
#ifndef DRIVER_ION_ION_H
#define DRIVER_ION_ION_H

#include "types.h"
#include "driver/fd_ops.h"

/* ── 句柄类型 ──────────────────────────────────────────────────────────── */
typedef uint32_t ion_handle_t;
#define ION_HANDLE_INVALID 0U

/* ── 堆类型(与 Linux ION ABI 兼容) ──────────────────────────────────── */
#define ION_HEAP_SYSTEM       0U
#define ION_HEAP_DMA_COHERENT 1U
#define ION_HEAP_CARVEOUT     2U

/* ── 最大并发缓冲区数 ─────────────────────────────────────────────────── */
#define ION_MAX_BUFS 64U

/* ── 服务操作表 ──────────────────────────────────────────────────────────
   driver/ion/ion.c 在 probe 时 ion_bind(&ion_ops)。未编译 ION 时指针为
   NULL，下面所有调用自然失败；内核据此决定 /dev/ion 是否可见。
*/
typedef struct {
    int (*alloc)(size_t size, void **vaddr, uint64_t *paddr, ion_handle_t *handle);
    int (*free)(ion_handle_t handle);
    int (*ref)(ion_handle_t handle);
    int (*get_buf)(ion_handle_t handle, void **vaddr, uint64_t *paddr);
    size_t (*get_size)(ion_handle_t handle);
    const fd_ops_t *fd_ops; /* ION fd 的 mmap/dup/close */
} ion_ops_t;

extern const ion_ops_t *g_ion_ops;
void ion_bind(const ion_ops_t *ops);

static inline bool ion_present(void)
{
    return g_ion_ops != NULL;
}

static inline int ion_alloc(size_t size, void **vaddr, uint64_t *paddr, ion_handle_t *handle)
{
    return g_ion_ops ? g_ion_ops->alloc(size, vaddr, paddr, handle) : -1;
}

static inline int ion_free(ion_handle_t handle)
{
    return g_ion_ops ? g_ion_ops->free(handle) : -1;
}

static inline int ion_ref(ion_handle_t handle)
{
    return g_ion_ops ? g_ion_ops->ref(handle) : -1;
}

static inline int ion_get_buf(ion_handle_t handle, void **vaddr, uint64_t *paddr)
{
    return g_ion_ops ? g_ion_ops->get_buf(handle, vaddr, paddr) : -1;
}

static inline size_t ion_get_size(ion_handle_t handle)
{
    return g_ion_ops ? g_ion_ops->get_size(handle) : 0;
}

#endif /* DRIVER_ION_ION_H */
