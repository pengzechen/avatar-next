/* driver/tpu/cvi_tpu.c — SOPHGO/Cvitek TPU 裸机驱动
 *
 * 改写自 ref/tpu-ana/hal/mars/tpu_platform.c
 *
 * 主要变化（相比 Linux 原版）：
 *   · wait_for_completion_interruptible_timeout → 轮询 TDMA_INT_MASK（同步等待）
 *   · 移除 spinlock / mutex / clk_* / reset_control_* / dma_mapping
 *   · 移除 kernel thread（直接同步调用）
 *   · pr_debug / pr_err → KLOG_DEBUG / KLOG_ERROR
 *   · ioremap → platform_get_mmio
 */
#include "cvi_tpu.h"
#include "types.h"
#include "mmio.h"
#include "klog.h"
#include "platform_cfg.h"

/* ── 驱动状态 ───────────────────────────────────────────────────────────── */
static struct {
    uintptr_t tdma_base;
    uintptr_t tiu_base;
    int       initialized;
} g_tpu;

/* 轮询超时：~60s（粗略循环计数） */
#define TDMA_TIMEOUT_LOOP  60000000U

/* ── MMIO 读写辅助 ──────────────────────────────────────────────────────── */
static inline uint32_t tdma_read(uint32_t off)
{
    return read32((void *)(g_tpu.tdma_base + off));
}
static inline void tdma_write(uint32_t val, uint32_t off)
{
    write32(val, (void *)(g_tpu.tdma_base + off));
}
static inline uint32_t tiu_read(uint32_t off)
{
    return read32((void *)(g_tpu.tiu_base + off));
}
static inline void tiu_write(uint32_t val, uint32_t off)
{
    write32(val, (void *)(g_tpu.tiu_base + off));
}

/* ── 内部函数 ───────────────────────────────────────────────────────────── */

/*
 * resync_cmd_id — 复位 TIU/TDMA 指令 ID 及中断状态
 * 对应 Linux 中的 resync_cmd_id()
 */
static void resync_cmd_id(void)
{
    uint32_t v;

    /* 复位 TIU ID 计数器 */
    v = tiu_read(BD_CTRL_BASE_ADDR + 0xCU);
    tiu_write(v | 0x1U, BD_CTRL_BASE_ADDR + 0xCU);
    tiu_write(v & ~0x1U, BD_CTRL_BASE_ADDR + 0xCU);

    /* 停止 TIU */
    v = tiu_read(BD_CTRL_BASE_ADDR);
    tiu_write(v & ~((1U << BD_TPU_EN) | (1U << BD_DES_ADDR_VLD)),
              BD_CTRL_BASE_ADDR);

    /* 清 TIU 中断状态 (bit 1) */
    v = tiu_read(BD_CTRL_BASE_ADDR);
    tiu_write(v | (1U << 1U), BD_CTRL_BASE_ADDR);

    /* 复位 TDMA sync ID */
    tdma_write(1U << TDMA_CTRL_RESET_SYNCID_BIT, TDMA_CTRL);
    tdma_write(0U, TDMA_CTRL);

    /* 清 TDMA 中断状态 */
    tdma_write(0xFFFF0000U, TDMA_INT_MASK);
}

/*
 * set_array_bases — 将 dmabuf 头中的 8 个数组基地址写入 TDMA 寄存器
 */
static void set_array_bases(const struct cvi_tpu_dma_hdr *hdr)
{
    tdma_write(hdr->arraybase_0_L, TDMA_ARRAYBASE0_L);
    tdma_write(hdr->arraybase_1_L, TDMA_ARRAYBASE1_L);
    tdma_write(hdr->arraybase_2_L, TDMA_ARRAYBASE2_L);
    tdma_write(hdr->arraybase_3_L, TDMA_ARRAYBASE3_L);
    tdma_write(hdr->arraybase_4_L, TDMA_ARRAYBASE4_L);
    tdma_write(hdr->arraybase_5_L, TDMA_ARRAYBASE5_L);
    tdma_write(hdr->arraybase_6_L, TDMA_ARRAYBASE6_L);
    tdma_write(hdr->arraybase_7_L, TDMA_ARRAYBASE7_L);
    /* 高32位：SG2002 物理地址在 32 位范围内，高位填 0 */
    tdma_write(0U, TDMA_ARRAYBASE0_H);
    tdma_write(0U, TDMA_ARRAYBASE1_H);
}

