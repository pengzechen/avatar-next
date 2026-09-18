/*
 * driver/eth/cvitek_eth.c — SG2002 / CV1812H 板载以太网驱动（C 实现）
 *
 * 硬件：Synopsys DesignWare MAC 3.70a（DWMAC）+ 内部 EPHY
 *       GMAC 基址来自 platform.conf 的 eth.base，sg2002-riscv64 上是 0x04070000。
 *
 * 本文件是原 Rust 驱动（driver/cvitek-eth + rust/avatar_eth FFI 桥）的 C 重写，
 * 去掉了 FFI 层，直接实现 netdev_t。初始化顺序、寄存器取值、PHY 自协商流程
 * 与 Rust 原版逐条对应。
 *
 * ── 与 Rust 原版的三个关键差异（直译会翻车的地方）────────────────────────
 *
 * 1. 描述符地址。Rust 写的是 `buf.as_ptr() as u32` —— 它在 riscv64 上"碰巧"对，
 *    因为 VA = PA + KERNEL_VMA 且 KERNEL_VMA = 0xffffffc000000000，截断到 32 位
 *    正好把整个偏移丢掉。这里必须显式走 virt_to_phys()。
 *
 * 2. 描述符 stride。DMA_BUS_MODE.DSL = 12 words（跳 48 字节）+ 描述符本身 16 字节
 *    = 64 字节步长，与 cvitek_dma_desc_t 的 64 字节对齐是**一对**，见下面的
 *    static_assert。同时 64 字节正好是 C906 的 cache line，一条描述符独占一行。
 *
 * 3. 缓存维护。Rust 有、而 virtio_net.c 没有 —— 在 C906 这种非相干 cache 的核上，
 *    缺了它就是收发出错帧。收发路径里的 clean/invalidate 都有注释标明方向。
 *
 * ── 并发假设 ────────────────────────────────────────────────────────────
 * 驱动只被 net-poll 任务访问，不加锁（与 virtio_net.c 同假设）。SG2002 的
 * RISC-V SMP bringup 本来就是关的（kernel/task/cpu.c）。
 */

#include "cvitek_eth.h"
#include "cvitek_eth_regs.h"

#include "arch.h"
#include "assert.h"
#include "barrier.h"
#include "cache.h"
#include "klog.h"
#include "kmalloc.h"
#include "mm_vm.h"          /* virt_to_phys / KERNEL_VMA */
#include "mmio.h"
#include "platform_cfg.h"   /* platform_get_mmio / g_mmio_needs_vma */
#include "string.h"
#include "net/netdev.h"
#include "timer/timer.h"

#if !ARCH_RISCV64
#error "cvitek_eth 目前仅支持 riscv64（见 Makefile §6c）"
#endif

/* ── SG2002 SoC 固定地址 ────────────────────────────────────────────────── */
/*
 * GMAC 基址是平台可配项（走 platform.conf），但 CLKGEN / RSTC 这两块是 SG2002
 * 的固定 IP 地址，只在解除 eth 时钟门控和软复位时各碰一次，不做成配置项。
 * 它们同样受平台 mmio_vma 规则约束 —— 见 cvitek_mmio_va()。
 */
#define CVITEK_CLKGEN_BASE        0x03002000UL  /* 时钟生成器           */
#define CVITEK_RSTC_BASE          0x03003000UL  /* 复位控制器           */

/* CLKGEN：bit25 = eth0 ahb 时钟，bit26 = eth0 ptpclk */
#define CVITEK_CLKGEN_CLK_EN_0    0x000U
#define CVITEK_CLKGEN_ETH_MASK    ((1U << 25) | (1U << 26))

/* RSTC：低电平有效，写 1 解除复位 */
#define CVITEK_RSTC_SOFT_RSTN_0   0x000U
#define CVITEK_RSTC_SOFT_RSTN_3   0x00CU
#define CVITEK_RSTC_ETH0_BIT      (1U << 12)  /* SOFT_RSTN_0: eth0 mac 复位  */
#define CVITEK_RSTC_EPHY_MASK     ((1U << 0) | (1U << 1))  /* SOFT_RSTN_3: ephy */

/* ── PHY ───────────────────────────────────────────────────────────────── */

#define CVITEK_PHY_ADDR           0U

#define CVITEK_PHY_REG_BMCR       0U
#define CVITEK_PHY_REG_BMSR       1U
#define CVITEK_PHY_REG_ANAR       4U
#define CVITEK_PHY_REG_LPA        5U

#define CVITEK_PHY_BMCR_RESET        (1U << 15)
#define CVITEK_PHY_BMCR_SPEED_100    (1U << 13)
#define CVITEK_PHY_BMCR_AN_ENABLE    (1U << 12)
#define CVITEK_PHY_BMCR_RESTART_AN   (1U << 9)
#define CVITEK_PHY_BMCR_FULL_DUPLEX  (1U << 8)

#define CVITEK_PHY_BMSR_LINK         (1U << 2)  /* latch-low，读两次才准 */

/* ANAR(reg4) ∩ LPA(reg5) 的协商结果位 */
#define CVITEK_PHY_AN_100FD  (1U << 8)
#define CVITEK_PHY_AN_100HD  (1U << 7)
#define CVITEK_PHY_AN_10FD   (1U << 6)
#define CVITEK_PHY_AN_10HD   (1U << 5)

