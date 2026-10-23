/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/npu/npu_impl.h — rknpu 内部实现之间的接口（不进公共头）

   公共世界只看到 include/driver/npu.h 的 npu_ops_t；本文件给 rknpu.c
   组装 ops 表用。没有 NPU 的构建不会编译这些实现。
*/

#ifndef DRIVER_NPU_IMPL_H
#define DRIVER_NPU_IMPL_H

#include "types.h"

struct vm;

void rknpu_impl_host_release(void);
void rknpu_impl_host_reclaim(void);

int npu_mmu_impl_init(void);
int npu_mmu_impl_enable(uint64_t ipa_base, uint64_t ipa_size);
void npu_mmu_impl_disable(void);
int npu_mmu_impl_sync(struct vm *vm);
void npu_mmu_impl_clear_fault(void);
void npu_mmu_impl_dump(void);
bool npu_mmu_impl_any_fault(void);
void npu_mmu_impl_probe(struct vm *vm, uint64_t ipa);

int rknpu_drm_impl_ioctl_file(void *file, uint64_t req, void *argp);
int rknpu_drm_impl_close_file(void *file);
int rknpu_drm_impl_mmap_lookup(void *file, uint64_t offset, uint64_t *paddr_out, uint64_t *size_out);

#endif /* DRIVER_NPU_IMPL_H */
