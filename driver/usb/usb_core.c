/*
 * driver/usb/usb_core.c — USB 核心协议栈：EP0 控制传输 + 设备枚举
 *
 * 通道分配约定：
 *   - 通道 0：EP0 控制传输
 *   - 通道 1：Bulk/Isoch（后续实现）
 *
 * RISC-V 缓存说明：
 *   dcache_clean_for_dma / dcache_invalidate_after_dma 见 crate::utils::cache。
 *   当前 SG2002 在 OpenSBI 下 L1 dcache 关闭，暂以空宏代替；启用 dcache 后需替换。
 */

#include "usb/dwc2_regs.h"
#include "usb/usb.h"
#include "usb/uvc.h"
#include "usb/uvc_video.h"
#include "usb/usb_core_internal.h"
#include "platform_cfg.h"
#include "mmio.h"
#include "mm_vm.h"
#include "klog.h"
#include "string.h"
#include "cache.h"     /* clean_dcache_range / invalidate_dcache_range */

/* ==========================================================================
 * 1. DMA 缓冲区（对齐 Rust DmaBuf：ep0.rs:57-66）
 * ========================================================================== */

#define EP0_BUF_ALIGN  256
#define OFF_EP0        0       /* SETUP 8 字节 + EP0 小数据 */
#define DMA_OFF_SMALL_IO 256   /* 多包数据 IN 临时区 */
#define USB_CONFIG_BUF_LEN 4096
#define USB_VIDEO_DMA_ALIGN 256

static uint8_t g_ep0_buf[1024] __attribute__((aligned(EP0_BUF_ALIGN)));
static uint8_t g_config_desc_buf[USB_CONFIG_BUF_LEN];

/* 对齐 Rust crate::utils::cache：clean = 写回 CPU 缓存 → DMA 可见
 *                      invalidate = 使失效 → CPU 看到 DMA 新数据 */
#define dcache_clean_for_dma(ptr, len) \
    clean_dcache_range((const void *)(ptr), (size_t)(len))

#define dcache_invalidate_after_dma(ptr, len) \
    invalidate_dcache_range((const void *)(ptr), (size_t)(len))

/* ==========================================================================
 * 2. 寄存器访问（通过 dwc2_get_base_virt()）
 * ========================================================================== */

extern uintptr_t dwc2_get_base_virt(void);

static inline uint32_t _r32(uintptr_t off)
{
    return read32((void *)(dwc2_get_base_virt() + off));
}
static inline void _w32(uintptr_t off, uint32_t val)
{
    write32(val, (void *)(dwc2_get_base_virt() + off));
}
static inline uint32_t _hc_r32(uint32_t ch, uintptr_t reg_off)
{
    return read32((void *)(dwc2_get_base_virt() + DWC2_OFF_HC_BASE
                           + ch * DWC2_OFF_HC_STRIDE + reg_off));
}
static inline void _hc_w32(uint32_t ch, uintptr_t reg_off, uint32_t val)
{
    write32(val, (void *)(dwc2_get_base_virt() + DWC2_OFF_HC_BASE
                          + ch * DWC2_OFF_HC_STRIDE + reg_off));
}

/* DMA 物理地址：SG2002 启用 MMU 时需减去 KERNEL_VMA */
static inline uint32_t dma_phys(void *ptr)
{
    uintptr_t va = (uintptr_t)ptr;
#if DEVICE_MMIO_NEEDS_VMA
    return (uint32_t)(va - KERNEL_VMA);
#else
    return (uint32_t)va;
#endif
}

/* ==========================================================================
 * 3. 自旋 / 总线 fence
 * ========================================================================== */

static void _spin(uint32_t n)
{
    timer_spin(n);
}

/* RISC-V: DMA 前执行 fence rw,rw 确保 CPU 写入对控制器可见 */
static inline void usb_bus_fence_before_dma(void)
{
#if ARCH_RISCV64
    __asm__ volatile("fence rw, rw" ::: "memory");
#endif
}

/* ==========================================================================
 * 4. 通道常量（对齐 ep0.rs:30-54）
 * ========================================================================== */

#define CH_CTL  0
#define CH_BULK 1

#define HCCHAR_EPDIR       (1u << 15)

#define HCINT_ALL_W1C  0x7FFu

#define PID_DATA0  0
#define PID_DATA1  2
#define PID_DATA2  1
#define PID_SETUP  3

/* ==========================================================================
 * 5. HCCHAR / HCTSIZ 构造（对齐 ep0.rs:274-325）
 * ========================================================================== */

static uint32_t hcchar_control(uint32_t dev, uint32_t ep, uint32_t mps,
                               bool dir_in)
{
    uint32_t v = mps & HCCHAR_MPS_MASK;
    v |= (ep & 0xf) << HCCHAR_EPNUM_SHIFT;
    if (dir_in)
        v |= HCCHAR_EPDIR;
    v |= HCCHAR_EPTYPE_CONTROL;
    v |= (dev & 0x7f) << HCCHAR_DEVADDR_SHIFT;
    return v;
}