/* 0x3300 = 100M + 自协商使能 + 重启自协商 + 全双工 */
#define CVITEK_PHY_BMCR_AN_100FD                                       \
    (CVITEK_PHY_BMCR_SPEED_100 | CVITEK_PHY_BMCR_AN_ENABLE |           \
     CVITEK_PHY_BMCR_RESTART_AN | CVITEK_PHY_BMCR_FULL_DUPLEX)

/* ── 超时（微秒）────────────────────────────────────────────────────────── */

#define CVITEK_MDIO_TIMEOUT_US     10000U    /* 单次 MDIO 事务          */
#define CVITEK_DMA_RESET_TIMEOUT_US 10000U   /* DMA 软复位              */
#define CVITEK_FIFO_FLUSH_TIMEOUT_US 10000U  /* TX FIFO flush           */
#define CVITEK_PHY_RESET_TIMEOUT_US 500000U  /* PHY 软复位              */
#define CVITEK_LINK_AN_TIMEOUT_US  2500000U  /* 自协商等 link up ~2.5s  */

/* EPHY 上电稳定时间 */
#define CVITEK_EPHY_SETTLE_MS      2U

/* ── 其他 ─────────────────────────────────────────────────────────────── */

#define CVITEK_MIN_ETH_FRAME       60U    /* 不含 FCS 的最小载荷，不足补零 */
#define CVITEK_GMAC_INT_DISABLE    0x60FU /* PCS/LPI 等无关中断位，保持原值 */

/* 硬件 MAC 读出来全 0 / 全 F 时使用的兜底地址 */
static const uint8_t cvitek_mac_fallback[6] = { 0x00, 0x50, 0x43, 0x02, 0x02, 0x02 };

/*
 * 四块 DMA 内存各自的页数。分配（cvitek_eth_init）和释放（cvitek_free_allocs）
 * 必须用同一组数字，所以集中在这里定义一次 —— 以前释放那边把描述符环写死成
 * 1 页，环一加深就会漏放。
 * 描述符按 64 字节步长算（见 CVITEK_BUSMODE_DSL_WORDS）。
 */
#define CVITEK_TX_DESC_PAGES  DIV_ROUND_UP(CVITEK_TX_RING_SIZE * 64U, 4096U)
#define CVITEK_RX_DESC_PAGES  DIV_ROUND_UP(CVITEK_RX_RING_SIZE * 64U, 4096U)
#define CVITEK_TX_BUF_PAGES   DIV_ROUND_UP(CVITEK_TX_RING_SIZE * CVITEK_BUF_SIZE, 4096U)
#define CVITEK_RX_BUF_PAGES   DIV_ROUND_UP(CVITEK_RX_RING_SIZE * CVITEK_BUF_SIZE, 4096U)

/* ── DMA 描述符 ────────────────────────────────────────────────────────── */

/*
 * 一条 16 字节的 normal-mode 描述符，靠 aligned(64) 把 sizeof 撑到 64。
 * 这不是为了好看：环是**步长 64 字节**的数组（DMA_BUS_MODE.DSL = 12 words
 * 让硬件在相邻描述符之间跳 48 字节），sizeof 必须是 64，元素下标 i 才能落在
 * 硬件期望的 i*64 偏移上。改这里必须同步改 CVITEK_BUSMODE_DSL_WORDS。
 */
typedef struct {
    uint32_t des0;
    uint32_t des1;
    uint32_t des2;   /* 数据缓冲物理地址 */
    uint32_t des3;
} __attribute__((aligned(64))) cvitek_dma_desc_t;

static_assert(sizeof(cvitek_dma_desc_t) == 64,
              "描述符必须占满 64 字节（DSL=12 words + 16 字节描述符 = 64 字节步长）");

/*
 * 描述符字段一律走 volatile 访问：这些字会被 DMA 在背后改写，编译器既不能
 * 缓存进寄存器，也不能把它们重排到缓存维护指令之后。
 */
#define CVITEK_DESC_RD(d, f)      (*(volatile uint32_t *)&(d)->f)
#define CVITEK_DESC_WR(d, f, v)   (*(volatile uint32_t *)&(d)->f = (uint32_t)(v))

/* ── 驱动状态 ──────────────────────────────────────────────────────────── */

struct cvitek_eth_nic {
    uintptr_t base;

    uint8_t mac[6];

    cvitek_dma_desc_t *tx_descs;
    cvitek_dma_desc_t *rx_descs;
    uint8_t           *tx_bufs;   /* TX_RING_SIZE 个连续的 BUF_SIZE 缓冲 */
    uint8_t           *rx_bufs;

    uint32_t tx_head;   /* 下一个可写的 TX 描述符             */
    uint32_t tx_tail;   /* 已发出但尚未回收的最早 TX 描述符   */
    uint32_t rx_cur;    /* 下一个期望 DMA 填完的 RX 描述符    */

    uint32_t tx_count;
    uint32_t rx_count;

    bool ready;
};

static struct cvitek_eth_nic *g_nic;

/* ── 寄存器访问 ────────────────────────────────────────────────────────── */

static inline uint32_t cvitek_read(const struct cvitek_eth_nic *nic, uint32_t off)
{
    return read32((const volatile void *)(nic->base + off));
}

static inline void cvitek_write(const struct cvitek_eth_nic *nic, uint32_t off,
                                uint32_t val)
{
    write32(val, (volatile void *)(nic->base + off));
}

