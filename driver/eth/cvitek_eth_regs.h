#ifndef CVITEK_ETH_REGS_H
#define CVITEK_ETH_REGS_H

/*
 * driver/eth/cvitek_eth_regs.h — Synopsys DesignWare MAC（DWMAC 3.70a）寄存器布局
 *
 * SG2002 / CV1812H 板载以太网控制器。寄存器偏移与位域命名对齐 Linux
 * drivers/net/ethernet/stmicro/stmmac/dwmac1000.h：
 *   GMAC 区段：base + 0x0000 .. 0x0FFF
 *   DMA  区段：base + 0x1000 .. 0x1FFF
 *
 * 所有宏都带 CVITEK_ 前缀 —— driver/ 下的 .c 共享同一套 CFLAGS 编译，
 * 而本表里 RE/TE/PS/DM/FS/LS 这类位域名过于通用，不加前缀必然撞车。
 */

#include "types.h"

/* ── GMAC 寄存器偏移（base + 0x0000..0x0FFF）────────────────────────────── */

#define CVITEK_GMAC_MAC_CONTROL   0x0000U  /* MAC 总开关              */
#define CVITEK_GMAC_FRAME_FILTER  0x0004U  /* 地址过滤策略            */
#define CVITEK_GMAC_HASH_HIGH     0x0008U
#define CVITEK_GMAC_HASH_LOW      0x000CU
#define CVITEK_GMAC_MII_ADDR      0x0010U  /* MDIO 地址/控制          */
#define CVITEK_GMAC_MII_DATA      0x0014U  /* MDIO 数据               */
#define CVITEK_GMAC_VERSION       0x0020U  /* DWMAC 版本号            */
#define CVITEK_GMAC_INT_MASK      0x003CU  /* GMAC 侧中断屏蔽         */
#define CVITEK_GMAC_ADDR0_HIGH    0x0040U  /* MAC 地址高 16 bit       */
#define CVITEK_GMAC_ADDR0_LOW     0x0044U  /* MAC 地址低 32 bit       */
#define CVITEK_GMAC_MMC_CNTRL     0x0100U  /* MMC 计数器控制          */

/* ── GMAC_CONTROL 位域 ──────────────────────────────────────────────────── */

#define CVITEK_MACCTL_RE          (1U << 2)   /* Receiver Enable              */
#define CVITEK_MACCTL_TE          (1U << 3)   /* Transmitter Enable           */
#define CVITEK_MACCTL_DC          (1U << 4)   /* Deferral Check               */
#define CVITEK_MACCTL_ACS         (1U << 7)   /* Auto-pad/CRC stripping       */
#define CVITEK_MACCTL_DM          (1U << 11)  /* Full duplex mode             */
#define CVITEK_MACCTL_FES         (1U << 14)  /* Speed: 0=10M, 1=100M (MII)   */
#define CVITEK_MACCTL_PS          (1U << 15)  /* Port select: 0=GMII, 1=MII   */
#define CVITEK_MACCTL_DCRS        (1U << 16)  /* Disable CRC check            */

/* ── FRAME_FILTER 位域 ──────────────────────────────────────────────────── */

#define CVITEK_FILTER_PR          (1U << 0)   /* Promiscuous Mode             */
#define CVITEK_FILTER_RA          (1U << 31)  /* Receive All                  */

/* ── MII_ADDR 位域（IEEE 802.3 clause 22）───────────────────────────────── */

#define CVITEK_MIIADDR_BUSY       (1U << 0)   /* 1 = busy                     */
#define CVITEK_MIIADDR_WRITE      (1U << 1)   /* 1 = write, 0 = read          */
#define CVITEK_MIIADDR_CLK_CSR_SHIFT  2       /* MDC 时钟分频                 */
#define CVITEK_MIIADDR_CLK_CSR_MASK   (0xFU << 2)
#define CVITEK_MIIADDR_REG_SHIFT      6       /* PHY 寄存器地址               */
#define CVITEK_MIIADDR_REG_MASK       (0x1FU << 6)
#define CVITEK_MIIADDR_PHY_SHIFT      11      /* PHY 设备地址                 */
#define CVITEK_MIIADDR_PHY_MASK       (0x1FU << 11)

/*
 * MII_CLK_CSR 字段值：CSR 时钟落在 60–100 MHz 时用 /42 分频（值 = 4）。
 * 注意：MII_ADDR 是「写进去就生效」的整字寄存器，每次访问都要把
 * PHY/REG/CLK_CSR 三个字段连同 BUSY 一起拼好再写，不能用读改写。
 */
#define CVITEK_MII_CLK_CSR_60_100M_DIV42  0x4U

/* ── ADDR0_HIGH 位域 ───────────────────────────────────────────────────── */

#define CVITEK_ADDR0HI_MASK       0xFFFFU     /* MAC 地址第 5、6 字节         */
#define CVITEK_ADDR0HI_ENABLE     (1U << 31)  /* AE：bit31 必须置 1           */

/* ── DMA 寄存器偏移（base + 0x1000..0x1FFF）─────────────────────────────── */

#define CVITEK_DMA_BUS_MODE       0x1000U
#define CVITEK_DMA_TX_POLL        0x1004U  /* 写 1 触发 TX 轮询            */
#define CVITEK_DMA_RX_POLL        0x1008U  /* 写 1 触发 RX 轮询            */
#define CVITEK_DMA_RX_BASE        0x100CU  /* RX 描述符环基址（物理）      */
#define CVITEK_DMA_TX_BASE        0x1010U  /* TX 描述符环基址（物理）      */
#define CVITEK_DMA_STATUS         0x1014U  /* W1C 中断状态                 */
#define CVITEK_DMA_OPERATION      0x1018U  /* 收发使能 + FIFO 模式         */
#define CVITEK_DMA_INTR_ENA       0x101CU

