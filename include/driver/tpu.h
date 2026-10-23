/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/tpu.h — TPU 的平台配置
*/

#ifndef DRIVER_TPU_H
#define DRIVER_TPU_H

#include "types.h"

typedef struct {
    uintptr_t tdma_base;
    uintptr_t tiu_base;
    uint32_t irq;
} tpu_cfg_t;

/*
   驱动在 probe 时 tpu_bind(&ops)；未编译 TPU 时指针为 NULL，
   内核据此决定 /dev/cvi-tpu0 是否可见。
*/
typedef struct {
    void (*init)(void);
    bool (*is_ready)(void);
    int (*run_dmabuf)(void *dmabuf_v, uint64_t dmabuf_p);
} tpu_ops_t;

extern const tpu_ops_t *g_tpu_ops;
void tpu_bind(const tpu_ops_t *ops);

static inline bool tpu_present(void)
{
    return g_tpu_ops != NULL;
}

static inline void cvi_tpu_init(void)
{
    if (g_tpu_ops)
        g_tpu_ops->init();
}

static inline bool cvi_tpu_is_ready(void)
{
    return g_tpu_ops ? g_tpu_ops->is_ready() : false;
}

static inline int cvi_tpu_run_dmabuf(void *dmabuf_v, uint64_t dmabuf_p)
{
    return g_tpu_ops ? g_tpu_ops->run_dmabuf(dmabuf_v, dmabuf_p) : -1;
}

#endif /* DRIVER_TPU_H */
