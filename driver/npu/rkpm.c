/* driver/npu/rkpm.c
 *
 * Rockchip RK3588 电源域管理实现
 *
 * 仅实现 NPU 所需的上电路径（PD_NPU / PD_NPUTOP / PD_NPU1 / PD_NPU2）。
 * 遵循 PMU1 的写掩码寄存器协议：高 16 位为写使能掩码，低 16 位为数据。
 */

#include "rkpm.h"
#include "klog.h"

/* ── RK3588 电源域描述符表 ───────────────────────────────────────────────── *
 *
 * 来源：Linux kernel drivers/soc/rockchip/pm_domains.c rk3588_pm_domains[]
 *   PD_NPU    pwr=BIT(1) status=BIT(1) → PWR 寄存器写掩码
 *   PD_NPUTOP pwr=BIT(3) idle=BIT(1)   repair_status=BIT(2)
 *   PD_NPU1   pwr=BIT(4) idle=BIT(2)   repair_status=BIT(3)
 *   PD_NPU2   pwr=BIT(5) idle=BIT(3)   repair_status=BIT(4)
 */
static const rkpm_domain_t rk3588_npu_domains[] = {
    /* PD_NPU */
    {
        .name               = "npu",
        .pwr_mask           = (1u << 1),
        .pwr_w_mask         = (1u << 1) << 16,
        .status_mask        = (1u << 1),  /* 0 = on */
        .repair_status_mask = 0,
        .req_mask           = 0,
        .idle_mask          = 0,
    },
    /* PD_NPUTOP */
    {
        .name               = "nputop",
        .pwr_mask           = (1u << 3),
        .pwr_w_mask         = (1u << 3) << 16,
        .status_mask        = 0,
        .repair_status_mask = (1u << 2),  /* repair_status bit 2 = ready */
        .req_mask           = (1u << 1),
        .idle_mask          = (1u << 1),
    },
    /* PD_NPU1 */
    {
        .name               = "npu1",
        .pwr_mask           = (1u << 4),
        .pwr_w_mask         = (1u << 4) << 16,
        .status_mask        = 0,
        .repair_status_mask = (1u << 3),
        .req_mask           = (1u << 2),
        .idle_mask          = (1u << 2),
    },
    /* PD_NPU2 */
    {
        .name               = "npu2",
        .pwr_mask           = (1u << 5),
        .pwr_w_mask         = (1u << 5) << 16,
        .status_mask        = 0,
        .repair_status_mask = (1u << 4),
        .req_mask           = (1u << 3),
        .idle_mask          = (1u << 3),
    },
};

#define NUM_NPU_DOMAINS \
    (sizeof(rk3588_npu_domains) / sizeof(rk3588_npu_domains[0]))

/* ── 内部：等待域上电稳定 ────────────────────────────────────────────────── */
static int rkpm_wait_on(rkpm_t *pm, const rkpm_domain_t *d)
{
    int i;

    /* 优先用 repair_status（有则等待对应位置 1 表示就绪）*/
    if (d->repair_status_mask) {
        for (i = 0; i < RKPM_MAX_WAIT; i++) {
            uint32_t v = rkpm_read(pm, PMU_REPAIR_STATUS_OFF);
            if (v & d->repair_status_mask)
                return 0;
        }
        KLOG_ERROR("rkpm: timeout waiting repair_status for %s\n", d->name);
        return RKPM_ERR_TIMEOUT;
    }

    /* 无 repair_status：用 status_mask（0 表示上电完成）*/
    if (d->status_mask) {
        for (i = 0; i < RKPM_MAX_WAIT; i++) {
            uint32_t v = rkpm_read(pm, PMU_STATUS_OFFSET);
            if (!(v & d->status_mask))
                return 0;
        }
        KLOG_ERROR("rkpm: timeout waiting status for %s\n", d->name);
        return RKPM_ERR_TIMEOUT;
    }

    /* 既无 repair_status 也无 status_mask：认为立即完成 */
    return 0;
}

/* ── 公开 API ────────────────────────────────────────────────────────────── */

void rkpm_init(rkpm_t *pm, uintptr_t base)
{
    pm->base = (void *)base;
}

int rkpm_power_on(rkpm_t *pm, uint32_t id)
{
    const rkpm_domain_t *d;

    if (id >= NUM_NPU_DOMAINS) {
        KLOG_ERROR("rkpm: invalid domain id %u\n", id);
        return -1;
    }
    d = &rk3588_npu_domains[id];

    KLOG_DEBUG("rkpm: powering on %s\n", d->name);

    /*
     * 写掩码协议：高 16 位置 1 表示允许写对应低位。
     * 上电 = 清零 pwr_mask 位，同时写使能 pwr_w_mask（高16位）。
     */
    if (d->pwr_w_mask) {
        /* 写入 "高16位=pwr_w_mask 低16位=0"（清除 pwr 位 = 上电）*/
        rkpm_write(pm, PMU_PWR_OFFSET, d->pwr_w_mask);
    } else {
        /* 无写掩码：读改写 */
        uint32_t cur = rkpm_read(pm, PMU_PWR_OFFSET);
        rkpm_write(pm, PMU_PWR_OFFSET, cur & ~d->pwr_mask);
    }

    return rkpm_wait_on(pm, d);
}