/* ── DMA_BUS_MODE 位域 ─────────────────────────────────────────────────── */

#define CVITEK_BUSMODE_SWR        (1U << 0)   /* Software Reset               */
#define CVITEK_BUSMODE_DSL_SHIFT  2           /* Descriptor Skip Length（字） */
#define CVITEK_BUSMODE_DSL_MASK   (0x1FU << 2)
#define CVITEK_BUSMODE_PBL_SHIFT  8           /* Programmable Burst Length    */
#define CVITEK_BUSMODE_PBL_MASK   (0x3FU << 8)
#define CVITEK_BUSMODE_FB         (1U << 16)  /* Fixed Burst Length           */
#define CVITEK_BUSMODE_AAL        (1U << 25)  /* Address-Aligned Beats        */

/*
 * DSL = 12 words：两条相邻描述符之间额外跳 48 字节，配合描述符本身 16 字节
 * 形成 **64 byte stride**，正好等于 C906 的 cache line 长度。
 * 这个值和 dma_desc_t 的 64 字节对齐是**一对**，改一个必须改另一个，
 * 否则 DMA 会按错误的步长走环 —— 见 cvitek_eth.c 的 _Static_assert。
 */
#define CVITEK_BUSMODE_DSL_WORDS  12U

/* ── DMA_STATUS 位域（W1C）─────────────────────────────────────────────── */

#define CVITEK_DMAST_TI           (1U << 0)   /* Transmit Interrupt           */
#define CVITEK_DMAST_RI           (1U << 6)   /* Receive Interrupt            */
#define CVITEK_DMAST_NIS          (1U << 16)  /* Normal Interrupt Summary     */

/* ── DMA_OPERATION 位域 ────────────────────────────────────────────────── */

#define CVITEK_DMAOP_SR           (1U << 1)   /* Start/Stop Receive           */
#define CVITEK_DMAOP_OSF          (1U << 2)   /* Operate on Second Frame      */
#define CVITEK_DMAOP_ST           (1U << 13)  /* Start/Stop Transmission      */
#define CVITEK_DMAOP_FTF          (1U << 20)  /* Flush TX FIFO（自清零）      */
#define CVITEK_DMAOP_TSF          (1U << 21)  /* Transmit Store-and-Forward   */
#define CVITEK_DMAOP_RSF          (1U << 25)  /* Receive Store-and-Forward    */

/* ── 描述符（normal mode，每条 16 字节逻辑内容 / 64 字节 stride）────────── */

/* TDES0 */
#define CVITEK_TDES0_OWN          (1U << 31)  /* 1 = 归 DMA，0 = 归 CPU       */

/* TDES1：控制位 + buffer-1 长度 */
#define CVITEK_TDES1_IC           (1U << 31)  /* Interrupt on Completion      */
#define CVITEK_TDES1_LS           (1U << 30)  /* Last Segment                 */
#define CVITEK_TDES1_FS           (1U << 29)  /* First Segment                */
#define CVITEK_TDES1_TER          (1U << 25)  /* Transmit End-of-Ring         */
#define CVITEK_TDES1_TBS1_MASK    0x7FFU      /* Buffer 1 size                */

/* RDES0 */
#define CVITEK_RDES0_OWN          (1U << 31)
#define CVITEK_RDES0_ES           (1U << 15)  /* Error Summary                */
#define CVITEK_RDES0_FL_SHIFT     16          /* 帧长（含 4 字节 FCS）        */
#define CVITEK_RDES0_FL_MASK      (0x3FFFU << 16)

/* RDES1 */
#define CVITEK_RDES1_RBS1_MASK    0x7FFU      /* Receive Buffer 1 size        */
#define CVITEK_RDES1_RER          (1U << 25)  /* Receive End-of-Ring          */

/*
 * RX 缓冲长度字段 RBS1 只有 11 位，上限 0x7FF = 2047，装不下 2048。
 * 用 2047 不影响功能：最长以太网帧 1518 字节，离 2047 还远。
 */
#define CVITEK_RX_BUF_DESC_LEN    2047U

/* ── 环尺寸 ────────────────────────────────────────────────────────────── */

/*
 * RX 环深度的意义是"能吸收多长的调度停顿"：本驱动是纯轮询的，环一旦填满而
 * 轮询任务还没被调度到，后续帧就直接丢。
 *
 *   1400B 载荷 @100Mbps 线速 ≈ 8525 包/秒
 *   RX 32  深 → 只能缓冲 3.6ms
 *   RX 256 深 → 可以缓冲 30ms
 *
 * 32 是在 SG2002 实测中暴露出来的：busybox shell（优先级 5，高于 net-poll 的
 * 20）和串口输出都能轻易抢占轮询任务几毫秒，3.6ms 的余量根本不够用。
 * 256 深的代价是 512KB 帧缓冲 —— 板子有 256MB，可以忽略。
 *
 * 注意：环深度和描述符步长是两件独立的事。步长恒为 64 字节（见下面的
 * CVITEK_BUSMODE_DSL_WORDS），改环深度不影响它。
 */
#define CVITEK_TX_RING_SIZE  64U
#define CVITEK_RX_RING_SIZE  256U
#define CVITEK_BUF_SIZE      2048U   /* 单个 DMA 帧缓冲大小              */

#endif /* CVITEK_ETH_REGS_H */
