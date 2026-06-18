/*
 * driver/blk/sdblk.c — SG2002 / CV1811 SDMMC (SDHCI) 块设备驱动
 * 设计：
 *   - 纯 PIO 轮询模式（无 DMA / 无中断）
 *   - 目标平台：SOPHGO SG2002 / CV1811H，SDIO0 控制器
 *   - 与 SDHCI 规范基本兼容
 *
 * 平台硬件地址由 platforms/<platform>/platform.lua 的 sdmmc 表提供，
 * 在 sdblk_init() 中通过 platform_get_mmio() 运行时读取。
 */

#include "blk/sdblk.h"
#include "platform_cfg.h"   /* platform_get_mmio / platform_get_uintptr */

#include "types.h"
#include "mmio.h"
#include "klog.h"
#include "timer/timer.h"

/* ── SDMMC 寄存器偏移（相对 sd_base） ───────────────────────── */
#define R_SDMA_SYSADDR   0x00
#define R_BLK_SIZE_CNT   0x04   /* [31:16]=BLK_CNT  [11:0]=XFER_BLK_SIZE */
#define R_ARG1           0x08
#define R_XFER_MODE_CMD  0x0C   /* [31:16]=CMD  [15:0]=XFER_MODE */
#define R_RESP0          0x10
#define R_RESP1          0x14
#define R_RESP2          0x18
#define R_RESP3          0x1C
#define R_BUF_DATA       0x20
#define R_PSTATE         0x24   /* Present State */
#define R_HOST_CTL1      0x28   /* Host Control 1 + Power + Bgap + Wakeup */
#define R_CLK_CTL        0x2C   /* Clock Control + Timeout */
#define R_INT_STS        0x30   /* Normal/Error Interrupt Status */
#define R_INT_STS_EN     0x34
#define R_INT_SIG_EN     0x38
#define R_HOST_CTL2      0x3C   /* Auto CMD Error + Host Control 2 */
#define R_CAP1           0x40
#define R_CAP2           0x44

/* ── Present State 位域 ──────────────────────────────────────── */
#define PSTATE_CMD_INHIBIT    (1U << 0)
#define PSTATE_DAT_INHIBIT    (1U << 1)
#define PSTATE_CARD_INSERTED  (1U << 16)

/* ── Host Control 1 / Power 位域 ────────────────────────────── */
#define HCTL_DAT_4BIT   (1U << 1)
#define HCTL_HS_EN      (1U << 2)
#define HCTL_SD_BUS_PWR (1U << 8)
#define HCTL_VOL_1V8    (5U << 9)   /* 101b = 1.8 V */
#define HCTL_VOL_3V0    (6U << 9)   /* 110b = 3.0 V */
#define HCTL_VOL_3V3    (7U << 9)   /* 111b = 3.3 V */
#define HCTL_VOL_MASK   (7U << 9)

/* ── Clock Control 位域 ──────────────────────────────────────── */
#define CLK_INT_CLK_EN        (1U << 0)
#define CLK_INT_STABLE        (1U << 1)
#define CLK_SD_CLK_EN         (1U << 2)
#define CLK_FREQ_SEL_SHIFT    8
#define CLK_FREQ_SEL_MASK     (0xFFU << 8)
#define CLK_TOUT_MASK         (0xFU << 16)
#define CLK_TOUT_MAX          (14U << 16)  /* TMCLK × 2^27，最大超时 */
#define CLK_SW_RST_ALL        (1U << 24)
#define CLK_SW_RST_CMD        (1U << 25)
#define CLK_SW_RST_DAT        (1U << 26)

/* ── Interrupt Status 位域 ────────────────────────────────────── */
#define INT_CMD_CMPL   (1U << 0)
#define INT_XFER_CMPL  (1U << 1)
#define INT_BUF_WRDY   (1U << 4)
#define INT_BUF_RRDY   (1U << 5)
#define INT_ERR        (1U << 15)
#define INT_CLEAR_ALL  0xF3FFFFFFU

/* ── Transfer Mode / Command 位域（写入 R_XFER_MODE_CMD） ──── */
#define XFER_DMA_EN      (1U << 0)
#define XFER_BLK_CNT_EN  (1U << 1)
#define XFER_AUTOCMD12   (1U << 2)    /* AUTO_CMD_EN = 01b */
#define XFER_DIR_READ    (1U << 4)    /* 1=read(card→host), 0=write */
#define XFER_MULTI_BLK   (1U << 5)
#define CMD_RESP_NONE    (0U << 16)
#define CMD_RESP_136     (1U << 16)   /* R2 */
#define CMD_RESP_48      (2U << 16)   /* R1/R3/R6/R7 */
#define CMD_RESP_48BUSY  (3U << 16)   /* R1b */
#define CMD_CRC_EN       (1U << 19)
#define CMD_IDX_EN       (1U << 20)
#define CMD_DATA_PRESENT (1U << 21)
#define CMD_IDX(n)       (((uint32_t)(n) & 0x3FU) << 24)