/*
 * fire_tdma — 启动 TDMA 描述符执行
 * 对应 Linux 中的 set_tdma_descriptor_Fire()
 */
static void fire_tdma(uint64_t desc_offset, uint32_t num_tdma)
{
    tdma_write((uint32_t)desc_offset, TDMA_DES_BASE);
    tdma_write(0U, TDMA_DEBUG_MODE);
    tdma_write(0U, TDMA_DCM_DISABLE);
    tdma_write(TDMA_MASK_INIT, TDMA_INT_MASK);
    tdma_write((1U << TDMA_CTRL_ENABLE_BIT)       |
               (1U << TDMA_CTRL_MODESEL_BIT)       |
               (num_tdma << TDMA_CTRL_DESNUM_BIT)  |
               (3U << TDMA_CTRL_BURSTLEN_BIT)      |
               (1U << TDMA_CTRL_FORCE_1ARRAY)      |
               (1U << TDMA_CTRL_INTRA_CMD_OFF)     |
               (1U << TDMA_CTRL_64BYTE_ALIGN_EN),
               TDMA_CTRL);
}

/*
 * fire_tiu — 启动 TIU 描述符执行
 * 对应 Linux 中的 set_tiu_descriptor()
 */
static void fire_tiu(uint64_t desc_offset)
{
    uint64_t desc_addr = desc_offset << BDC_ENGINE_CMD_ALIGNED_BIT;
    uint32_t v;

    tiu_write((uint32_t)(desc_addr & 0xFFFFFFFFU), BD_CTRL_BASE_ADDR + 0x4U);
    v = tiu_read(BD_CTRL_BASE_ADDR + 0x8U);
    tiu_write((v & 0xFFFFFF00U) | ((uint32_t)(desc_addr >> 32U) & 0xFFU),
              BD_CTRL_BASE_ADDR + 0x8U);

    /* 禁用 pre_exe */
    v = tiu_read(BD_CTRL_BASE_ADDR + 0xCU);
    tiu_write(v | (1U << 11U), BD_CTRL_BASE_ADDR + 0xCU);

    /* 设置 1 array，lane=8 (bits[29:22] = 3) */
    v = tiu_read(BD_CTRL_BASE_ADDR);
    v &= ~0x3FC00000U;
    tiu_write(v | (3U << 22U), BD_CTRL_BASE_ADDR);

    /* 触发 TIU */
    v = tiu_read(BD_CTRL_BASE_ADDR);
    tiu_write(v | (1U << BD_DES_ADDR_VLD)
                | (1U << BD_INTR_ENABLE)
                | (1U << BD_TPU_EN),
              BD_CTRL_BASE_ADDR);
}

/*
 * poll_tdma_done — 轮询等待 TDMA 完成（替换 wait_for_completion_timeout）
 *
 * TDMA_INT_MASK[31:16] 是中断状态，屏蔽 TDMA_MASK_INIT 位后：
 *   TDMA_INT_EOD   (0x01) — 描述符执行完成
 *   TDMA_INT_EOPMU (0x8000) — PMU 缓冲满
 *   其他非零值 — 错误
 */