static uint32_t __attribute__((unused))
hcchar_bulk(uint32_t dev, uint32_t ep, uint32_t mps,
                             bool dir_in)
{
    uint32_t v = mps & HCCHAR_MPS_MASK;
    v |= (ep & 0xf) << HCCHAR_EPNUM_SHIFT;
    if (dir_in)
        v |= HCCHAR_EPDIR;
    v |= (HCCHAR_EPTYPE_BULK << HCCHAR_EPTYPE_SHIFT);
    v |= (dev & 0x7f) << HCCHAR_DEVADDR_SHIFT;
    return v;
}

static uint32_t hcchar_isoch(uint32_t dev, uint32_t ep, uint32_t mps,
                             uint32_t mult, bool dir_in)
{
    uint32_t v = mps & HCCHAR_MPS_MASK;
    v |= (ep & 0xf) << HCCHAR_EPNUM_SHIFT;
    if (dir_in)
        v |= HCCHAR_EPDIR;
    v |= (HCCHAR_EPTYPE_ISO << HCCHAR_EPTYPE_SHIFT);
    if (mult < 1)
        mult = 1;
    if (mult > 3)
        mult = 3;
    v |= (mult & HCCHAR_MC_MASK) << HCCHAR_MC_SHIFT;
    v |= (dev & 0x7f) << HCCHAR_DEVADDR_SHIFT;
    return v;
}

static uint32_t next_uframe_oddfrm(void)
{
    uint32_t fr = _r32(DWC2_OFF_HFNUM) & 0xffffu;
    return (fr & 1u) == 0 ? HCCHAR_ODDFRM : 0;
}

/*
 * HCTSIZ: PID(pktcnt, xfersize)
 *
 * 对齐 hctsiz() ep0.rs:323: (HCTSIZ::PID.val(pid) + HCTSIZ::PKTCNT.val(pktcnt)
 *                           + HCTSIZ::XFERSIZE.val(xfersize)).value
 * 即 PID 在 bits[29:30], PKTCNT 在 bits[19:28], XFERSIZE 在 bits[0:18]。
 */
static uint32_t hctsiz(uint32_t pid, uint32_t pktcnt, uint32_t xfersize)
{
    return ((pid & 0x3) << HCTSIZ_PID_SHIFT)
         | ((pktcnt & HCTSIZ_PKTCNT_MASK) << HCTSIZ_PKTCNT_SHIFT)
         | (xfersize & HCTSIZ_XFERSIZE_MASK);
}

static uint32_t normalize_ep0_mps(uint8_t b)
{
    switch (b) {
    case 8:  case 16: case 32: case 64:
        return b;
    default:
        return 8;
    }
}