/* ── 常用命令字辅助宏 ─────────────────────────────────────────── */
#define CMDW_NONE(n) (CMD_IDX(n) | CMD_RESP_NONE)
#define CMDW_R1(n)   (CMD_IDX(n) | CMD_RESP_48    | CMD_CRC_EN | CMD_IDX_EN)
#define CMDW_R1B(n)  (CMD_IDX(n) | CMD_RESP_48BUSY| CMD_CRC_EN | CMD_IDX_EN)
#define CMDW_R2(n)   (CMD_IDX(n) | CMD_RESP_136   | CMD_CRC_EN)
#define CMDW_R3(n)   (CMD_IDX(n) | CMD_RESP_48)   /* OCR: 无 CRC/IDX 检查 */

/* ── Block Size/Count 寄存器辅助 ─────────────────────────────── */
#define BLKSZ_512  0x200U
#define BLKSZ_CNT(sz, cnt) ((uint32_t)(sz) | ((uint32_t)(cnt) << 16))

/* ── 驱动状态（前置声明，MMIO 助手需要访问） ─────────────────── */
static struct {
    uintptr_t sd_base;        /* platform.sdmmc.sd_base  （SDHCI 寄存器基址） */
    uintptr_t top_base;       /* platform.sdmmc.top_base （TOP 控制模块基址） */
    uint32_t  top_off_pwrsw;  /* platform.sdmmc.top_off_pwrsw_ctrl 偏移 */
    uint32_t rca;           /* 相对卡地址（高 16 位有效） */
    uint8_t  csd_structure; /* 0=v1.0(SDSC), 1=v2.0(SDHC/SDXC), 2=v3.0(SDUC) */
    uint64_t capacity;      /* 卡容量（字节） */
    bool     initialized;
} g_sdblk;

/* ── MMIO 助手（运行时地址，来自 g_sdblk.sd_base / top_base）── */
static inline uint32_t sd_rd(uint32_t off)
{
    return read32((const volatile void *)(g_sdblk.sd_base + off));
}

static inline void sd_wr(uint32_t off, uint32_t v)
{
    write32(v, (volatile void *)(g_sdblk.sd_base + off));
}

static inline void sd_set(uint32_t off, uint32_t mask)
{
    sd_wr(off, sd_rd(off) | mask);
}

static inline void sd_clr(uint32_t off, uint32_t mask)
{
    sd_wr(off, sd_rd(off) & ~mask);
}

static inline void sd_mod(uint32_t off, uint32_t mask, uint32_t val)
{
    sd_wr(off, (sd_rd(off) & ~mask) | (val & mask));
}

static inline void top_wr(uint32_t off, uint32_t v)
{
    write32(v, (volatile void *)(g_sdblk.top_base + off));
}

/* 快速 PIO 读写（热路径，不需要额外 dsb） */
static inline uint32_t sd_rd_pio(uint32_t off)
{
    return read32_relaxed((const volatile void *)(g_sdblk.sd_base + off));
}

static inline void sd_wr_pio(uint32_t off, uint32_t v)
{
    write32_relaxed(v, (volatile void *)(g_sdblk.sd_base + off));
}

/* ── 延迟（NOP 循环） ─────────────────────────────────────────── */
static void sd_delay(volatile uint32_t n)
{
    timer_spin(n);
}

#define SD_DELAY_SHORT()  sd_delay(0x10U)
#define SD_DELAY_LONG()   sd_delay(0x100000U)

/* ════════════════════════════════════════════════════════════════
 * 内部函数
 * ════════════════════════════════════════════════════════════════ */

static void sd_power(uint32_t volt_bits)
{
    if (volt_bits == 0) {
        sd_clr(R_HOST_CTL1, HCTL_SD_BUS_PWR);
        top_wr(g_sdblk.top_off_pwrsw, 0x09U);
    } else {
        sd_mod(R_HOST_CTL1, HCTL_VOL_MASK | HCTL_SD_BUS_PWR,
               volt_bits | HCTL_SD_BUS_PWR);
        top_wr(g_sdblk.top_off_pwrsw,
               (volt_bits == HCTL_VOL_1V8) ? 0x0DU : 0x09U);
    }
    SD_DELAY_LONG();
}