/* ── 地址换算 ──────────────────────────────────────────────────────────── */

/*
 * 硬编码的 SoC 物理地址 → 内核可访问的虚拟地址。
 * sg2002-riscv64 的 platform.conf 里 mmio_vma = true，MMIO 要走 KERNEL_VMA
 * 高半别名；这个规则由 g_mmio_needs_vma 给出，和 platform_get_mmio() 一致。
 */
static inline uintptr_t cvitek_mmio_va(uintptr_t pa)
{
    return g_mmio_needs_vma ? (pa + (uintptr_t)KERNEL_VMA) : pa;
}

/*
 * 内核虚拟地址 → DMA 用的物理地址。
 * DWMAC 3.70a 只有 32 位 DMA 地址；SG2002 的 DRAM 在 0x80000000..0x8FFFFFFF，
 * 装得下。init 里对每个缓冲都做了上界检查，这里只做转换。
 */
static inline uint32_t cvitek_dma_pa(const void *va)
{
    return (uint32_t)virt_to_phys(va);
}

static inline uint8_t *cvitek_buf(uint8_t *pool, uint32_t idx)
{
    return pool + (size_t)idx * CVITEK_BUF_SIZE;
}

/* ── 超时助手 ──────────────────────────────────────────────────────────── */
/*
 * 基于 timer_read_counter()（RISC-V 的 rdtime CSR），不依赖定时器中断 ——
 * 这样在关中断的上下文里调用也不会死等。
 */

static uint64_t cvitek_deadline_us(uint32_t us)
{
    uint64_t freq = timer_counter_frequency();
    if (!freq)
        freq = 1000000ULL;
    return timer_read_counter()
         + (freq / 1000000ULL) * us
         + (freq % 1000000ULL) * us / 1000000ULL;
}

static inline bool cvitek_expired(uint64_t deadline)
{
    return (int64_t)(timer_read_counter() - deadline) >= 0;
}

/* ── 缓存维护 ──────────────────────────────────────────────────────────── */

static inline void cvitek_clean_desc(const cvitek_dma_desc_t *d)
{
    clean_dcache_range(d, sizeof(*d));
}

static inline void cvitek_inval_desc(const cvitek_dma_desc_t *d)
{
    invalidate_dcache_range(d, sizeof(*d));
}

/* ── 时钟 / 复位 ───────────────────────────────────────────────────────── */

static void cvitek_clk_and_reset_enable(void)
{
    volatile uint32_t *clk_en0 =
        (volatile uint32_t *)cvitek_mmio_va(CVITEK_CLKGEN_BASE + CVITEK_CLKGEN_CLK_EN_0);
    volatile uint32_t *soft_rstn_0 =
        (volatile uint32_t *)cvitek_mmio_va(CVITEK_RSTC_BASE + CVITEK_RSTC_SOFT_RSTN_0);
    volatile uint32_t *soft_rstn_3 =
        (volatile uint32_t *)cvitek_mmio_va(CVITEK_RSTC_BASE + CVITEK_RSTC_SOFT_RSTN_3);

    /* 放开 eth0 时钟门控 */
    write32(read32(clk_en0) | CVITEK_CLKGEN_ETH_MASK, clk_en0);

    /* 解除 ETH0 MAC 软复位（SOFT_RSTN_0 bit12） */
    write32(read32(soft_rstn_0) | CVITEK_RSTC_ETH0_BIT, soft_rstn_0);

    /* 解除 EPHY 相关复位（SOFT_RSTN_3 bit0/1） */
    write32(read32(soft_rstn_3) | CVITEK_RSTC_EPHY_MASK, soft_rstn_3);

    /* 等 EPHY 稳定 */
    timer_delay_ms(CVITEK_EPHY_SETTLE_MS);
}

/* ── MDIO（IEEE 802.3 clause 22）───────────────────────────────────────── */

static int cvitek_mdio_wait(const struct cvitek_eth_nic *nic)
{
    uint64_t deadline = cvitek_deadline_us(CVITEK_MDIO_TIMEOUT_US);

    while (cvitek_read(nic, CVITEK_GMAC_MII_ADDR) & CVITEK_MIIADDR_BUSY) {
        if (cvitek_expired(deadline)) {
            KLOG_WARN("[cvitek-eth] MDIO busy timeout\n");
            return -1;
        }
    }
    return 0;
}

static int cvitek_mdio_read(const struct cvitek_eth_nic *nic, uint32_t phy,
                            uint32_t reg, uint16_t *out)
{
    if (cvitek_mdio_wait(nic) != 0)
        return -1;

    /*
     * MII_ADDR 是「写进去就生效」的整字寄存器，PHY/REG/CLK_CSR/BUSY 必须一次拼好，
     * 不能用读改写 —— 读回的值带 BUSY，写回去会重复触发事务。
     */
    cvitek_write(nic, CVITEK_GMAC_MII_ADDR,
                 ((phy << CVITEK_MIIADDR_PHY_SHIFT) & CVITEK_MIIADDR_PHY_MASK) |
                 ((reg << CVITEK_MIIADDR_REG_SHIFT) & CVITEK_MIIADDR_REG_MASK) |
                 (CVITEK_MII_CLK_CSR_60_100M_DIV42 << CVITEK_MIIADDR_CLK_CSR_SHIFT) |
                 CVITEK_MIIADDR_BUSY);

    if (cvitek_mdio_wait(nic) != 0)
        return -1;

    *out = (uint16_t)(cvitek_read(nic, CVITEK_GMAC_MII_DATA) & 0xFFFFU);
    return 0;
}