static inline uint16_t le16_load(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t le32_load(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ==========================================================================
 * 6. 通道控制：等待禁用 / 等待 halt（对齐 ep0.rs:144-182）
 * ========================================================================== */

static int ch_wait_disabled(uint32_t ch)
{
    for (uint32_t i = 0; i < 2000000; i++) {
        if (!(_hc_r32(ch, HC_OFF_CHAR) & HCCHAR_CHENA))
            return 0;
        _spin(8);
    }
    KLOG_ERROR("[USB] ch%lu wait_disabled timeout HCCHAR=0x%08x\n",
               (unsigned long)ch, _hc_r32(ch, HC_OFF_CHAR));
    return -1;
}

static void ch_halt(uint32_t ch)
{
    uint32_t v = _hc_r32(ch, HC_OFF_CHAR);
    if (!(v & HCCHAR_CHENA))
        return;
    _hc_w32(ch, HC_OFF_CHAR, v | HCCHAR_CHENA | HCCHAR_CHDIS);
    for (uint32_t i = 0; i < 500000; i++) {
        if (!(_hc_r32(ch, HC_OFF_CHAR) & HCCHAR_CHENA))
            return;
        _spin(8);
    }
}

static int ch_wait_halted(uint32_t ch, uint32_t *out_hcint)
{
    for (uint32_t i = 0; i < 8000000; i++) {
        uint32_t hi = _hc_r32(ch, HC_OFF_INT) | dwc2_usb_take_hcint(ch);
        if (hi & HCINT_CHHLTD) {
            _hc_w32(ch, HC_OFF_INT, hi);
            *out_hcint = hi;
            return 0;
        }
        _spin(8);
    }
    KLOG_ERROR("[USB] ch%lu wait_halted timeout HCINT=0x%08x\n",
               (unsigned long)ch, _hc_r32(ch, HC_OFF_INT));
    return -1;
}

/* ==========================================================================
 * 7. ch_xfer — 核心传输引擎（对齐 ep0.rs:186-238）
 * ==========================================================================
 *
 * EP0 控制传输上：
 *   NAK = 设备未就绪 → 重试最多 64 次
 *   XACTERR = CRC/PID/bit-stuff 错误 → 重试最多 8 次（退避 1ms）
 *   STALL → 立即返回错误
 */

#define NAK_RETRIES  64
#define XACT_RETRIES 8

static int ch_xfer(uint32_t ch, uint32_t hcchar, uint32_t hctsiz_val,
                   uint32_t dma_off, uint32_t *out_hcint)
{
    uint32_t dmap = dma_phys(g_ep0_buf + dma_off);
    uint32_t xact_left = XACT_RETRIES;

    for (uint32_t attempt = 0; attempt <= NAK_RETRIES; attempt++) {
        if (ch_wait_disabled(ch) != 0)
            return -1;
        ch_halt(ch);

        _hc_w32(ch, HC_OFF_SPLT, 0);
        _hc_w32(ch, HC_OFF_INT, HCINT_ALL_W1C);
        (void)dwc2_usb_take_hcint(ch);
        _hc_w32(ch, HC_OFF_TSIZ, hctsiz_val);
        usb_bus_fence_before_dma();
        _hc_w32(ch, HC_OFF_DMA, dmap);
        usb_bus_fence_before_dma();
        _hc_w32(ch, HC_OFF_CHAR, hcchar | HCCHAR_CHENA);

        uint32_t hi;
        if (ch_wait_halted(ch, &hi) != 0)
            return -1;

        if (hi & HCINT_STALL) {
            KLOG_ERROR("[USB] ch%lu STALL hcchar=0x%08x hctsiz=0x%08x dma=0x%08x "
                       "hcint=0x%08x hctsiz_now=0x%08x hcdma_now=0x%08x\n",
                       (unsigned long)ch, hcchar, hctsiz_val, dmap, hi,
                       _hc_r32(ch, HC_OFF_TSIZ), _hc_r32(ch, HC_OFF_DMA));
            KLOG_ERROR("[USB] ch%lu STALL regs HPRT0=0x%08x GINTSTS=0x%08x "
                       "GOTGCTL=0x%08x GUSBCFG=0x%08x HCFG=0x%08x\n",
                       (unsigned long)ch, _r32(DWC2_OFF_HPRT0),
                       _r32(DWC2_OFF_GINTSTS), _r32(DWC2_OFF_GOTGCTL),
                       _r32(DWC2_OFF_GUSBCFG), _r32(DWC2_OFF_HCFG));
            *out_hcint = hi;
            return -2;  /* STALL */
        }
        if (hi & HCINT_XACTERR) {
            if (xact_left == 0) {
                KLOG_ERROR("[USB] ch%lu XACT exhausted "
                           "hcchar=0x%08x hctsiz=0x%08x dma=0x%08x hcint=0x%08x\n",
                           (unsigned long)ch, hcchar, hctsiz_val, dmap, hi);
                *out_hcint = hi;
                return -1;
            }
            xact_left--;
            timer_delay_ms(1);
            continue;
        }
        if (hi & HCINT_NAK) {
            if (attempt == 0 || attempt == NAK_RETRIES) {
                KLOG_INFO("[USB] ch%lu NAK attempt=%lu/%u hcint=0x%08x\n",
                          (unsigned long)ch, (unsigned long)attempt, NAK_RETRIES, hi);
            }
            if (attempt == NAK_RETRIES) {
                KLOG_ERROR("[USB] ch%lu NAK exhausted "
                           "hcchar=0x%08x hctsiz=0x%08x dma=0x%08x hcint=0x%08x\n",
                           (unsigned long)ch, hcchar, hctsiz_val, dmap, hi);
                *out_hcint = hi;
                return -1;
            }
            timer_delay_ms(1);
            continue;
        }
        if (!(hi & HCINT_XFERCOMPL)) {
            KLOG_ERROR("[USB] ch%lu CHHLTD without XFERCOMPL "
                       "hcchar=0x%08x hctsiz=0x%08x hcint=0x%08x\n",
                       (unsigned long)ch, hcchar, hctsiz_val, hi);
            *out_hcint = hi;
            return -1;
        }
        if (attempt > 0) {
            KLOG_INFO("[USB] ch%lu XFERCOMPL after %lu retries hcint=0x%08x\n",
                      (unsigned long)ch, (unsigned long)attempt, hi);
        }
        *out_hcint = hi;
        return 0;
    }
    return -1;
}

/* ==========================================================================
 * 8. EP0 控制传输（对齐 ep0.rs:345-367, 507-562）
 * ========================================================================== */

/*
 * ep0_control_write_no_data — SETUP + STATUS IN (0 bytes)
 *
 * 对齐 ep0.rs:351 ep0_control_write_no_data()
 */
int usb_ep0_control_write_no_data(uint32_t dev, const uint8_t setup[8],
                                  uint32_t ep0_mps)
{
    uint32_t hcint = 0;

    /* SETUP phase */
    memcpy(g_ep0_buf + OFF_EP0, setup, 8);
    dcache_clean_for_dma(g_ep0_buf + OFF_EP0, 8);

    uint32_t hc = hcchar_control(dev, 0, ep0_mps, false);
    int rc = ch_xfer(CH_CTL, hc, hctsiz(PID_SETUP, 1, 8), OFF_EP0, &hcint);
    if (rc != 0) {
        KLOG_ERROR("[USB] ep0_write_no_data SETUP failed: rc=%d addr=%lu\n",
                   rc, (unsigned long)dev);
        return rc;
    }

    /* STATUS IN (0 bytes) */
    hc = hcchar_control(dev, 0, ep0_mps, true);
    rc = ch_xfer(CH_CTL, hc, hctsiz(PID_DATA1, 1, 0), OFF_EP0, &hcint);
    if (rc != 0) {
        KLOG_ERROR("[USB] ep0_write_no_data STATUS failed: rc=%d addr=%lu\n",
                   rc, (unsigned long)dev);
        return rc;
    }

    return 0;
}

/*
 * ep0_control_read — SETUP + DATA IN (multi-packet) + STATUS OUT
 *
 * 对齐 ep0.rs:509 ep0_control_read()
 */
int usb_ep0_control_read(uint32_t dev, const uint8_t setup[8],
                         uint32_t ep0_mps, uint8_t *out, uint32_t out_len)
{
    if (out_len == 0 || out_len > 4096)
        return -1;

    uint32_t hcint = 0;
    uint32_t total = out_len;

    /* SETUP */
    memcpy(g_ep0_buf + OFF_EP0, setup, 8);
    dcache_clean_for_dma(g_ep0_buf + OFF_EP0, 8);

    uint32_t hc = hcchar_control(dev, 0, ep0_mps, false);
    int rc = ch_xfer(CH_CTL, hc, hctsiz(PID_SETUP, 1, 8), OFF_EP0, &hcint);
    if (rc != 0)
        return rc;

    /* DATA IN — 多包 */
    uint32_t left    = total;
    uint32_t out_off = 0;
    uint32_t toggle  = PID_DATA1;  /* DATA 阶段从 DATA1 开始 */

    while (left > 0) {
        uint32_t chunk = left < ep0_mps ? left : ep0_mps;
        /* pktcnt: ceil-div by mps */
        uint32_t pkts = (chunk + ep0_mps - 1) / ep0_mps;

        hc = hcchar_control(dev, 0, ep0_mps, true);
        rc = ch_xfer(CH_CTL, hc, hctsiz(toggle, pkts, chunk),
                     DMA_OFF_SMALL_IO, &hcint);
        if (rc != 0)
            return rc;

        dcache_invalidate_after_dma(g_ep0_buf + DMA_OFF_SMALL_IO, chunk);
        memcpy(out + out_off, g_ep0_buf + DMA_OFF_SMALL_IO, chunk);

        out_off += chunk;
        left    -= chunk;
        toggle   = (toggle == PID_DATA1) ? PID_DATA0 : PID_DATA1;
    }

    /* STATUS OUT (0 bytes) */
    hc = hcchar_control(dev, 0, ep0_mps, false);
    rc = ch_xfer(CH_CTL, hc, hctsiz(PID_DATA1, 1, 0), OFF_EP0, &hcint);
    if (rc != 0)
        return rc;

    return 0;
}

/* ep0_control_write — SETUP + DATA OUT + STATUS IN */
int usb_ep0_control_write(uint32_t dev, const uint8_t setup[8],
                          uint32_t ep0_mps, const uint8_t *data,
                          uint32_t data_len)
{
    if (data_len > 4096)
        return -1;

    uint32_t hcint = 0;

    memcpy(g_ep0_buf + OFF_EP0, setup, 8);
    dcache_clean_for_dma(g_ep0_buf + OFF_EP0, 8);

    uint32_t hc = hcchar_control(dev, 0, ep0_mps, false);
    int rc = ch_xfer(CH_CTL, hc, hctsiz(PID_SETUP, 1, 8), OFF_EP0, &hcint);
    if (rc != 0)
        return rc;

    uint32_t left = data_len;
    uint32_t src = 0;
    uint32_t toggle = PID_DATA1;
    while (left > 0) {
        uint32_t chunk = left < ep0_mps ? left : ep0_mps;
        uint32_t pkts = (chunk + ep0_mps - 1) / ep0_mps;

        memcpy(g_ep0_buf + DMA_OFF_SMALL_IO, data + src, chunk);
        dcache_clean_for_dma(g_ep0_buf + DMA_OFF_SMALL_IO, chunk);

        hc = hcchar_control(dev, 0, ep0_mps, false);
        rc = ch_xfer(CH_CTL, hc, hctsiz(toggle, pkts, chunk),
                     DMA_OFF_SMALL_IO, &hcint);
        if (rc != 0)
            return rc;

        src += chunk;
        left -= chunk;
        toggle = (toggle == PID_DATA1) ? PID_DATA0 : PID_DATA1;
    }

    hc = hcchar_control(dev, 0, ep0_mps, true);
    return ch_xfer(CH_CTL, hc, hctsiz(PID_DATA1, 1, 0), OFF_EP0, &hcint);
}

/* ==========================================================================
 * 9. 标准 USB 请求（对齐 ep0.rs:370-478, setup.rs）
 * ========================================================================== */

/*
 * 构造 SETUP 包（小端，8 字节）
 * 对齐 setup.rs 中各标准请求函数
 */

static void make_setup_get_descriptor_device(uint16_t w_length, uint8_t out[8])
{
    out[0] = 0x80;  /* bmRequestType: Dir IN, Type Standard, Recip Device */
    out[1] = 6;     /* GET_DESCRIPTOR */
    out[2] = 0x00;  /* wValue low (descriptor index = 0) */
    out[3] = 0x01;  /* wValue high (DEVICE = 1) */
    out[4] = 0x00;  /* wIndex low */
    out[5] = 0x00;  /* wIndex high */
    out[6] = (uint8_t)(w_length & 0xff);
    out[7] = (uint8_t)((w_length >> 8) & 0xff);
}

static void make_setup_get_descriptor_configuration(uint8_t cfg_index,
                                                    uint16_t w_length,
                                                    uint8_t out[8])
{
    out[0] = 0x80;       /* Dir IN, Type Standard, Recip Device */
    out[1] = 6;          /* GET_DESCRIPTOR */
    out[2] = cfg_index;  /* descriptor index */
    out[3] = 0x02;       /* CONFIGURATION */
    out[4] = 0;
    out[5] = 0;
    out[6] = (uint8_t)(w_length & 0xff);
    out[7] = (uint8_t)((w_length >> 8) & 0xff);
}

static void make_setup_set_address(uint8_t addr, uint8_t out[8])
{
    out[0] = 0x00;  /* Dir OUT, Type Standard, Recip Device */
    out[1] = 5;     /* SET_ADDRESS */
    out[2] = addr;
    out[3] = 0;
    out[4] = 0;
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
}

static void make_setup_set_configuration(uint8_t cfg, uint8_t out[8])
{
    out[0] = 0x00;
    out[1] = 9;     /* SET_CONFIGURATION */
    out[2] = cfg;
    out[3] = 0;
    out[4] = 0;
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
}

static void make_setup_set_interface(uint8_t alt, uint8_t interface, uint8_t out[8])
{
    out[0] = 0x01;  /* Dir OUT, Type Standard, Recip Interface */
    out[1] = 0x0b;  /* SET_INTERFACE */
    out[2] = alt;
    out[3] = 0;
    out[4] = interface;
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
}

/*
 * get_device_desc_default_addr — 对齐 sg200x-bsp ep0.rs:get_device_vid_pid_default_addr()
 *
 * BSP 的默认地址枚举首包使用 MPS=64 直接读 18 字节设备描述符；不要先发
 * GET_DESCRIPTOR(8)，部分设备/控制器组合会在该 DATA 阶段 STALL。
 */
static int get_device_desc_default_addr(uint8_t *out_buf)
{
    uint8_t setup[8];
    make_setup_get_descriptor_device(18, setup);
    KLOG_INFO("[USB]   SETUP bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
              setup[0], setup[1], setup[2], setup[3],
              setup[4], setup[5], setup[6], setup[7]);
    memcpy(g_ep0_buf + OFF_EP0, setup, 8);
    dcache_clean_for_dma(g_ep0_buf + OFF_EP0, 8);

    uint32_t hcint = 0;
    int rc;
    uint32_t hc = hcchar_control(0, 0, 64, false);
    rc = ch_xfer(CH_CTL, hc, hctsiz(PID_SETUP, 1, 8), OFF_EP0, &hcint);
    KLOG_INFO("[USB]   18B SETUP @addr=0 MPS=64: HCCHAR=0x%08x rc=%d HCINT=0x%08x\n",
              hc, rc, hcint);
    if (rc != 0)
        return rc;

    hc = hcchar_control(0, 0, 64, true);
    rc = ch_xfer(CH_CTL, hc, hctsiz(PID_DATA1, 1, 18), OFF_EP0, &hcint);
    if (rc != 0) {
        KLOG_ERROR("[USB]   18B DATA @addr=0 MPS=64 failed: rc=%d HCINT=0x%08x\n",
                   rc, hcint);
        return rc;
    }

    uint32_t rem = _hc_r32(CH_CTL, HC_OFF_TSIZ) & HCTSIZ_XFERSIZE_MASK;
    uint32_t actual = 18 - rem;
    KLOG_INFO("[USB]   18B DATA @addr=0 MPS=64: actual=%lu bytes\n",
              (unsigned long)actual);
    if (actual < 18) {
        KLOG_ERROR("[USB]   short device descriptor: %lu bytes\n",
                   (unsigned long)actual);
        return -1;
    }

    dcache_invalidate_after_dma(g_ep0_buf + OFF_EP0, 18);
    memcpy(out_buf, g_ep0_buf + OFF_EP0, 18);

    hc = hcchar_control(0, 0, 64, false);
    rc = ch_xfer(CH_CTL, hc, hctsiz(PID_DATA1, 1, 0), OFF_EP0, &hcint);
    if (rc != 0) {
        KLOG_ERROR("[USB]   18B STATUS OUT @addr=0 MPS=64 failed: rc=%d HCINT=0x%08x\n",
                   rc, hcint);
    }
    return rc;
}

/* SET_ADDRESS @ addr=0（对齐 ep0.rs:376） */
static int set_usb_address(uint8_t addr, uint32_t ep0_mps)
{
    uint8_t setup[8];
    make_setup_set_address(addr, setup);
    return usb_ep0_control_write_no_data(0, setup, ep0_mps);
}

/* SET_CONFIGURATION（对齐 ep0.rs:385） */
static int set_configuration(uint32_t dev, uint8_t cfg, uint32_t ep0_mps)
{
    uint8_t setup[8];
    make_setup_set_configuration(cfg, setup);
    return usb_ep0_control_write_no_data(dev, setup, ep0_mps);
}

static int read_configuration_descriptor(uint32_t dev, uint32_t ep0_mps,
                                         uint8_t cfg_index, uint8_t *buf,
                                         uint32_t cap, uint32_t *out_total,
                                         uint8_t *out_cfg_value)
{
    uint8_t setup[8];
    uint8_t hdr[9];

    make_setup_get_descriptor_configuration(cfg_index, 9, setup);
    int rc = usb_ep0_control_read(dev, setup, ep0_mps, hdr, sizeof(hdr));
    if (rc != 0) {
        KLOG_ERROR("[USB] GET_DESCRIPTOR(CONFIG,9) failed: %d\n", rc);
        return rc;
    }
    if (hdr[0] < 9 || hdr[1] != USB_DESC_CONFIGURATION) {
        KLOG_ERROR("[USB] bad config header: len=%u type=%u\n", hdr[0], hdr[1]);
        return -1;
    }

    uint32_t total = le16_load(hdr + 2);
    if (total < 9 || total > cap) {
        KLOG_ERROR("[USB] bad config total length: %lu\n", (unsigned long)total);
        return -1;
    }

    make_setup_get_descriptor_configuration(cfg_index, (uint16_t)total, setup);
    rc = usb_ep0_control_read(dev, setup, ep0_mps, buf, total);
    if (rc != 0) {
        KLOG_ERROR("[USB] GET_DESCRIPTOR(CONFIG,%lu) failed: %d\n",
                   (unsigned long)total, rc);
        return rc;
    }
    if (buf[1] != USB_DESC_CONFIGURATION) {
        KLOG_ERROR("[USB] full config descriptor type mismatch: %u\n", buf[1]);
        return -1;
    }

    *out_total = total;
    *out_cfg_value = buf[5];
    KLOG_INFO("[USB] Config desc: total=%lu interfaces=%u cfg=%u attr=0x%02x max_power=%u\n",
              (unsigned long)total, buf[4], buf[5], buf[7], buf[8]);
    return 0;
}

int usb_set_interface(uint32_t dev, uint8_t interface, uint8_t alt,
                      uint32_t ep0_mps)
{
    uint8_t setup[8];
    make_setup_set_interface(alt, interface, setup);
    return usb_ep0_control_write_no_data(dev, setup, ep0_mps);
}

int usb_isoch_in_packet(uint32_t dev, uint8_t ep, uint16_t mps_raw,
                        uint8_t *buf, uint32_t cap, uint32_t *out_actual)
{
    if (buf == NULL || out_actual == NULL)
        return -1;
    *out_actual = 0;

    uint32_t mps = mps_raw & 0x7ffu;
    uint32_t mult = ((mps_raw >> 11) & 0x3u) + 1u;
    if (mps == 0 || mult == 0 || mult > 3)
        return -1;

    uint32_t xfersize = mps * mult;
    if (xfersize == 0 || xfersize > cap)
        return -1;

    uint32_t pid = PID_DATA0;
    if (mult == 2)
        pid = PID_DATA1;
    else if (mult == 3)
        pid = PID_DATA2;

    if (ch_wait_disabled(CH_BULK) != 0)
        return -1;
    ch_halt(CH_BULK);

    uint32_t hc = hcchar_isoch(dev, ep, mps, mult, true);
    uint32_t tsiz = hctsiz(pid, mult, xfersize);
    uint32_t dmap = dma_phys(buf);

    _hc_w32(CH_BULK, HC_OFF_SPLT, 0);
    _hc_w32(CH_BULK, HC_OFF_INT, HCINT_ALL_W1C);
    _hc_w32(CH_BULK, HC_OFF_INTMSK, 0);
    (void)dwc2_usb_take_hcint(CH_BULK);
    _hc_w32(CH_BULK, HC_OFF_TSIZ, tsiz);
    usb_bus_fence_before_dma();
    _hc_w32(CH_BULK, HC_OFF_DMA, dmap);
    usb_bus_fence_before_dma();
    _hc_w32(CH_BULK, HC_OFF_CHAR, hc | next_uframe_oddfrm() | HCCHAR_CHENA);

    uint32_t hi = 0;
    if (ch_wait_halted(CH_BULK, &hi) != 0)
        return -1;

    if (hi & HCINT_STALL)
        return -2;
    if (hi & HCINT_AHBERR)
        return -1;
    if (hi & (HCINT_FRMOVRN | HCINT_XACTERR | HCINT_BBLERR |
              HCINT_DATATGLERR | HCINT_NYET | HCINT_NAK))
        return 0;
    if (!(hi & HCINT_XFERCOMPL))
        return 0;

    uint32_t rem = _hc_r32(CH_BULK, HC_OFF_TSIZ) & HCTSIZ_XFERSIZE_MASK;
    uint32_t actual = xfersize > rem ? xfersize - rem : 0;
    if (actual > cap)
        actual = cap;
    if (actual > 0)
        dcache_invalidate_after_dma(buf, actual);
    *out_actual = actual;
    return 0;
}

int usb_isoch_channel_setup(uint32_t dev, uint8_t ep, uint16_t mps_raw)
{
    (void)dev; (void)ep;
    uint32_t mps = mps_raw & 0x7ffu;
    uint32_t mult = ((mps_raw >> 11) & 0x3u) + 1u;
    if (mps == 0 || mult > 3)
        return -1;

    if (ch_wait_disabled(CH_BULK) != 0)
        return -1;
    ch_halt(CH_BULK);

    _hc_w32(CH_BULK, HC_OFF_SPLT, 0);
    _hc_w32(CH_BULK, HC_OFF_INTMSK, 0);
    (void)dwc2_usb_take_hcint(CH_BULK);
    return 0;
}

int usb_isoch_in_packet_fast(uint32_t dev, uint8_t ep, uint16_t mps_raw,
                             uint8_t *buf, uint32_t cap, uint32_t *out_actual)
{
    *out_actual = 0;

    uint32_t mps = mps_raw & 0x7ffu;
    uint32_t mult = ((mps_raw >> 11) & 0x3u) + 1u;
    uint32_t xfersize = mps * mult;
    if (xfersize == 0 || xfersize > cap)
        return -1;

    uint32_t pid = PID_DATA0;
    if (mult == 2)      pid = PID_DATA1;
    else if (mult == 3) pid = PID_DATA2;

    uint32_t hc = hcchar_isoch(dev, ep, mps, mult, true);
    uint32_t tsiz = hctsiz(pid, mult, xfersize);
    uint32_t dmap = dma_phys(buf);

    _hc_w32(CH_BULK, HC_OFF_INT, HCINT_ALL_W1C);
    _hc_w32(CH_BULK, HC_OFF_TSIZ, tsiz);
    usb_bus_fence_before_dma();
    _hc_w32(CH_BULK, HC_OFF_DMA, dmap);
    usb_bus_fence_before_dma();
    _hc_w32(CH_BULK, HC_OFF_CHAR, hc | next_uframe_oddfrm() | HCCHAR_CHENA);

    uint32_t hi = 0;
    for (uint32_t w = 0; w < 4000000; w++) {
        hi = _hc_r32(CH_BULK, HC_OFF_INT);
        if (hi & HCINT_CHHLTD)
            break;
    }
    _hc_w32(CH_BULK, HC_OFF_INT, hi);

    if (hi & HCINT_AHBERR)
        return -1;
    if (!(hi & HCINT_CHHLTD))
        return 0;
    if (hi & (HCINT_FRMOVRN | HCINT_XACTERR | HCINT_BBLERR |
              HCINT_DATATGLERR | HCINT_NYET | HCINT_NAK))
        return 0;
    if (!(hi & HCINT_XFERCOMPL))
        return 0;

    uint32_t rem = _hc_r32(CH_BULK, HC_OFF_TSIZ) & HCTSIZ_XFERSIZE_MASK;
    uint32_t actual = xfersize > rem ? xfersize - rem : 0;
    if (actual > cap)
        actual = cap;
    if (actual > 0)
        dcache_invalidate_after_dma(buf, actual);
    *out_actual = actual;
    return 0;
}

/* ==========================================================================
 * 10. 公开：根端口枚举（对齐 enumerate.rs + topology.rs）
 * ========================================================================== */

/*
 * dwc2_usb_enumerate_device — 对根端口执行完整枚举序列
 *
 * 对齐 enumerate.rs:12 enumerate_root_port() +
 *      topology.rs:enumerate_bus_print_tree() 中单设备路径
 *
 * 流程：
 *   1. 确认 CONNSTS=1
 *   2. 根端口复位脉冲 (~60ms) + 恢复等待 (~80ms)
 *   3. @addr=0 获取 18 字节设备描述符
 *   4. SET_ADDRESS → 新地址
 *   5. usb_post_set_address_delay() ~80ms
 *   6. 读取并解析配置描述符
 *   7. SET_CONFIGURATION(bConfigurationValue)
 *   8. 填充结果
 */
int dwc2_usb_enumerate_device(usb_enumerate_result_t *result)
{
    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));
    uvc_video_clear();

    /* 1. 确认设备连接 */
    uint32_t hprt = dwc2_usb_read_hprt0();
    if (!(hprt & HPRT0_CONNSTS)) {
        KLOG_ERROR("[USB] enumerate: no device on root port\n");
        return -1;
    }

    /* 2. 总线复位（复用 dwc2_usb_reset_root_port） */
    KLOG_INFO("[USB] === Root Port Enumeration ===\n");
    KLOG_INFO("[USB] Resetting root port...\n");
    int rc = dwc2_usb_reset_root_port();
    if (rc != 0) {
        KLOG_ERROR("[USB] Root port reset failed: %d\n", rc);
        return rc;
    }

    hprt = dwc2_usb_read_hprt0();
    uint32_t spd = (hprt >> HPRT0_SPD_SHIFT) & HPRT0_SPD_MASK;
    const char *spd_str = (spd == 0) ? "HS" : (spd == 1) ? "FS" :
                          (spd == 2) ? "LS" : "?";
    if (!(hprt & HPRT0_CONNSTS)) {
        KLOG_ERROR("[USB] Device disconnected during reset\n");
        return -1;
    }

    /* 3. 对齐 BSP：@addr=0, MPS=64, 一次读完整 18 字节设备描述符 */
    KLOG_INFO("[USB] Step 1: GET_DESCRIPTOR(18) @addr=0 MPS=64...\n");
    uint8_t desc18[18];
    rc = get_device_desc_default_addr(desc18);
    if (rc != 0) {
        KLOG_ERROR("[USB] GET_DESCRIPTOR(18) failed: %d\n", rc);
        return rc;
    }
    uint32_t ep0_mps = normalize_ep0_mps(desc18[7]);
    uint8_t  dev_class = desc18[4];
    uint16_t vid = (uint16_t)(desc18[8]  | ((uint16_t)desc18[9]  << 8));
    uint16_t pid = (uint16_t)(desc18[10] | ((uint16_t)desc18[11] << 8));

    KLOG_INFO("[USB]   VID=0x%04x PID=0x%04x class=%u MPS=%lu\n",
              vid, pid, dev_class, (unsigned long)ep0_mps);

    /* 4. SET_ADDRESS（对齐 ep0.rs:376） */
    uint32_t new_addr = 1;
    KLOG_INFO("[USB] SET_ADDRESS %lu (MPS=%lu)...\n",
              (unsigned long)new_addr, (unsigned long)ep0_mps);
    rc = set_usb_address((uint8_t)new_addr, ep0_mps);
    if (rc != 0) {
        KLOG_ERROR("[USB] SET_ADDRESS failed: %d\n", rc);
        return rc;
    }

    /* 5. 等待地址生效（对齐 ep0.rs:328 usb_post_set_address_delay ~80ms） */
    timer_delay_ms(80);

    /* 6. 填充基础结果，随后用配置描述符补充接口/类信息 */
    result->num_devices       = 1;
    result->first_msc_addr    = 0;
    result->first_uvc_addr    = 0;
    result->devices[0].dev_addr           = (uint8_t)new_addr;
    result->devices[0].port               = 0;
    result->devices[0].speed              = (uint8_t)spd;
    result->devices[0].id_vendor          = vid;
    result->devices[0].id_product         = pid;
    result->devices[0].b_device_class     = dev_class;
    result->devices[0].b_max_packet_size0 = (uint8_t)ep0_mps;
    result->devices[0].is_hub = (dev_class == 0x09);
    result->devices[0].is_msc = false;
    result->devices[0].is_uvc = false;

    uint32_t cfg_total = 0;
    uint8_t cfg_value = 1;
    KLOG_INFO("[USB] GET_DESCRIPTOR(CONFIGURATION)...\n");
    rc = read_configuration_descriptor(new_addr, ep0_mps, 0, g_config_desc_buf,
                                       sizeof(g_config_desc_buf), &cfg_total,
                                       &cfg_value);
    if (rc != 0)
        return rc;

    if (uvc_parse_config(g_config_desc_buf, cfg_total, &result->devices[0])) {
        result->first_uvc_addr = (uint8_t)new_addr;
        result->first_uvc_vid = vid;
        result->first_uvc_pid = pid;
    }

    /* 7. SET_CONFIGURATION(bConfigurationValue)（对齐 ep0.rs:385） */
    KLOG_INFO("[USB] SET_CONFIGURATION(%u)...\n", cfg_value);
    rc = set_configuration(new_addr, cfg_value, ep0_mps);
    if (rc != 0) {
        KLOG_ERROR("[USB] SET_CONFIGURATION failed: %d\n", rc);
        return rc;
    }

    if (result->devices[0].is_uvc) {
        rc = uvc_start_video_stream(new_addr, ep0_mps, &result->devices[0]);
        if (rc == 0)
            uvc_video_register_device(new_addr, &result->devices[0]);
        else
            KLOG_WARN("[USB] UVC stream arm failed: %d\n", rc);
    }

    KLOG_INFO("[USB] === Enumerated: addr=%lu VID=0x%04x PID=0x%04x "
              "class=%u MPS=%lu SPD=%s UVC=%u ===\n",
              (unsigned long)new_addr, vid, pid, dev_class,
              (unsigned long)ep0_mps, spd_str,
              result->devices[0].is_uvc ? 1u : 0u);
    return 0;
}

int dwc2_usb_capture_first_uvc_frame(const usb_enumerate_result_t *result,
                                     usb_uvc_frame_t *frame)
{
    if (result == NULL || frame == NULL || result->first_uvc_addr == 0)
        return -1;

    for (uint8_t i = 0; i < result->num_devices && i < USB_MAX_DEVICES; i++) {
        const usb_device_info_t *dev = &result->devices[i];
        if (dev->is_uvc && dev->dev_addr == result->first_uvc_addr)
            return uvc_capture_one_frame(dev->dev_addr, dev, frame);
    }

    return -1;
}