static int poll_tdma_done(void)
{
    uint32_t i;
    for (i = 0U; i < TDMA_TIMEOUT_LOOP; i++) {
        uint32_t reg        = tdma_read(TDMA_INT_MASK);
        uint32_t int_status = (reg >> 16U) & ~TDMA_MASK_INIT;

        if (int_status == TDMA_INT_EOD || int_status == TDMA_INT_EOPMU) {
            tdma_write(0xFFFF0000U, TDMA_INT_MASK);  /* 清中断 */
            return 0;
        }
        if (int_status != 0U) {
            KLOG_ERROR("cvi_tpu: TDMA error int_status=0x%x reg=0x%x\n",
                       int_status, reg);
            tdma_write(0xFFFF0000U, TDMA_INT_MASK);
            return -1;
        }
    }
    KLOG_ERROR("cvi_tpu: TDMA poll timeout\n");
    return -1;
}

/*
 * poll_tiu_done — 轮询等待 TIU 完成指定 BD 数
 * 对应 Linux 中的 poll_cmdbuf_done() TIU 部分
 */
static int poll_tiu_done(uint32_t bd_cmd_id)
{
    uint32_t i;
    if (bd_cmd_id == 0U)
        return 0;
    for (i = 0U; i < TDMA_TIMEOUT_LOOP; i++) {
        uint32_t v       = tiu_read(BD_CTRL_BASE_ADDR);
        uint32_t done_id = (v >> 6U) & 0xFFFFU;
        bool     done    = (v & (1U << 1U)) != 0U;
        if (done_id >= bd_cmd_id && done) {
            tiu_write(v | (1U << 1U), BD_CTRL_BASE_ADDR);  /* 清中断 */
            return 0;
        }
    }
    KLOG_ERROR("cvi_tpu: TIU poll timeout bd_cmd_id=%u\n", bd_cmd_id);
    return -1;
}

/* ── 公开 API ────────────────────────────────────────────────────────────── */

/*
 * cvi_tpu_init — 从 platform_cfg 读取 MMIO 基地址，完成 TPU 基本初始化
 */
void cvi_tpu_init(void)
{
    g_tpu.tdma_base  = platform_get_mmio("tpu", "tdma_base");
    g_tpu.tiu_base   = platform_get_mmio("tpu", "tiu_base");
    g_tpu.initialized = 0;

    if (!g_tpu.tdma_base || !g_tpu.tiu_base) {
        KLOG_ERROR("cvi_tpu: tpu.tdma_base or tpu.tiu_base not in platform.lua\n");
        return;
    }

    KLOG_INFO("cvi_tpu: TDMA=0x%lx TIU=0x%lx\n",
              (unsigned long)g_tpu.tdma_base,
              (unsigned long)g_tpu.tiu_base);

    /* 清残留中断，复位指令 ID */
    tdma_write(0xFFFF0000U, TDMA_INT_MASK);
    resync_cmd_id();

    g_tpu.initialized = 1;
    KLOG_INFO("cvi_tpu: initialized\n");
}

bool cvi_tpu_is_ready(void)
{
    return g_tpu.initialized != 0;
}

/*
 * cvi_tpu_run_dmabuf — 执行一个 DMA 缓冲（同步轮询，无中断/线程）
 *
 * @dmabuf_v: DMA 缓冲虚拟地址（含 dma_hdr + cpu_sync_desc 数组）
 * @dmabuf_p: DMA 缓冲物理地址（供 TPU DMA 引擎使用）
 *
 * 改写自 platform_run_dmabuf()，直接同步等待替代 wait_for_completion。
 */