static int cvitek_mdio_write(const struct cvitek_eth_nic *nic, uint32_t phy,
                             uint32_t reg, uint16_t val)
{
    if (cvitek_mdio_wait(nic) != 0)
        return -1;

    cvitek_write(nic, CVITEK_GMAC_MII_DATA, val);
    cvitek_write(nic, CVITEK_GMAC_MII_ADDR,
                 ((phy << CVITEK_MIIADDR_PHY_SHIFT) & CVITEK_MIIADDR_PHY_MASK) |
                 ((reg << CVITEK_MIIADDR_REG_SHIFT) & CVITEK_MIIADDR_REG_MASK) |
                 (CVITEK_MII_CLK_CSR_60_100M_DIV42 << CVITEK_MIIADDR_CLK_CSR_SHIFT) |
                 CVITEK_MIIADDR_WRITE | CVITEK_MIIADDR_BUSY);

    return cvitek_mdio_wait(nic);
}

/* ── DMA ───────────────────────────────────────────────────────────────── */

static void cvitek_dma_reset(const struct cvitek_eth_nic *nic)
{
    cvitek_write(nic, CVITEK_DMA_BUS_MODE, CVITEK_BUSMODE_SWR);

    uint64_t deadline = cvitek_deadline_us(CVITEK_DMA_RESET_TIMEOUT_US);
    while (cvitek_read(nic, CVITEK_DMA_BUS_MODE) & CVITEK_BUSMODE_SWR) {
        if (cvitek_expired(deadline)) {
            KLOG_WARN("[cvitek-eth] DMA soft reset timeout\n");
            break;
        }
    }
}

static void cvitek_setup_tx_ring(struct cvitek_eth_nic *nic)
{
    for (uint32_t i = 0; i < CVITEK_TX_RING_SIZE; i++) {
        cvitek_dma_desc_t *d = &nic->tx_descs[i];

        CVITEK_DESC_WR(d, des0, 0);
        CVITEK_DESC_WR(d, des1,
                       (i == CVITEK_TX_RING_SIZE - 1) ? CVITEK_TDES1_TER : 0);
        CVITEK_DESC_WR(d, des2, cvitek_dma_pa(cvitek_buf(nic->tx_bufs, i)));
        CVITEK_DESC_WR(d, des3, 0);
        cvitek_clean_desc(d);
    }
}

static void cvitek_setup_rx_ring(struct cvitek_eth_nic *nic)
{
    for (uint32_t i = 0; i < CVITEK_RX_RING_SIZE; i++) {
        cvitek_dma_desc_t *d = &nic->rx_descs[i];
        uint32_t rdes1 = CVITEK_RX_BUF_DESC_LEN;

        if (i == CVITEK_RX_RING_SIZE - 1)
            rdes1 |= CVITEK_RDES1_RER;

        CVITEK_DESC_WR(d, des2, cvitek_dma_pa(cvitek_buf(nic->rx_bufs, i)));
        CVITEK_DESC_WR(d, des1, rdes1);
        CVITEK_DESC_WR(d, des3, 0);
        wmb();
        CVITEK_DESC_WR(d, des0, CVITEK_RDES0_OWN);
        cvitek_clean_desc(d);
    }
}

/* 把 RX 描述符还给 DMA（读完一帧后调用） */
static void cvitek_requeue_rx(struct cvitek_eth_nic *nic, uint32_t slot)
{
    cvitek_dma_desc_t *d = &nic->rx_descs[slot];
    uint32_t rdes1 = CVITEK_RX_BUF_DESC_LEN;

    if (slot == CVITEK_RX_RING_SIZE - 1)
        rdes1 |= CVITEK_RDES1_RER;

    CVITEK_DESC_WR(d, des2, cvitek_dma_pa(cvitek_buf(nic->rx_bufs, slot)));
    CVITEK_DESC_WR(d, des1, rdes1);
    CVITEK_DESC_WR(d, des3, 0);
    wmb();
    CVITEK_DESC_WR(d, des0, CVITEK_RDES0_OWN);
    cvitek_clean_desc(d);

    /* 清 RI/NIS（W1C）并催一次 RX 轮询 */
    cvitek_write(nic, CVITEK_DMA_STATUS, CVITEK_DMAST_RI | CVITEK_DMAST_NIS);
    cvitek_write(nic, CVITEK_DMA_RX_POLL, 1);
}

/* 回收已被 DMA 发完的 TX 描述符 */
static void cvitek_reclaim_tx(struct cvitek_eth_nic *nic)
{
    while (nic->tx_tail != nic->tx_head) {
        cvitek_dma_desc_t *d = &nic->tx_descs[nic->tx_tail];

        cvitek_inval_desc(d);
        if (CVITEK_DESC_RD(d, des0) & CVITEK_TDES0_OWN)
            break;   /* DMA 还没发完 */

        nic->tx_tail = (nic->tx_tail + 1) % CVITEK_TX_RING_SIZE;
    }
}

/* ── PHY ───────────────────────────────────────────────────────────────── */