static void sd_set_clock(uint8_t divider)
{
    sd_clr(R_CLK_CTL, CLK_SD_CLK_EN);
    sd_mod(R_CLK_CTL, CLK_FREQ_SEL_MASK,
           (uint32_t)divider << CLK_FREQ_SEL_SHIFT);
    sd_set(R_CLK_CTL, CLK_INT_CLK_EN);
    while (!(sd_rd(R_CLK_CTL) & CLK_INT_STABLE)) {
        SD_DELAY_SHORT();
    }
    sd_set(R_CLK_CTL, CLK_SD_CLK_EN);
    SD_DELAY_LONG();
}

static void sd_reset_config(void)
{
    sd_power(0);
    sd_clr(R_CLK_CTL, CLK_SW_RST_DAT | CLK_SW_RST_CMD | CLK_SW_RST_ALL);
    sd_delay(0x1000U);
    sd_power(HCTL_VOL_3V3);
    sd_set(R_HOST_CTL1, HCTL_DAT_4BIT);
}

static int sd_wait_cmd_done(void)
{
    for (uint32_t i = 0; i < 1000000; i++) {
        uint32_t sts = sd_rd(R_INT_STS);
        if (sts & INT_ERR) {
            sd_wr(R_INT_STS, INT_CLEAR_ALL);
            return SDBLK_ERR;
        }
        if (sts & INT_CMD_CMPL) {
            sd_wr(R_INT_STS, INT_CMD_CMPL);
            return SDBLK_OK;
        }
        sd_delay(1);
    }
    KLOG_ERROR("[sdblk] cmd timeout (no completion)\n");
    sd_wr(R_INT_STS, INT_CLEAR_ALL);
    return SDBLK_ERR;
}

static int sd_wait_xfer_done(void)
{
    for (uint32_t i = 0; i < 5000000; i++) {
        uint32_t sts = sd_rd(R_INT_STS);
        if (sts & INT_XFER_CMPL) {
            sd_wr(R_INT_STS, INT_XFER_CMPL);
            return SDBLK_OK;
        }
        if (sts & INT_ERR) {
            sd_wr(R_INT_STS, INT_CLEAR_ALL);
            KLOG_ERROR("[sdblk] xfer error, int_sts=0x%08x\n", sts);
            return SDBLK_ERR;
        }
        sd_delay(1);
    }
    KLOG_ERROR("[sdblk] xfer timeout\n");
    sd_wr(R_INT_STS, INT_CLEAR_ALL);
    return SDBLK_ERR;
}

static int sd_send_cmd(uint32_t cmdw, uint32_t arg)
{
    while (sd_rd(R_PSTATE) & (PSTATE_CMD_INHIBIT | PSTATE_DAT_INHIBIT)) {
        /* spin */
    }
    sd_mod(R_CLK_CTL, CLK_TOUT_MASK, CLK_TOUT_MAX);
    sd_wr(R_INT_STS, INT_CLEAR_ALL);
    sd_wr(R_ARG1, arg);
    sd_wr(R_XFER_MODE_CMD, cmdw);
    return sd_wait_cmd_done();
}

/*
 * sd_pio_read_blocks — PIO 读取 N 个 512 字节块
 * 每块等待 BUF_RRDY，然后从 R_BUF_DATA 读取 128 个 32 位字。
 */
static int sd_pio_read_blocks(uint8_t *buf, size_t count)
{
    for (size_t b = 0; b < count; b++) {
        for (uint32_t t = 0; t < 5000000; t++) {
            uint32_t sts = sd_rd(R_INT_STS);
            if (sts & INT_BUF_RRDY) {
                sd_wr(R_INT_STS, INT_BUF_RRDY);
                goto rd_ready;
            }
            if (sts & INT_ERR) {
                sd_wr(R_INT_STS, INT_CLEAR_ALL);
                return SDBLK_ERR;
            }
        }
        return SDBLK_ERR;
rd_ready:
        for (size_t i = 0; i < SDBLK_BLOCK_SIZE / 4; i++) {
            uint32_t v = sd_rd_pio(R_BUF_DATA);
            buf[0] = (uint8_t)(v);
            buf[1] = (uint8_t)(v >>  8);
            buf[2] = (uint8_t)(v >> 16);
            buf[3] = (uint8_t)(v >> 24);
            buf += 4;
        }
    }
    return SDBLK_OK;
}

