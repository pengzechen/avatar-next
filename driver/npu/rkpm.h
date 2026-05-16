/* driver/npu/rkpm.h
 *
 * Rockchip RK3588 电源域管理（PMU）
 *
 * 封装 PMU1 MMIO 寄存器操作，用于在 NPU 初始化前上电
 * NPU 相关电源域（PD_NPU / PD_NPUTOP / PD_NPU1 / PD_NPU2）。
 *
 * 参考: testos-reflector/src/npu/rkpm.h + Linux rk3588_pm_domains
 */

#ifndef __RKPM_H__
#define __RKPM_H__

#include "types.h"
#include "mmio.h"

/* ── 电源域 ID（与 rk3588_domains[] 索引对应）──────────────────────────── */
#define PD_NPU     0  /* NPU top-level power island */
#define PD_NPUTOP  1  /* NPU bus & logic */
#define PD_NPU1    2  /* NPU core 1 */
#define PD_NPU2    3  /* NPU core 2 */

/* 最大等待轮数（轮询状态位）*/
#define RKPM_MAX_WAIT  10000

/* 返回错误码 */
#define RKPM_ERR_TIMEOUT  (-1)

/* ── PMU 寄存器偏移（相对 PMU1 基地址）──────────────────────────────────── */
#define PMU_PWR_OFFSET          0x14C   /* 写电源控制（带写掩码方式）*/
#define PMU_STATUS_OFFSET       0x180   /* 电源域状态 */
#define PMU_REQ_OFFSET          0x10C   /* IDLE 请求 */
#define PMU_IDLE_OFFSET         0x120   /* IDLE 状态 */
#define PMU_ACK_OFFSET          0x118   /* IDLE ACK */
#define PMU_REPAIR_STATUS_OFF   0x290   /* repair done 状态 */

/* ── 电源域描述符 ────────────────────────────────────────────────────────── */
typedef struct {
    const char *name;
    uint32_t    pwr_mask;           /* PWR 寄存器中对应位 */
    uint32_t    pwr_w_mask;         /* 写掩码高 16 位 = pwr_mask << 16 */
    uint32_t    status_mask;        /* STATUS 寄存器中对应位（0=on） */
    uint32_t    repair_status_mask; /* REPAIR_STATUS 中的就绪位（0=无） */
    uint32_t    req_mask;           /* IDLE REQ 位 */
    uint32_t    idle_mask;          /* IDLE 状态位 */
} rkpm_domain_t;

/* ── PMU 句柄 ────────────────────────────────────────────────────────────── */
typedef struct {
    void *base;  /* PMU1 MMIO 基地址（虚拟，mmio_vma=false 时 = 物理）*/
} rkpm_t;

/* ── 内联 MMIO 访问 ──────────────────────────────────────────────────────── */
static inline void rkpm_write(rkpm_t *pm, uint32_t off, uint32_t val)
{
    write32(val, (void *)((uintptr_t)pm->base + off));
}

static inline uint32_t rkpm_read(rkpm_t *pm, uint32_t off)
{
    return read32((void *)((uintptr_t)pm->base + off));
}

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * rkpm_init - 初始化 PMU 句柄
 * @pm:   指向 rkpm_t 结构体
 * @base: PMU1 MMIO 基地址（物理）
 */
void rkpm_init(rkpm_t *pm, uintptr_t base);

/**
 * rkpm_power_on - 上电指定电源域
 * @pm: PMU 句柄
 * @id: 电源域 ID（PD_NPU / PD_NPUTOP / PD_NPU1 / PD_NPU2）
 * 返回 0 = 成功，RKPM_ERR_TIMEOUT = 超时
 */
int rkpm_power_on(rkpm_t *pm, uint32_t id);

#endif /* __RKPM_H__ */