/* timer_poll_until_us 的谓词：BMSR bit2 是 latch-low，得连读两次 */
static bool cvitek_link_up_pred(void *ctx)
{
    struct cvitek_eth_nic *nic = (struct cvitek_eth_nic *)ctx;
    uint16_t bmsr = 0;

    (void)cvitek_mdio_read(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_BMSR, &bmsr);
    if (cvitek_mdio_read(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_BMSR, &bmsr) != 0)
        return false;

    return (bmsr & CVITEK_PHY_BMSR_LINK) != 0;
}

static void cvitek_phy_init(struct cvitek_eth_nic *nic, bool *speed_100m,
                            bool *full_duplex)
{
    uint16_t bmcr = 0, bmsr = 0, lpa = 0, anar = 0;
    uint64_t deadline;

    /* 1. 软复位 PHY */
    if (cvitek_mdio_write(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_BMCR,
                          CVITEK_PHY_BMCR_RESET) != 0)
        KLOG_WARN("[cvitek-eth] PHY reset write failed\n");

    deadline = cvitek_deadline_us(CVITEK_PHY_RESET_TIMEOUT_US);
    while ((cvitek_read(nic, CVITEK_GMAC_MII_ADDR) & CVITEK_MIIADDR_BUSY) ||
           ((cvitek_mdio_read(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_BMCR, &bmcr) == 0) &&
            (bmcr & CVITEK_PHY_BMCR_RESET))) {
        if (cvitek_expired(deadline)) {
            KLOG_WARN("[cvitek-eth] PHY reset timeout (bmcr=0x%04x)\n", bmcr);
            break;
        }
    }

    /* 2. 启动自协商：100M 全双工 */
    if (cvitek_mdio_write(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_BMCR,
                          CVITEK_PHY_BMCR_AN_100FD) != 0)
        KLOG_WARN("[cvitek-eth] PHY AN start failed\n");

    /* 3. 等 link up，与 Rust 原版一样上限 ~2.5s */
    int rc = timer_poll_until_us(cvitek_link_up_pred, nic,
                                 CVITEK_LINK_AN_TIMEOUT_US, 2000U);

    (void)cvitek_mdio_read(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_BMSR, &bmsr);
    (void)cvitek_mdio_read(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_BMSR, &bmsr);
    (void)cvitek_mdio_read(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_BMCR, &bmcr);
    (void)cvitek_mdio_read(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_LPA, &lpa);

    if (rc == 0)
        KLOG_INFO("[cvitek-eth] link UP bmcr=0x%04x bmsr=0x%04x lpa=0x%04x\n",
                  bmcr, bmsr, lpa);
    else
        KLOG_WARN("[cvitek-eth] link still DOWN after AN timeout, continuing "
                  "(bmcr=0x%04x bmsr=0x%04x lpa=0x%04x)\n", bmcr, bmsr, lpa);

    /* 4. 由 ANAR ∩ LPA 定速率/双工 */
    (void)cvitek_mdio_read(nic, CVITEK_PHY_ADDR, CVITEK_PHY_REG_ANAR, &anar);
    uint16_t common = (uint16_t)(anar & lpa);

    if (common & CVITEK_PHY_AN_100FD) {
        *speed_100m = true;  *full_duplex = true;
    } else if (common & CVITEK_PHY_AN_100HD) {
        *speed_100m = true;  *full_duplex = false;
    } else if (common & CVITEK_PHY_AN_10FD) {
        *speed_100m = false; *full_duplex = true;
    } else if (common & CVITEK_PHY_AN_10HD) {
        *speed_100m = false; *full_duplex = false;
    } else {
        /* 协商结果无法确定：Rust 原版这里无条件落 100M FD，保持一致 */
        KLOG_WARN("[cvitek-eth] AN result indeterminate, assuming 100M FD\n");
        *speed_100m = true;  *full_duplex = true;
    }

    KLOG_INFO("[cvitek-eth] link mode = %s %s (anar=0x%04x lpa=0x%04x)\n",
              *speed_100m ? "100M" : "10M",
              *full_duplex ? "FD" : "HD", anar, lpa);
}

/* ── MAC 地址 ──────────────────────────────────────────────────────────── */

static void cvitek_read_mac(const struct cvitek_eth_nic *nic, uint8_t mac[6])
{
    uint32_t lo = cvitek_read(nic, CVITEK_GMAC_ADDR0_LOW);
    uint32_t hi = cvitek_read(nic, CVITEK_GMAC_ADDR0_HIGH);

    mac[0] = (uint8_t)(lo & 0xFF);
    mac[1] = (uint8_t)((lo >> 8) & 0xFF);
    mac[2] = (uint8_t)((lo >> 16) & 0xFF);
    mac[3] = (uint8_t)((lo >> 24) & 0xFF);
    mac[4] = (uint8_t)(hi & 0xFF);
    mac[5] = (uint8_t)((hi >> 8) & 0xFF);
}

static void cvitek_write_mac(const struct cvitek_eth_nic *nic, const uint8_t mac[6])
{
    uint32_t lo = (uint32_t)mac[0]
                | ((uint32_t)mac[1] << 8)
                | ((uint32_t)mac[2] << 16)
                | ((uint32_t)mac[3] << 24);
    uint32_t hi = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);

    cvitek_write(nic, CVITEK_GMAC_ADDR0_LOW, lo);
    /* bit31 是 AE（地址使能），必须置 1 */
    cvitek_write(nic, CVITEK_GMAC_ADDR0_HIGH,
                 (hi & CVITEK_ADDR0HI_MASK) | CVITEK_ADDR0HI_ENABLE);
}