/*
 * sd_pio_write_blocks — PIO 写入 N 个 512 字节块
 * 每块等待 BUF_WRDY，然后向 R_BUF_DATA 写入 128 个 32 位字。
 */
static int sd_pio_write_blocks(const uint8_t *buf, size_t count)
{
    for (size_t b = 0; b < count; b++) {
        for (uint32_t t = 0; t < 5000000; t++) {
            uint32_t sts = sd_rd(R_INT_STS);
            if (sts & INT_BUF_WRDY) {
                sd_wr(R_INT_STS, INT_BUF_WRDY);
                goto wr_ready;
            }
            if (sts & INT_ERR) {
                sd_wr(R_INT_STS, INT_CLEAR_ALL);
                return SDBLK_ERR;
            }
        }
        return SDBLK_ERR;
wr_ready:
        for (size_t i = 0; i < SDBLK_BLOCK_SIZE / 4; i++) {
            uint32_t v = (uint32_t)buf[0]
                       | ((uint32_t)buf[1] <<  8)
                       | ((uint32_t)buf[2] << 16)
                       | ((uint32_t)buf[3] << 24);
            sd_wr_pio(R_BUF_DATA, v);
            buf += 4;
        }
    }
    return SDBLK_OK;
}

/*
 * sd_parse_csd — 从 R2 响应寄存器解析 SD 卡容量
 *
 * SDHCI 将 R2 响应的 [135:8] 放入 RESP[127:0]：
 *   RESP_REG[i] = CSD[i+8]（CSD[7:0] CRC 已由硬件丢弃）
 *   r3=RESP3=CSD[127:96], r2=RESP2=CSD[95:64],
 *   r1=RESP1=CSD[63:32],  r0=RESP0=CSD[39:8]
 */
static uint64_t sd_parse_csd(uint8_t *out_struct,
                              uint32_t r0, uint32_t r1,
                              uint32_t r2, uint32_t r3)
{
    uint8_t csd_struct = (uint8_t)((r3 >> 22) & 0x3U);
    if (out_struct) *out_struct = csd_struct;

    uint64_t cap = 0;
    switch (csd_struct) {
    case 0: {
        /* CSD v1.0 (SDSC) */
        uint32_t read_bl_len = (r2 >> 8) & 0xFU;
        uint32_t c_size      = ((r2 & 0x3U) << 10) | ((r1 >> 22) & 0x3FFU);
        uint32_t c_size_mult = (r1 >> 7) & 0x7U;
        uint64_t mult        = 1ULL << (c_size_mult + 2);
        uint64_t blocknr     = ((uint64_t)c_size + 1) * mult;
        uint64_t block_len   = 1ULL << read_bl_len;
        cap = blocknr * block_len;
        break;
    }
    case 1: {
        /* CSD v2.0 (SDHC/SDXC)：C_SIZE = response1[29:8]，22-bit */
        uint64_t c_size = (r1 >> 8) & 0x3FFFFFU;
        cap = (c_size + 1) * 512ULL * 1024ULL;
        break;
    }
    case 2: {
        /* CSD v3.0 (SDUC)：C_SIZE 28-bit，跨 response2/response1 */
        uint64_t c_size = ((uint64_t)(r2 & 0xFU) << 24)
                        | ((r1 >> 8) & 0xFFFFFFU);
        cap = (c_size + 1) * 512ULL * 1024ULL;
        break;
    }
    default:
        break;
    }
    (void)r0;
    return cap;
}

/* ════════════════════════════════════════════════════════════════
 * 公开 API
 * ════════════════════════════════════════════════════════════════ */

