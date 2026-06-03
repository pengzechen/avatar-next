/* driver/ion/ion.h — 裸机 Ion 内存分配器接口
 *
 * 参考：Android ION + ref: rcore-os/tgoskits sg2002-tpu/src/ion/
 *
 * 功能：从 PMM 分配物理连续页，映射到内核虚拟地址，返回句柄。
 * 典型用途：为 TPU DMA 缓冲区分配物理连续内存。
 *
 * 使用：
 *   void *va; uint64_t pa; ion_handle_t h;
 *   if (ion_alloc(size, &va, &pa, &h) == 0) {
 *       // DMA 操作使用 pa，CPU 访问使用 va
 *       cvi_tpu_run_dmabuf(va, pa);
 *       ion_free(h);
 *   }
 */
#ifndef DRIVER_ION_ION_H
#define DRIVER_ION_ION_H

#include "types.h"

/* ── 句柄类型 ──────────────────────────────────────────────────────────── */
typedef uint32_t ion_handle_t;
#define ION_HANDLE_INVALID  0U

/* ── 堆类型（与 Linux ION ABI 兼容） ──────────────────────────────────── */
#define ION_HEAP_SYSTEM       0U
#define ION_HEAP_DMA_COHERENT 1U
#define ION_HEAP_CARVEOUT     2U

/* ── 最大并发缓冲区数 ─────────────────────────────────────────────────── */
#define ION_MAX_BUFS  64U

/* ── 公开 API ──────────────────────────────────────────────────────────── */

/**
 * ion_alloc — 分配 DMA 内存
 *
 * @size:   请求大小（字节），向上取整到页大小（4KB）
 * @vaddr:  [out] 内核虚拟地址
 * @paddr:  [out] 物理地址（供 DMA 引擎使用）
 * @handle: [out] 分配句柄（用于 ion_free / ion_get_buf）
 *
 * 返回 0 表示成功，-1 表示失败（内存不足或表满）。
 */
int ion_alloc(size_t size, void **vaddr, uint64_t *paddr, ion_handle_t *handle);

/**
 * ion_free — 释放已分配的缓冲区
 *
 * @handle: ion_alloc 返回的句柄
 *
 * 返回 0 表示成功，-1 表示句柄无效。
 */
int ion_free(ion_handle_t handle);

/**
 * ion_ref — 增加缓冲区引用计数
 *
 * @handle: ion_alloc 返回的句柄
 *
 * 返回 0 表示成功，-1 表示句柄无效。
 */
int ion_ref(ion_handle_t handle);

/**
 * ion_get_buf — 从句柄获取地址信息
 *
 * @handle: 有效的 ion_handle_t
 * @vaddr:  [out] 内核虚拟地址（可为 NULL）
 * @paddr:  [out] 物理地址（可为 NULL）
 *
 * 返回 0 表示成功，-1 表示句柄无效。
 */
int ion_get_buf(ion_handle_t handle, void **vaddr, uint64_t *paddr);

/**
 * ion_get_size — 获取缓冲区大小
 *
 * 返回缓冲区大小（字节），句柄无效时返回 0。
 */
size_t ion_get_size(ion_handle_t handle);

#endif /* DRIVER_ION_ION_H */