/* ── 收发 ──────────────────────────────────────────────────────────────── */

/* 返回 0 成功，-1 描述符忙（让调用方下轮重试）/ 参数错 */
static int cvitek_eth_send(struct cvitek_eth_nic *nic, const uint8_t *frame,
                           size_t len)
{
    if (!nic || !nic->ready || !frame)
        return -1;
    if (len > CVITEK_BUF_SIZE)
        return -1;

    cvitek_reclaim_tx(nic);

    uint32_t idx = nic->tx_head;
    cvitek_dma_desc_t *d = &nic->tx_descs[idx];

    cvitek_inval_desc(d);
    if (CVITEK_DESC_RD(d, des0) & CVITEK_TDES0_OWN)
        return -1;   /* 环满，DMA 还没回收 */

    uint8_t *buf = cvitek_buf(nic->tx_bufs, idx);
    size_t tx_len = (len < CVITEK_MIN_ETH_FRAME) ? CVITEK_MIN_ETH_FRAME : len;

    memcpy(buf, frame, len);
    if (tx_len > len)
        memset(buf + len, 0, tx_len - len);

    /* CPU 写 → DMA 读：先把帧数据推到内存，再交给 DMA */
    clean_dcache_range(buf, tx_len);

    uint32_t tdes1 = CVITEK_TDES1_IC | CVITEK_TDES1_FS | CVITEK_TDES1_LS
                   | ((uint32_t)tx_len & CVITEK_TDES1_TBS1_MASK);
    if (idx == CVITEK_TX_RING_SIZE - 1)
        tdes1 |= CVITEK_TDES1_TER;

    CVITEK_DESC_WR(d, des2, cvitek_dma_pa(buf));
    CVITEK_DESC_WR(d, des1, tdes1);
    CVITEK_DESC_WR(d, des3, 0);
    wmb();
    /* 最后写 OWN，把描述符交给 DMA；再 clean 一次让 OWN 确实落到内存 */
    CVITEK_DESC_WR(d, des0, CVITEK_TDES0_OWN);
    cvitek_clean_desc(d);

    nic->tx_count++;
    nic->tx_head = (idx + 1) % CVITEK_TX_RING_SIZE;

    cvitek_write(nic, CVITEK_DMA_TX_POLL, 1);
    return 0;
}

/* 返回帧长（>0）；无帧返回 0；错误返回 -1。与 Rust eth_recv 语义一致 */
static int cvitek_eth_recv(struct cvitek_eth_nic *nic, uint8_t *frame,
                           size_t maxlen)
{
    if (!nic || !nic->ready || !frame)
        return -1;

    uint32_t idx = nic->rx_cur;
    cvitek_dma_desc_t *d = &nic->rx_descs[idx];

    /* DMA 写 → CPU 读：先把描述符从内存捞回来 */
    cvitek_inval_desc(d);
    uint32_t des0 = CVITEK_DESC_RD(d, des0);

    if (des0 & CVITEK_RDES0_OWN)
        return 0;   /* DMA 还没填完，本轮无帧 */

    if (des0 & CVITEK_RDES0_ES) {
        /* 坏帧：还回去，跳过 */
        cvitek_requeue_rx(nic, idx);
        nic->rx_cur = (idx + 1) % CVITEK_RX_RING_SIZE;
        return 0;
    }

    /* RDES0.FL 含 4 字节 FCS */
    uint32_t flen = (des0 & CVITEK_RDES0_FL_MASK) >> CVITEK_RDES0_FL_SHIFT;
    flen = (flen >= 4U) ? (flen - 4U) : flen;
    if (flen > CVITEK_BUF_SIZE)
        flen = CVITEK_BUF_SIZE;

    uint8_t *buf = cvitek_buf(nic->rx_bufs, idx);

    /* DMA 写 → CPU 读：把帧数据从内存捞回来 */
    invalidate_dcache_range(buf, flen);

    nic->rx_cur = (idx + 1) % CVITEK_RX_RING_SIZE;
    nic->rx_count++;

    size_t n = ((size_t)flen < maxlen) ? (size_t)flen : maxlen;
    memcpy(frame, buf, n);

    /* 立刻还给 DMA（Rust 版靠 RxToken::drop 做，C 里手动） */
    cvitek_requeue_rx(nic, idx);

    return (int)n;
}

/* ── netdev 接入 ───────────────────────────────────────────────────────── */

static int cvitek_netdev_send(void *ctx, const uint8_t *frame, size_t len)
{
    return cvitek_eth_send((struct cvitek_eth_nic *)ctx, frame, len);
}

static int cvitek_netdev_recv(void *ctx, uint8_t *frame, size_t maxlen)
{
    return cvitek_eth_recv((struct cvitek_eth_nic *)ctx, frame, maxlen);
}

/* ── 初始化 ────────────────────────────────────────────────────────────── */

static void cvitek_free_allocs(struct cvitek_eth_nic *nic)
{
    if (nic->rx_bufs)
        kfree_pages(nic->rx_bufs, CVITEK_RX_BUF_PAGES);
    if (nic->tx_bufs)
        kfree_pages(nic->tx_bufs, CVITEK_TX_BUF_PAGES);
    if (nic->rx_descs)
        kfree_pages((void *)nic->rx_descs, CVITEK_RX_DESC_PAGES);
    if (nic->tx_descs)
        kfree_pages((void *)nic->tx_descs, CVITEK_TX_DESC_PAGES);
    kfree(nic, sizeof(*nic));
}