int sdblk_init(void)
{
    g_sdblk.initialized  = false;
    /* 从 platform.lua 的 platform.sdmmc.* 读取硬件地址 */
    g_sdblk.sd_base       = platform_get_mmio("sdmmc", "sd_base");
    g_sdblk.top_base      = platform_get_mmio("sdmmc", "top_base");
    g_sdblk.top_off_pwrsw = (uint32_t)platform_get_uintptr("sdmmc", "top_off_pwrsw_ctrl");
    if (!g_sdblk.sd_base) {
        KLOG_ERROR("[sdblk] platform.sdmmc not configured\n");
        return SDBLK_ERR;
    }

    top_wr(g_sdblk.top_off_pwrsw, 0x09U);

    if (!(sd_rd(R_PSTATE) & PSTATE_CARD_INSERTED)) {
        KLOG_WARN("[sdblk] no card inserted\n");
        return SDBLK_NOCARD;
    }
    KLOG_DEBUG("[sdblk] card detected, initializing...\n");

    /* 复位配置（与 Rust BSP 一致：关电 → 清复位位 → 开 3.3V → 4bit） */
    sd_reset_config();

    /* 切换到 1.8V（U-Boot 已将物理电压轨切到 1.8V） */
    sd_power(HCTL_VOL_1V8);

    /* 时钟 divider=4（与 Rust BSP 一致） */
    sd_set_clock(4);

    /* CMD0: GO_IDLE_STATE */
    sd_send_cmd(CMDW_NONE(0), 0);

    /* CMD8: SEND_IF_COND */
    sd_send_cmd(CMDW_R1(8), 0x1AAU);

    /* ACMD41 循环：等待卡初始化完成 */
    for (int retry = 0; retry < 1000; retry++) {
        sd_send_cmd(CMDW_R1(55), 0);
        int rc = sd_send_cmd(CMDW_R3(41),
                             0x40000000U | 0x00300000U | (0x1FFU << 15));
        if (rc == SDBLK_OK && (sd_rd(R_RESP0) >> 31)) {
            break;
        }
        sd_delay(0x1000000U);
    }

    /* CMD2: ALL_SEND_CID */
    if (sd_send_cmd(CMDW_R2(2), 0) != SDBLK_OK) {
        KLOG_ERROR("[sdblk] CMD2 failed\n");
        return SDBLK_ERR;
    }

    /* CMD3: SEND_RELATIVE_ADDR（R6 → Response48Busy 与 Rust BSP 一致） */
    if (sd_send_cmd(CMDW_R1B(3), 0) != SDBLK_OK) {
        KLOG_ERROR("[sdblk] CMD3 failed\n");
        return SDBLK_ERR;
    }
    g_sdblk.rca = sd_rd(R_RESP0) & 0xFFFF0000U;
    KLOG_DEBUG("[sdblk] RCA = 0x%08x\n", g_sdblk.rca);

    /* CMD9: SEND_CSD */
    if (sd_send_cmd(CMDW_R2(9), g_sdblk.rca) != SDBLK_OK) {
        KLOG_ERROR("[sdblk] CMD9 failed\n");
        return SDBLK_ERR;
    }
    uint32_t csd_r0 = sd_rd(R_RESP0);
    uint32_t csd_r1 = sd_rd(R_RESP1);
    uint32_t csd_r2 = sd_rd(R_RESP2);
    uint32_t csd_r3 = sd_rd(R_RESP3);
    g_sdblk.capacity = sd_parse_csd(&g_sdblk.csd_structure,
                                     csd_r0, csd_r1, csd_r2, csd_r3);
    KLOG_INFO("[sdblk] CSD v%u.0, capacity = %llu MiB\n",
              (unsigned)(g_sdblk.csd_structure + 1),
              (unsigned long long)(g_sdblk.capacity >> 20));

    /* CMD7: SELECT_CARD */
    if (sd_send_cmd(CMDW_R1B(7), g_sdblk.rca) != SDBLK_OK) {
        KLOG_ERROR("[sdblk] CMD7 failed\n");
        return SDBLK_ERR;
    }

    /* ACMD6: SET_BUS_WIDTH = 4 bit */
    sd_send_cmd(CMDW_R1(55), g_sdblk.rca);
    if (sd_send_cmd(CMDW_R1(6), 2) != SDBLK_OK) {
        KLOG_ERROR("[sdblk] ACMD6 (4-bit) failed\n");
        return SDBLK_ERR;
    }
    sd_set(R_HOST_CTL1, HCTL_DAT_4BIT);

    g_sdblk.initialized = true;
    KLOG_INFO("[sdblk] init done, rca=0x%08x, cap=%llu MiB\n",
              g_sdblk.rca, (unsigned long long)(g_sdblk.capacity >> 20));
    return SDBLK_OK;
}

