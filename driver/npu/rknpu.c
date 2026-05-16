/* driver/npu/rknpu.c
 *
 * Rockchip RK3588 NPU 驱动
 *
 * 初始化流程：
 *   1. 通过 rkpm 上电四个 NPU 电源域
 *   2. 清除遗留中断状态
 *   3. 读取并验证版本寄存器（魔数 0x46495245 = "FIRE"）
 *   4. 向 GICv3 注册 NPU0 IRQ 处理程序并使能中断
 *
 * 中断处理：
 *   读 INT_STATUS → 解析任务计数 → 清 INT_CLEAR
 *
 * 框架集成：
 *   - 通过 platform_get_uintptr("npu", "base0") 获取 MMIO 基地址
 *   - 通过 platform_get_uintptr("npu", "irq0") 获取中断号
 *   - 由 platform.lua 的 register_device("npu0") 在 drivers 阶段调用
 */

#include "rknpu.h"
#include "rkpm.h"

#include "types.h"
#include "mmio.h"
#include "klog.h"
#include "platform_cfg.h"
#include "exception.h"
#include "irq/gicv3.h"

/* ── 运行时状态 ──────────────────────────────────────────────────────────── */

/* 从 platform_cfg 获取的 NPU0 MMIO 基地址 */
static uintptr_t g_npu0_base = 0;
/* 从 platform_cfg 获取的 NPU0 GIC 中断号 */
static int g_npu0_irq = 0;
/* IRQ 触发次数（调试用）*/
static volatile int g_irq_count = 0;

/* ── RK3588 NPU 配置 ─────────────────────────────────────────────────────── */
static const rknpu_config_t rk3588_cfg = {
    .pc_data_amount_scale  = 2,
    .pc_task_number_bits   = 12,
    .pc_task_number_mask   = 0xFFF,
    .pc_task_status_offset = 0x3C,
    .core_mask             = 0x7,   /* 三核全启 */
};

/* ── 内联 MMIO 辅助（以 g_npu0_base 为基址）────────────────────────────── */
static inline uint32_t npu_read(uint32_t off)
{
    return read32((void *)(g_npu0_base + off));
}

static inline void npu_write(uint32_t val, uint32_t off)
{
    write32(val, (void *)(g_npu0_base + off));
}

/* ── 中断处理程序 ────────────────────────────────────────────────────────── */
static void rknpu_irq_handler(uint64_t *frame)
{
    (void)frame;

    uint32_t status = npu_read(RKNPU_INT_STATUS);
    uint32_t task_count = status & rk3588_cfg.pc_task_number_mask;

    g_irq_count++;
    KLOG_INFO("rknpu: IRQ #%d status=0x%x task_count=%u\n",
              g_irq_count, status, task_count);

    /* 清除所有中断位 */
    npu_write(RKNPU_INT_CLEAR_ALL, RKNPU_INT_CLEAR);

    /* 通知 GICv3 完成（EOI 由异常框架在 irq_handler 返回后写入）*/
}

/* ── 版本校验 ────────────────────────────────────────────────────────────── */
bool rknpu_validate_version(void)
{
    uint32_t version     = npu_read(RKNPU_VERSION);
    uint32_t version_num = npu_read(RKNPU_VERSION_NUM);

    KLOG_INFO("rknpu: version=0x%08x version_num=0x%x\n", version, version_num);

    if (version != RKNPU_VERSION_MAGIC) {
        KLOG_ERROR("rknpu: unexpected version 0x%08x (expected 0x%08x)\n",
                   version, RKNPU_VERSION_MAGIC);
        return false;
    }

    KLOG_INFO("rknpu: version OK (\"FIRE\")\n");
    return true;
}

/* ── 主初始化入口 ────────────────────────────────────────────────────────── */
void rknpu_init(void)
{
    KLOG_INFO("rknpu: initializing RK3588 NPU\n");

    /* 1. 从平台配置获取运行时参数 */
    g_npu0_base = platform_get_uintptr("npu", "base0");
    g_npu0_irq  = (int)platform_get_uintptr("npu", "irq0");

    if (g_npu0_base == 0) {
        KLOG_ERROR("rknpu: platform 'npu.base0' not set, aborting\n");
        return;
    }
    KLOG_INFO("rknpu: NPU0 base=0x%lx irq=%d\n",
              (unsigned long)g_npu0_base, g_npu0_irq);

    /* 2. 上电 NPU 电源域 */
    rkpm_t pm;
    uintptr_t pmu_base = platform_get_uintptr("npu", "pmu");
    if (pmu_base == 0)
        pmu_base = RKNPU_PMU1_BASE;   /* 回退到硬编码地址 */

    rkpm_init(&pm, pmu_base);

    int ret;
    ret = rkpm_power_on(&pm, PD_NPU);
    if (ret) KLOG_WARN("rknpu: PD_NPU power-on timeout\n");

    ret = rkpm_power_on(&pm, PD_NPUTOP);
    if (ret) KLOG_WARN("rknpu: PD_NPUTOP power-on timeout\n");

    ret = rkpm_power_on(&pm, PD_NPU1);
    if (ret) KLOG_WARN("rknpu: PD_NPU1 power-on timeout\n");

    ret = rkpm_power_on(&pm, PD_NPU2);
    if (ret) KLOG_WARN("rknpu: PD_NPU2 power-on timeout\n");

    KLOG_INFO("rknpu: NPU power domains on\n");

    /* 3. 清除遗留中断 */
    npu_write(RKNPU_INT_CLEAR_ALL, RKNPU_INT_CLEAR);

    /* 4. 校验版本寄存器 */
    rknpu_validate_version();

    /* 5. 注册 GICv3 中断 */
    if (g_npu0_irq > 0) {
        irq_install(g_npu0_irq, rknpu_irq_handler);
        gicv3_enable_int(g_npu0_irq, true);
        KLOG_INFO("rknpu: IRQ %d registered and enabled\n", g_npu0_irq);
    }

    KLOG_INFO("rknpu: init done\n");
}