static int cvitek_eth_init(uintptr_t base)
{
    struct cvitek_eth_nic *nic;

    if (base == 0) {
        KLOG_WARN("[cvitek-eth] no eth.base in platform config, skip init\n");
        return -1;
    }

    nic = kmalloc(sizeof(*nic));
    if (!nic) {
        KLOG_ERROR("[cvitek-eth] kmalloc(nic) failed\n");
        return -1;
    }

    /*
     * 环和帧缓冲都要求「物理连续 + 64 字节对齐 + PA 落在 32 位内」：
     * kalloc_pages 返回页对齐、已清零、物理连续的内存，三条都满足，
     * 而且比 Rust 版逐个 Box::new 分配 64 次更省事。
     *
     * RX 帧缓冲是 512KB（128 页）的连续请求，而 PMM 那边是线性扫描 bitmap
     * 找连续块 —— 碎片化时有可能失败，所以失败时报清楚是哪一块、要多少页。
     */
    nic->tx_descs = kalloc_pages(CVITEK_TX_DESC_PAGES);
    nic->rx_descs = kalloc_pages(CVITEK_RX_DESC_PAGES);
    nic->tx_bufs  = kalloc_pages(CVITEK_TX_BUF_PAGES);
    nic->rx_bufs  = kalloc_pages(CVITEK_RX_BUF_PAGES);

    if (!nic->tx_descs || !nic->rx_descs || !nic->tx_bufs || !nic->rx_bufs) {
        KLOG_ERROR("[cvitek-eth] DMA 分配失败：需要的页数 desc tx=%u rx=%u, "
                   "buf tx=%u rx=%u（拿到 desc tx=%d rx=%d, buf tx=%d rx=%d）\n",
                   CVITEK_TX_DESC_PAGES, CVITEK_RX_DESC_PAGES,
                   CVITEK_TX_BUF_PAGES, CVITEK_RX_BUF_PAGES,
                   nic->tx_descs != NULL, nic->rx_descs != NULL,
                   nic->tx_bufs != NULL, nic->rx_bufs != NULL);
        cvitek_free_allocs(nic);
        return -1;
    }

    /* DWMAC 只有 32 位 DMA 地址，缓冲的物理地址必须落在 4GB 以内 */
    if (virt_to_phys(nic->tx_descs) > 0xFFFFFFFFULL ||
        virt_to_phys(nic->rx_bufs) > 0xFFFFFFFFULL) {
        KLOG_ERROR("[cvitek-eth] DMA buffer PA above 4GB, unusable\n");
        cvitek_free_allocs(nic);
        return -1;
    }

    nic->base = base;

    KLOG_INFO("[cvitek-eth] DWMAC version 0x%08x base=0x%lx\n",
              cvitek_read(nic, CVITEK_GMAC_VERSION), (unsigned long)base);

    /* 读硬件 MAC；全 0 / 全 F 说明没烧，用兜底地址 */
    cvitek_read_mac(nic, nic->mac);
    {
        bool all_zero = true, all_ff = true;
        for (int i = 0; i < 6; i++) {
            if (nic->mac[i] != 0x00) all_zero = false;
            if (nic->mac[i] != 0xFF) all_ff = false;
        }
        if (all_zero || all_ff)
            memcpy(nic->mac, cvitek_mac_fallback, 6);
    }
    KLOG_INFO("[cvitek-eth] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
              nic->mac[0], nic->mac[1], nic->mac[2],
              nic->mac[3], nic->mac[4], nic->mac[5]);

    /* 时钟 + 复位 */
    cvitek_clk_and_reset_enable();

    /* DMA 软复位 */
    cvitek_dma_reset(nic);

    /* 建立描述符环 */
    cvitek_setup_tx_ring(nic);
    cvitek_setup_rx_ring(nic);

    /* 总线模式：PBL=8, DSL=12（64 字节步长）, FB=1, AAL=1 */
    cvitek_write(nic, CVITEK_DMA_BUS_MODE,
                 (8U << CVITEK_BUSMODE_PBL_SHIFT) |
                 ((uint32_t)CVITEK_BUSMODE_DSL_WORDS << CVITEK_BUSMODE_DSL_SHIFT) |
                 CVITEK_BUSMODE_FB | CVITEK_BUSMODE_AAL);
    cvitek_write(nic, CVITEK_DMA_TX_BASE, cvitek_dma_pa(nic->tx_descs));
    cvitek_write(nic, CVITEK_DMA_RX_BASE, cvitek_dma_pa(nic->rx_descs));

    /* 纯轮询：GMAC 侧屏蔽无关中断，DMA 侧中断全关 */
    cvitek_write(nic, CVITEK_GMAC_INT_MASK, CVITEK_GMAC_INT_DISABLE);
    cvitek_write(nic, CVITEK_DMA_INTR_ENA, 0);

    cvitek_write_mac(nic, nic->mac);

    /* 收所有帧（Promiscuous + ReceiveAll + hash 全 1） */
    cvitek_write(nic, CVITEK_GMAC_FRAME_FILTER, CVITEK_FILTER_PR | CVITEK_FILTER_RA);
    cvitek_write(nic, CVITEK_GMAC_HASH_HIGH, 0xFFFFFFFFU);
    cvitek_write(nic, CVITEK_GMAC_HASH_LOW, 0xFFFFFFFFU);

    /* PHY 自协商 */
    bool speed_100m = true, full_duplex = true;
    cvitek_phy_init(nic, &speed_100m, &full_duplex);

    /* MAC 配置 */
    uint32_t mc = CVITEK_MACCTL_PS | CVITEK_MACCTL_TE | CVITEK_MACCTL_RE;
    if (speed_100m)
        mc |= CVITEK_MACCTL_FES;
    if (full_duplex)
        mc |= CVITEK_MACCTL_DM;
    cvitek_write(nic, CVITEK_GMAC_MAC_CONTROL, mc);
    cvitek_write(nic, CVITEK_GMAC_MMC_CNTRL, 0x01U);   /* 冻结 MMC 计数器 */

    /* 启动 DMA：先 flush TX FIFO 并等它自清零，再开 ST/SR */
    cvitek_write(nic, CVITEK_DMA_OPERATION,
                 CVITEK_DMAOP_TSF | CVITEK_DMAOP_RSF |
                 CVITEK_DMAOP_OSF | CVITEK_DMAOP_FTF);

    uint64_t deadline = cvitek_deadline_us(CVITEK_FIFO_FLUSH_TIMEOUT_US);
    while (cvitek_read(nic, CVITEK_DMA_OPERATION) & CVITEK_DMAOP_FTF) {
        if (cvitek_expired(deadline)) {
            KLOG_WARN("[cvitek-eth] TX FIFO flush timeout\n");
            break;
        }
    }

    uint32_t op = cvitek_read(nic, CVITEK_DMA_OPERATION);
    cvitek_write(nic, CVITEK_DMA_OPERATION, op | CVITEK_DMAOP_ST | CVITEK_DMAOP_SR);
    cvitek_write(nic, CVITEK_DMA_RX_POLL, 1);

    KLOG_DEBUG("[cvitek-eth] mac_ctl=0x%08x frame_filter=0x%08x "
               "addr0_hi=0x%08x addr0_lo=0x%08x\n",
               cvitek_read(nic, CVITEK_GMAC_MAC_CONTROL),
               cvitek_read(nic, CVITEK_GMAC_FRAME_FILTER),
               cvitek_read(nic, CVITEK_GMAC_ADDR0_HIGH),
               cvitek_read(nic, CVITEK_GMAC_ADDR0_LOW));
    KLOG_DEBUG("[cvitek-eth] dma_bus=0x%08x dma_op=0x%08x dma_status=0x%08x "
               "tx_base=0x%08x rx_base=0x%08x\n",
               cvitek_read(nic, CVITEK_DMA_BUS_MODE),
               cvitek_read(nic, CVITEK_DMA_OPERATION),
               cvitek_read(nic, CVITEK_DMA_STATUS),
               cvitek_read(nic, CVITEK_DMA_TX_BASE),
               cvitek_read(nic, CVITEK_DMA_RX_BASE));

    nic->ready = true;
    g_nic = nic;

    /* 注册到 netdev（注意 netdev_register 是单例，再注册会覆盖前一个） */
    {
        static netdev_t cvitek_dev;
        memset(&cvitek_dev, 0, sizeof(cvitek_dev));
        cvitek_dev.name = "cvitek0";
        cvitek_dev.ctx  = nic;
        cvitek_dev.send = cvitek_netdev_send;
        cvitek_dev.recv = cvitek_netdev_recv;
        memcpy(cvitek_dev.mac, nic->mac, 6);
        netdev_register(&cvitek_dev);
    }

    KLOG_INFO("[cvitek-eth] initialized OK\n");
    return 0;
}

