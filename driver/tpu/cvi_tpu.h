/* driver/tpu/cvi_tpu.h — SOPHGO/Cvitek TPU (TDMA + TIU) 裸机驱动接口
 *
 * 改写自 ref/tpu-ana/hal/mars/，移除所有 Linux 内核依赖。
 * 硬件：SG2002 / CV1800B
 *   TDMA (Tensor DMA)              : 0x0C100000
 *   TIU  (Tensor Instruction Unit) : 0x0C101000  (BD_CTRL@+0x100 = 0x0C101100)
 */
#ifndef DRIVER_TPU_CVI_TPU_H
#define DRIVER_TPU_CVI_TPU_H

#include "types.h"

/* ── TDMA 寄存器偏移 ──────────────────────────────────────────────────── */
#define TDMA_CTRL               0x00U
#define TDMA_DES_BASE           0x04U
#define TDMA_INT_MASK           0x08U
#define TDMA_SYNC_STATUS        0x0CU
#define TDMA_ARRAYBASE0_L       0x70U
#define TDMA_ARRAYBASE1_L       0x74U
#define TDMA_ARRAYBASE2_L       0x78U
#define TDMA_ARRAYBASE3_L       0x7CU
#define TDMA_ARRAYBASE4_L       0x80U
#define TDMA_ARRAYBASE5_L       0x84U
#define TDMA_ARRAYBASE6_L       0x88U
#define TDMA_ARRAYBASE7_L       0x8CU
#define TDMA_ARRAYBASE0_H       0x90U
#define TDMA_ARRAYBASE1_H       0x94U
#define TDMA_DEBUG_MODE         0xA0U
#define TDMA_DCM_DISABLE        0xA4U
#define TPUPMU_CTRL             0x200U
#define TPUPMU_BUFBASE          0x20CU
#define TPUPMU_BUFSIZE          0x210U

/* TDMA CTRL 位域 */
#define TDMA_CTRL_ENABLE_BIT        0U
#define TDMA_CTRL_MODESEL_BIT       1U
#define TDMA_CTRL_RESET_SYNCID_BIT  2U
#define TDMA_CTRL_FORCE_1ARRAY      5U
#define TDMA_CTRL_BURSTLEN_BIT      8U
#define TDMA_CTRL_64BYTE_ALIGN_EN   10U
#define TDMA_CTRL_INTRA_CMD_OFF     13U
#define TDMA_CTRL_DESNUM_BIT        16U

/* TDMA INT_MASK 状态（高16位右移后的值） */
#define TDMA_MASK_INIT      0x20U    /* 忽略 nchw/stride=0 错误 */
#define TDMA_INT_EOD        0x01U    /* End-of-descriptor */
#define TDMA_INT_EOPMU      0x8000U  /* End-of-PMU */

/* ── TIU 寄存器偏移 ───────────────────────────────────────────────────── */
#define BD_CTRL_BASE_ADDR           0x100U
#define BDC_ENGINE_CMD_ALIGNED_BIT  8U
/* BD_CTRL 位域 */
#define BD_TPU_EN           0U
#define BD_DES_ADDR_VLD     30U
#define BD_INTR_ENABLE      31U

/* ── DMA 缓冲头结构（128 字节，来自 cvi_tpu_interface.h） ─────────────── */
#define TPU_DMABUF_HEADER_M     0xB5B5U

struct cvi_tpu_dma_hdr {
    uint16_t dmabuf_magic_m;
    uint16_t dmabuf_magic_s;
    uint32_t dmabuf_size;
    uint32_t cpu_desc_count;
    uint32_t bd_desc_count;
    uint32_t tdma_desc_count;
    uint32_t tpu_clk_rate;
    uint32_t pmubuf_size;
    uint32_t pmubuf_offset;
    uint32_t arraybase_0_L;  uint32_t arraybase_0_H;
    uint32_t arraybase_1_L;  uint32_t arraybase_1_H;
    uint32_t arraybase_2_L;  uint32_t arraybase_2_H;
    uint32_t arraybase_3_L;  uint32_t arraybase_3_H;
    uint32_t arraybase_4_L;  uint32_t arraybase_4_H;
    uint32_t arraybase_5_L;  uint32_t arraybase_5_H;
    uint32_t arraybase_6_L;  uint32_t arraybase_6_H;
    uint32_t arraybase_7_L;  uint32_t arraybase_7_H;
    uint32_t reserve[8];
} __attribute__((packed));

/* CPU 同步描述符（来自 cvi_regcpu.h） */
struct cvi_tpu_cpu_sync_desc {
    uint32_t op_type;
    uint32_t num_bd;
    uint32_t num_gdma;
    uint32_t offset_bd;
    uint32_t offset_gdma;
    uint32_t reserved[2];
    char     str[196];   /* (56 - 7) * 4 bytes */
};

/* ── 公开 API ──────────────────────────────────────────────────────────── */
void cvi_tpu_init(void);
int  cvi_tpu_run_dmabuf(void *dmabuf_v, uint64_t dmabuf_p);
bool cvi_tpu_is_ready(void);

#endif /* DRIVER_TPU_CVI_TPU_H */