int cvi_tpu_run_dmabuf(void *dmabuf_v, uint64_t dmabuf_p)
{
    uint32_t i;
    int ret;

    if (!g_tpu.initialized) {
        KLOG_ERROR("cvi_tpu: not initialized\n");
        return -1;
    }

    const struct cvi_tpu_dma_hdr      *hdr  =
        (const struct cvi_tpu_dma_hdr *)dmabuf_v;
    const struct cvi_tpu_cpu_sync_desc *desc =
        (const struct cvi_tpu_cpu_sync_desc *)
        ((const uint8_t *)dmabuf_v + sizeof(*hdr));

    if (hdr->dmabuf_magic_m != TPU_DMABUF_HEADER_M) {
        KLOG_ERROR("cvi_tpu: bad magic 0x%x (expect 0x%x)\n",
                   hdr->dmabuf_magic_m, TPU_DMABUF_HEADER_M);
        return -1;
    }

    /* dmabuf 必须 4KB 页对齐（TPU DMA 引擎要求） */
    if (dmabuf_p & 0xFFFU) {
        KLOG_ERROR("cvi_tpu: dmabuf_p=0x%llx not page-aligned\n",
                   (unsigned long long)dmabuf_p);
        return -1;
    }

    KLOG_DEBUG("cvi_tpu: run_dmabuf p=0x%llx cpu_desc=%u\n",
               (unsigned long long)dmabuf_p, hdr->cpu_desc_count);

    /* 写入数组基地址 */
    set_array_bases(hdr);

    /* PMU 缓冲（可选，需 16 字节对齐） */
    bool pmu_en = (hdr->pmubuf_offset != 0U && hdr->pmubuf_size != 0U
                   && (hdr->pmubuf_offset & 0xFU) == 0U
                   && (hdr->pmubuf_size   & 0xFU) == 0U);
    if (pmu_en) {
        uint64_t pmu_p    = dmabuf_p + hdr->pmubuf_offset;
        uint32_t buf_addr = (uint32_t)(pmu_p >> 4U);
        uint32_t buf_size = hdr->pmubuf_size >> 4U;
        uint32_t pmu_ctrl = 0U;
        tdma_write(buf_addr, TPUPMU_BUFBASE);
        tdma_write(buf_size, TPUPMU_BUFSIZE);
        /* event=TdmaBandwidth(0x2)«带宽统计», enable+tpu+tdma, burst=16, ring-buf */
        pmu_ctrl |= 0x1U;              /* enable */
        pmu_ctrl |= 0x8U;              /* enable_tpu */
        pmu_ctrl |= 0x10U;             /* enable_tdma */
        pmu_ctrl |= (0x2U << 5U);      /* event = TdmaBandwidth */
        pmu_ctrl |= (0x3U << 8U);      /* burst length = 16 */
        pmu_ctrl |= (0x1U << 10U);     /* ring buffer mode */
        pmu_ctrl &= ~0xFFFF0000U;      /* enable dcm */
        tdma_write(pmu_ctrl, TPUPMU_CTRL);
    }

    for (i = 0U; i < hdr->cpu_desc_count; i++, desc++) {
        uint32_t bd_num   = desc->num_bd   & 0xFFFFU;
        uint32_t tdma_num = desc->num_gdma & 0xFFFFU;

        KLOG_DEBUG("cvi_tpu: desc[%u] bd=%u tdma=%u\n", i, bd_num, tdma_num);

        resync_cmd_id();

        if (bd_num > 0U)
            fire_tiu(desc->offset_bd);
        if (tdma_num > 0U)
            fire_tdma(desc->offset_gdma, tdma_num);

        /* 同步轮询等待 TDMA（替换 wait_for_completion_interruptible_timeout） */
        if (tdma_num > 0U) {
            ret = poll_tdma_done();
            if (ret) {
                KLOG_ERROR("cvi_tpu: TDMA failed at desc[%u]\n", i);
                return ret;
            }
        }

        ret = poll_tiu_done(bd_num);
        if (ret) {
            KLOG_ERROR("cvi_tpu: TIU failed at desc[%u]\n", i);
            return ret;
        }
    }

    /* 停止 PMU，等待 EOPMU */
    if (pmu_en) {
        uint32_t v = tdma_read(TPUPMU_CTRL);
        tdma_write(v & ~0x1U, TPUPMU_CTRL);
        poll_tdma_done();
    }

    KLOG_DEBUG("cvi_tpu: run_dmabuf done\n");
    return 0;
}