int sdblk_read_blocks(uint32_t block_id, void *buf, size_t count)
{
    if (!g_sdblk.initialized || !buf || count == 0)
        return SDBLK_ERR;

    int rc;
    if (count == 1) {
        /* CMD17: READ_SINGLE_BLOCK */
        sd_wr(R_BLK_SIZE_CNT, BLKSZ_CNT(BLKSZ_512, 1));
        uint32_t cmdw = CMDW_R1(17) | CMD_DATA_PRESENT | XFER_DIR_READ;
        rc = sd_send_cmd(cmdw, block_id);
    } else {
        /* CMD18: READ_MULTIPLE_BLOCK + AutoCMD12 */
        sd_wr(R_BLK_SIZE_CNT, BLKSZ_CNT(BLKSZ_512, (uint32_t)count));
        uint32_t cmdw = CMDW_R1(18) | CMD_DATA_PRESENT | XFER_DIR_READ
                      | XFER_BLK_CNT_EN | XFER_MULTI_BLK | XFER_AUTOCMD12;
        rc = sd_send_cmd(cmdw, block_id);
    }
    if (rc != SDBLK_OK) return rc;

    rc = sd_pio_read_blocks((uint8_t *)buf, count);
    if (rc != SDBLK_OK) return rc;

    rc = sd_wait_xfer_done();
    sd_wr(R_INT_STS, sd_rd(R_INT_STS));
    return rc;
}

int sdblk_write_blocks(uint32_t block_id, const void *buf, size_t count)
{
    if (!g_sdblk.initialized || !buf || count == 0)
        return SDBLK_ERR;

    int rc;
    if (count == 1) {
        /* CMD24: WRITE_BLOCK */
        sd_wr(R_BLK_SIZE_CNT, BLKSZ_CNT(BLKSZ_512, 1));
        uint32_t cmdw = CMDW_R1(24) | CMD_DATA_PRESENT;
        rc = sd_send_cmd(cmdw, block_id);
    } else {
        /* CMD25: WRITE_MULTIPLE_BLOCK + AutoCMD12 */
        sd_wr(R_BLK_SIZE_CNT, BLKSZ_CNT(BLKSZ_512, (uint32_t)count));
        uint32_t cmdw = CMDW_R1(25) | CMD_DATA_PRESENT
                      | XFER_BLK_CNT_EN | XFER_MULTI_BLK | XFER_AUTOCMD12;
        rc = sd_send_cmd(cmdw, block_id);
    }
    if (rc != SDBLK_OK) return rc;

    rc = sd_pio_write_blocks((const uint8_t *)buf, count);
    if (rc != SDBLK_OK) return rc;

    rc = sd_wait_xfer_done();
    sd_wr(R_INT_STS, sd_rd(R_INT_STS));
    return rc;
}

uint64_t sdblk_capacity_bytes(void)
{
    return g_sdblk.initialized ? g_sdblk.capacity : 0ULL;
}

uint64_t sdblk_capacity_blocks(void)
{
    return g_sdblk.initialized ? g_sdblk.capacity / SDBLK_BLOCK_SIZE : 0ULL;
}

/* ════════════════════════════════════════════════════════════════
 * lwext4 块设备接口
 * ════════════════════════════════════════════════════════════════ */
#include <ext4_blockdev.h>
#include <ext4_errno.h>

static int sdblk_bdev_open(struct ext4_blockdev *bdev)
{
    int r = sdblk_init();
    if (r != SDBLK_OK)
        return (r == SDBLK_NOCARD) ? ENODEV : EIO;
    bdev->bdif->ph_bcnt = sdblk_capacity_blocks();
    bdev->part_size     = sdblk_capacity_bytes();
    return EOK;
}

static int sdblk_bdev_bread(struct ext4_blockdev *bdev, void *buf,
                            uint64_t blk_id, uint32_t blk_cnt)
{
    (void)bdev;
    return sdblk_read_blocks((uint32_t)blk_id, buf, blk_cnt) == SDBLK_OK
           ? EOK : EIO;
}

static int sdblk_bdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
                             uint64_t blk_id, uint32_t blk_cnt)
{
    (void)bdev;
    return sdblk_write_blocks((uint32_t)blk_id, buf, blk_cnt) == SDBLK_OK
           ? EOK : EIO;
}

static int sdblk_bdev_close(struct ext4_blockdev *bdev)
{
    (void)bdev;
    return EOK;
}

EXT4_BLOCKDEV_STATIC_INSTANCE(
    g_sdblk_bdev,
    SDBLK_BLOCK_SIZE,
    0,               /* ph_bcnt 在 sdblk_bdev_open() 中由 sdblk_capacity_blocks() 填充 */
    sdblk_bdev_open,
    sdblk_bdev_bread,
    sdblk_bdev_bwrite,
    sdblk_bdev_close,
    NULL,   /* lock   */
    NULL    /* unlock */
);

struct ext4_blockdev *sdblk_get_bdev(void)
{
    return &g_sdblk_bdev;
}