void cvitek_eth_init_from_platform(void)
{
    /* platform.conf 的 eth.base；sg2002 上 mmio_vma=true，这里拿到的是高半别名 */
    uintptr_t base = platform_get_mmio("eth", "base");

    KLOG_DEBUG("[cvitek-eth] platform eth.base=0x%lx mmio_vma=%d\n",
               (unsigned long)base, g_mmio_needs_vma);

    cvitek_eth_init(base);
}

void cvitek_eth_send_probe(void)
{
    if (!g_nic || !g_nic->ready) {
        kprintf("[cvitek-eth] probe: NIC not ready\n");
        return;
    }

    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    memset(&pkt[0], 0xFF, 6);            /* 目的地址：广播 */
    memcpy(&pkt[6], g_nic->mac, 6);      /* 源地址：本机 MAC */
    pkt[12] = 0x88U;                     /* 自定义 EtherType */
    pkt[13] = 0xB5U;
    {
        const char payload[] = "avatar cvitek-eth probe";
        memcpy(&pkt[14], payload, sizeof(payload) - 1U);
    }

    int rc = cvitek_eth_send(g_nic, pkt, sizeof(pkt));

    /*
     * 探针是显式调用的 bring-up 工具，输出走 kprintf 而不是 KLOG_INFO：
     * 上板排障时常用 LOG=none 构建（热路径日志会污染性能测量），而 KLOG_*
     * 在 LOG=none 下被编译期抹除，用它会让探针变成一声不吭的空操作。
     */
    kprintf("[cvitek-eth] probe tx rc=%d len=%zu tx_count=%u rx_count=%u\n",
            rc, sizeof(pkt), g_nic->tx_count, g_nic->rx_count);
}
