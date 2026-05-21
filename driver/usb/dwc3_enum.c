/* driver/usb/dwc3_enum.c
 *
 * xHCI USB 设备枚举 + HID 鼠标数据读取
 *
 * 依赖 dwc3.c 的全局状态（g_dwc3_base0/1, g_xhci_hw）。
 * 通过 extern 声明共享。
 *
 * 流程：
 *   1. 扫描所有端口，找到已连接设备（CCS=1）
 *   2. 端口复位（PORTSC.PR=1，等待 PED=1）
 *   3. Enable Slot Command → slot_id
 *   4. Address Device Command（BSR=0，控制器自动发 SET_ADDRESS）
 *   5. Control 传输: GET_DESCRIPTOR(Device) → 获取 bMaxPacketSize0
 *   6. Control 传输: GET_DESCRIPTOR(Configuration) → 解析端点描述符
 *   7. Control 传输: SET_CONFIGURATION(1)
 *   8. HID: SET_PROTOCOL(0=boot protocol)
 *   9. Configure Endpoint Command（EP1 IN）
 *  10. 轮询 Interrupt IN 传输，打印 HID 报告
 *
 * 注意：
 *   - AC64=0：所有 DMA 地址必须 < 4 GB（32 位）
 *   - CSZ=1：上下文条目 64 字节
 *   - mmio_vma=true：所有寄存器地址需加 KERNEL_VMA 偏移
 *   - 无中断，完全轮询
 */

#include "usb/dwc3.h"
#include "mmio.h"
#include "klog.h"
#include "mm_vm.h"
#include "string.h"
#include "cache.h"
/* ─── 枚举资源数据结构 ───────────────────────────────────────────────── */

#define CTX_SZ          64U     /* CSZ=1: 64 字节上下文条目 */
#define CTRL_RING_TRBS  16U     /* EP0 传输环：15 数据 + 1 Link */
#define INTR_RING_TRBS  16U     /* EP1 IN 环 */
#define HID_N_BUFS      4U      /* 预挂起的 Interrupt IN 缓冲区数 */
#define HID_BUF_SZ      8U      /* HID boot 报告最大字节数 */

/* 64 字节上下文条目 */
typedef union {
    uint32_t w[16];
    uint8_t  b[64];
} __attribute__((aligned(64))) ctx_t;

/* Input Context: Control Context + Slot + 4 EP (DCI 1-4) */
typedef struct {
    ctx_t ctrl;       /* Input Control Context */
    ctx_t slot;       /* Slot Context */
    ctx_t ep[4];      /* DCI1=EP0, DCI2=EP1OUT, DCI3=EP1IN, DCI4=EP2OUT */
} __attribute__((aligned(64))) in_ctx_t;

/* Output Device Context: Slot + 4 EP */
typedef struct {
    ctx_t slot;
    ctx_t ep[4];
} __attribute__((aligned(64))) out_ctx_t;

/* 所有枚举资源（BSS 中仅一份，同时枚举一个设备）*/
typedef struct {
    in_ctx_t   in_ctx;
    out_ctx_t  out_ctx;
    xhci_trb_t ctrl_ring[CTRL_RING_TRBS];
    xhci_trb_t intr_ring[INTR_RING_TRBS];
    uint8_t    hid_bufs[HID_N_BUFS][HID_BUF_SZ];
    uint8_t    desc_buf[256];
} __attribute__((aligned(64))) enum_res_t;

static enum_res_t g_enum;

/* ─── 获取 32 位物理地址（AC64=0）───────────────────────────────────── */

static inline uint32_t ep32(const void *va)
{
    return (uint32_t)(virt_to_phys(va) & 0xFFFFFFFFUL);
}

/* ─── 内存屏障 ───────────────────────────────────────────────────────── */

static inline void dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ─── 忙等（不依赖定时器）─────────────────────────────────────────────── */

static void delay_iters(volatile uint32_t n)
{
    while (n--) __asm__ volatile("" ::: "memory");
}

/* ─── xHCI 寄存器读写 ────────────────────────────────────────────────── */

static inline uint32_t op_r32(uintptr_t op, uint32_t off)
{
    return read32((void *)(op + off));
}

static inline void op_w32(uintptr_t op, uint32_t off, uint32_t v)
{
    write32(v, (void *)(op + off));
}

static inline uint32_t rt_r32(uintptr_t rt, uint32_t off)
{
    return read32((void *)(rt + off));
}

static inline void rt_w32(uintptr_t rt, uint32_t off, uint32_t v)
{
    write32(v, (void *)(rt + off));
}

/* ─── 生产者环管理（命令环 / 传输环）───────────────────────────────── */

typedef struct {
    xhci_trb_t   *ring;
    uint32_t  cap;    /* 含 Link TRB 的总槽数 */
    uint32_t  enq;    /* 下一写入索引 */
    uint8_t   pcs;    /* Producer Cycle State */
} ring_t;

/* 初始化一条新的生产者环（Link TRB cycle=~pcs，硬件从 enq=0 PCS=pcs 开始消费）*/
static void ring_init(ring_t *r, xhci_trb_t *trbs, uint32_t cap, uint8_t init_pcs)
{
    r->ring = trbs;
    r->cap  = cap;
    r->enq  = 0U;
    r->pcs  = init_pcs;
    uint32_t lp = cap - 1U;
    trbs[lp].param_lo = ep32(trbs);
    trbs[lp].param_hi = 0U;
    trbs[lp].status   = 0U;
    /* Link TRB cycle = ~pcs（阻止硬件在环满前越过 Link）*/
    trbs[lp].control  = XHCI_TRB_TYPE_LINK | XHCI_TRB_LINK_TC | (uint32_t)(init_pcs ^ 1U);
    clean_dcache_range(&trbs[lp], sizeof(xhci_trb_t));
}

/* 仅初始化环追踪状态（用于 dwc3.c 已建立的命令环）*/
static void ring_attach(ring_t *r, xhci_trb_t *trbs, uint32_t cap, uint8_t pcs)
{
    r->ring = trbs;
    r->cap  = cap;
    r->enq  = 0U;
    r->pcs  = pcs;
}

/* 入队一个 TRB，返回其物理地址；自动处理 Link TRB 回绕 */
static uint32_t ring_enq(ring_t *r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctrl)
{
    uint32_t i    = r->enq;
    uint32_t phys = ep32(&r->ring[i]);

    r->ring[i].param_lo = p0;
    r->ring[i].param_hi = p1;
    r->ring[i].status   = st;
    dsb();
    r->ring[i].control  = ctrl | (uint32_t)r->pcs;
    dsb();
    clean_dcache_range(&r->ring[i], sizeof(xhci_trb_t));

    r->enq++;
    if (r->enq == r->cap - 1U) {
        /* 到达 Link TRB 位置：将 Link TRB cycle 更新为当前 PCS，然后切换 PCS */
        r->ring[r->cap - 1U].control =
            (r->ring[r->cap - 1U].control & ~1U) | (uint32_t)r->pcs;
        dsb();
        clean_dcache_range(&r->ring[r->cap - 1U], sizeof(xhci_trb_t));
        r->pcs ^= 1U;
        r->enq  = 0U;
    }
    return phys;
}

/* ─── 消费者事件环管理 ────────────────────────────────────────────────── */

typedef struct {
    xhci_trb_t   *ring;
    uint32_t  cap;
    uint32_t  deq;    /* 下一读取索引 */
    uint8_t   ccs;    /* Consumer Cycle State（硬件写入起始 cycle=1）*/
    uintptr_t ir0;    /* Interrupter 0 基址（用于更新 ERDP）*/
} evt_t;

static void evt_init(evt_t *e, xhci_trb_t *ring, uint32_t cap, uintptr_t ir0)
{
    e->ring = ring;
    e->cap  = cap;
    e->deq  = 0U;
    e->ccs  = 1U;    /* 硬件以 PCS=1 开始写入事件 */
    e->ir0  = ir0;
}

/* 尝试从事件环取一个 TRB；无事件返回 -1 */
static int evt_try(evt_t *e, xhci_trb_t *out)
{
    invalidate_dcache_range(&e->ring[e->deq], sizeof(xhci_trb_t));
    uint32_t ctrl = e->ring[e->deq].control;
    if ((ctrl & 1U) != (uint32_t)e->ccs)
        return -1;

    *out = e->ring[e->deq];
    e->deq++;
    if (e->deq == e->cap) { e->deq = 0U; e->ccs ^= 1U; }

    /* 更新 ERDP（告知控制器消费者进度）*/
    uint32_t erdp = ep32(&e->ring[e->deq]) | XHCI_ERDP_EHB;
    rt_w32(e->ir0, XHCI_IR_ERDP_LO, erdp);
    rt_w32(e->ir0, XHCI_IR_ERDP_HI, 0U);
    return 0;
}

/* 轮询事件环，最多等待 max_iters 次空轮询 */
static int evt_poll(evt_t *e, xhci_trb_t *out, uint32_t max_iters)
{
    for (uint32_t i = 0; i < max_iters; i++) {
        if (evt_try(e, out) == 0) return 0;
        delay_iters(10U);
    }
    return -1;
}

/* 排空所有待处理事件（启动前的端口状态变化等）*/
static void evt_drain(evt_t *e)
{
    xhci_trb_t ev;
    uint32_t n;
    /* 每次等 200ms（20000 次），最多排空 64 个事件 */
    for (n = 0; n < 64U; n++) {
        if (evt_poll(e, &ev, 20000U) != 0) break;
        uint32_t t = (ev.control >> 10) & 0x3FU;
        KLOG_INFO("dwc3_enum: drain evt[%u] type=%u ctrl=0x%08x\n",
                  n, (unsigned)t, (unsigned)ev.control);
    }
    KLOG_INFO("dwc3_enum: drain done  n=%u  deq=%u\n", n, (unsigned)e->deq);
}

/* 等待命令完成事件（TRB type=33）；返回 0=Success，其他=Completion Code */
static int wait_cmd(evt_t *e, uint8_t *slot_out)
{
    xhci_trb_t ev;
    for (uint32_t tries = 0; tries < 60000U; tries++) {
        if (evt_poll(e, &ev, 500U) == 0) {
            uint32_t t = (ev.control >> 10) & 0x3FU;
            if (t == 33U) {
                uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFU);
                if (slot_out)
                    *slot_out = (uint8_t)((ev.control >> 24) & 0xFFU);
                if (cc != 1U) {
                    KLOG_ERROR("dwc3_enum: cmd CC=%u\n", (unsigned)cc);
                    return -(int)(unsigned)cc;
                }
                return 0;
            }
            KLOG_INFO("dwc3_enum: non-cmd evt type=%u ctrl=0x%08x\n",
                      (unsigned)t, (unsigned)ev.control);
        }
    }
    KLOG_ERROR("dwc3_enum: cmd timeout  deq=%u\n", (unsigned)e->deq);
    KLOG_ERROR("  evt_phys=0x%08x\n", ep32(e->ring));
    KLOG_ERROR("  ERDP=0x%08x\n",
              (unsigned)rt_r32(e->ir0, XHCI_IR_ERDP_LO));
    /* 打印事件环头部诊断（先 invalidate 确保读到 DRAM）*/
    for (uint32_t i = 0; i < 8U; i++) {
        invalidate_dcache_range(&e->ring[i], sizeof(xhci_trb_t));
        KLOG_ERROR("  evt_ring[%u] ctrl=0x%08x\n",
                  i, (unsigned)e->ring[i].control);
        KLOG_ERROR("    st=0x%08x\n", (unsigned)e->ring[i].status);
    }
    return -1;
}

/* 等待传输完成事件（TRB type=32）；返回剩余字节数（>=0）或 <0 表示错误 */
static int wait_xfer(evt_t *e)
{
    xhci_trb_t ev;
    for (uint32_t tries = 0; tries < 6000U; tries++) {
        if (evt_poll(e, &ev, 500U) == 0) {
            uint32_t t = (ev.control >> 10) & 0x3FU;
            if (t == 32U) {
                uint8_t  cc  = (uint8_t)((ev.status >> 24) & 0xFFU);
                uint32_t rem = ev.status & 0x00FFFFFFU;
                if (cc == 1U || cc == 13U) /* Success or Short Packet */
                    return (int)rem;
                KLOG_ERROR("dwc3_enum: xfer CC=%u\n", (unsigned)cc);
                return -(int)(unsigned)cc;
            }
            KLOG_INFO("dwc3_enum: xfer non-xfer evt type=%u ctrl=0x%08x st=0x%08x\n",
                      (unsigned)t, (unsigned)ev.control, (unsigned)ev.status);
        }
    }
    KLOG_ERROR("dwc3_enum: xfer timeout  deq=%u\n", (unsigned)e->deq);
    KLOG_ERROR("  ERDP=0x%08x\n",
              (unsigned)rt_r32(e->ir0, XHCI_IR_ERDP_LO));
    for (uint32_t i = 0; i < 8U; i++) {
        invalidate_dcache_range(&e->ring[i], sizeof(xhci_trb_t));
        KLOG_ERROR("  evt_ring[%u] ctrl=0x%08x st=0x%08x\n",
                  i, (unsigned)e->ring[i].control, (unsigned)e->ring[i].status);
    }
    return -1;
}

/* ─── 门铃 ────────────────────────────────────────────────────────────── */

static inline void ring_db(uintptr_t db_base, uint8_t slot, uint8_t dci)
{
    write32((uint32_t)dci, (void *)(db_base + (uintptr_t)slot * 4U));
}

/* ─── 控制传输（Setup + [Data IN] + Status）───────────────────────────── */

/* IN 数据方向控制传输（GET_DESCRIPTOR 等）*/
static int ctrl_in(evt_t *e, ring_t *cr, uintptr_t db_base,
                   uint8_t slot_id,
                   uint32_t setup_p0, uint32_t setup_p1, uint32_t len,
                   void *buf)
{
    uint32_t buf_phys = ep32(buf);

    /* Setup Stage TRB: type=2, TRT=3(IN data), IDT=1, TRBLen=8 */
    ring_enq(cr, setup_p0, setup_p1, 8U,
             (2U << 10) | (1U << 6) | (3U << 16));

    /* Data Stage TRB: type=3, DIR=IN(bit16=1), no IOC */
    ring_enq(cr, buf_phys, 0U, len,
             (3U << 10) | (1U << 16));

    /* Status Stage TRB: type=4, DIR=OUT(bit16=0, after IN data), IOC */
    ring_enq(cr, 0U, 0U, 0U,
             (4U << 10) | (1U << 5));

    dsb();
    /* debug: print Setup TRB before doorbell */
    invalidate_dcache_range(&cr->ring[0], sizeof(xhci_trb_t) * 3U);
    KLOG_INFO("ctrl_in: DB[%u]=1 at 0x%lx  setup_ctrl=0x%08x  data_ctrl=0x%08x  status_ctrl=0x%08x\n",
              (unsigned)slot_id, (unsigned long)(db_base + (uintptr_t)slot_id * 4U),
              (unsigned)cr->ring[0].control, (unsigned)cr->ring[1].control,
              (unsigned)cr->ring[2].control);
    ring_db(db_base, slot_id, 1U);   /* EP0 DCI=1 */

    int r = wait_xfer(e);
    invalidate_dcache_range(buf, len);
    if (r < 0) return r;
    return (int)len - r;
}

/* 无数据控制传输（SET_CONFIGURATION, SET_PROTOCOL 等）*/
static int ctrl_nodata(evt_t *e, ring_t *cr, uintptr_t db_base,
                       uint8_t slot_id,
                       uint32_t setup_p0, uint32_t setup_p1)
{
    /* Setup Stage TRB: TRT=0(no data) */
    ring_enq(cr, setup_p0, setup_p1, 8U,
             (2U << 10) | (1U << 6) | (0U << 16));

    /* Status Stage TRB: DIR=IN(bit16=1, 无数据时 status 为 IN), IOC */
    ring_enq(cr, 0U, 0U, 0U,
             (4U << 10) | (1U << 5) | (1U << 16));

    dsb();
    ring_db(db_base, slot_id, 1U);

    return wait_xfer(e);
}

/* ─── USB Setup 包宏（xHCI TRB param_lo/param_hi 编码）
 *
 * param_lo = {wValue_hi, wValue_lo, bRequest, bmRequestType} (bytes 3,2,1,0)
 * param_hi = {wLength_hi, wLength_lo, wIndex_hi, wIndex_lo}  (bytes 7,6,5,4)
 * ─────────────────────────────────────────────────────────────────────────*/

/* GET_DESCRIPTOR(Device, len) */
#define SETUP_GET_DEV_DESC(len) \
    0x01000680U, (((uint32_t)(len) & 0xFFU) << 16)

/* GET_DESCRIPTOR(Configuration, len) */
#define SETUP_GET_CFG_DESC(len) \
    0x02000680U, (((uint32_t)(len) & 0xFFU) << 16)

/* SET_CONFIGURATION(val) */
#define SETUP_SET_CONFIGURATION(val) \
    ((uint32_t)((val) & 0xFFU) << 16) | 0x0900U, 0U

/* SET_PROTOCOL(interface, protocol)  — HID Class request */
#define SETUP_SET_PROTOCOL(iface, proto) \
    ((uint32_t)((proto) & 0xFFU) << 16) | 0x0B21U, \
    ((uint32_t)((iface) & 0xFFU))

/* ─── 描述符解析结果 ─────────────────────────────────────────────────── */

static uint8_t  g_ep_addr      = 0;    /* Interrupt IN 端点地址 */
static uint16_t g_ep_maxpkt    = 4U;   /* wMaxPacketSize */
static uint8_t  g_ep_interval  = 8U;   /* bInterval（帧数，FS=1ms/帧）*/
static uint8_t  g_hid_iface    = 0U;   /* HID 接口编号 */
static uint8_t  g_ep_dci       = 3U;   /* DCI = ep_num*2 + 1(IN) */

/* 遍历完整配置描述符，找到 HID boot 中断 IN 端点 */
static void parse_config(const uint8_t *buf, uint32_t len)
{
    uint32_t pos     = 0;
    uint8_t  cur_if  = 0;
    int      hid_ok  = 0;

    while (pos + 2U <= len) {
        uint8_t bLen  = buf[pos];
        uint8_t bType = buf[pos + 1U];

        if (bLen < 2U || pos + bLen > len) break;

        if (bType == 4U && bLen >= 9U) {                    /* Interface Descriptor */
            cur_if = buf[pos + 2U];
            /* bInterfaceClass=3(HID), bInterfaceSubClass=1(boot) */
            hid_ok = (buf[pos + 5U] == 3U && buf[pos + 6U] == 1U) ? 1 : 0;
            if (hid_ok)
                g_hid_iface = cur_if;
        } else if (bType == 5U && bLen >= 7U && hid_ok) {  /* Endpoint Descriptor */
            uint8_t  addr = buf[pos + 2U];
            uint8_t  attr = buf[pos + 3U];
            uint16_t mpkt = (uint16_t)buf[pos + 4U] |
                            ((uint16_t)buf[pos + 5U] << 8);
            if ((addr & 0x80U) && (attr & 0x3U) == 3U) {   /* IN + Interrupt */
                g_ep_addr     = addr;
                g_ep_maxpkt   = mpkt;
                g_ep_interval = buf[pos + 6U];
                g_ep_dci      = (uint8_t)(((addr & 0xFU) * 2U) + 1U);
                KLOG_INFO("dwc3_enum: HID EP addr=0x%02x maxpkt=%u\n", addr, mpkt);
                KLOG_INFO("  interval=%u dci=%u\n", g_ep_interval, g_ep_dci);
            }
        }
        pos += bLen;
    }
}

/* ─── Configure Endpoint Command 辅助 ────────────────────────────────── */

/* 设置 Input Context 用于 Configure Endpoint：
 *   A0=1（Slot 始终），A_dci=1（新端点），Context Entries 取最大 DCI */
static void prepare_cfg_ep_ctx(in_ctx_t *in, out_ctx_t *out,
                                uint8_t dci,
                                uint32_t intr_ring_phys,
                                uint16_t maxpkt, uint8_t interval_xhci)
{
    /* 从输出上下文复制 Slot Context，更新 Context Entries */
    in->slot = out->slot;
    /* Context Entries = max(already, dci) */
    uint32_t ce = (in->slot.w[0] >> 27) & 0x1FU;
    if ((uint32_t)dci > ce) ce = dci;
    in->slot.w[0] = (in->slot.w[0] & ~(0x1FU << 27)) | ((ce & 0x1FU) << 27);

    /* Input Control Context: Drop=0, Add = (1<<0)|(1<<dci) */
    in->ctrl.w[0] = 0U;
    in->ctrl.w[1] = (1U << 0) | (1U << (uint32_t)dci);

    /* EP Context at index [dci-1] (DCI1→ep[0], DCI2→ep[1], DCI3→ep[2]) */
    ctx_t *ep = &in->ep[dci - 1U];
    for (int i = 0; i < 16; i++) ep->w[i] = 0;

    /* DW0: Interval */
    ep->w[0] = ((uint32_t)interval_xhci << 16);
    /* DW1: MaxPacketSize | EP Type=7(Interrupt IN) | CErr=3 */
    ep->w[1] = ((uint32_t)maxpkt << 16) | (7U << 3) | 3U;
    /* DW2: TR Dequeue Pointer Lo | DCS=1 */
    ep->w[2] = (intr_ring_phys & ~0xFU) | 1U;
    /* DW3: TR Dequeue Hi = 0 (AC64=0) */
    ep->w[3] = 0U;
    /* DW4: Average TRB Length = maxpkt */
    ep->w[4] = (uint32_t)maxpkt;
}

/* ─── 主入口 ─────────────────────────────────────────────────────────── */

void dwc3_hid_enumerate(void)
{
    KLOG_INFO("dwc3_enum: === USB HID enum start ===\n");

    /* ── 找到第一个有连接设备的控制器和端口 ── */
    int      ctl  = -1;
    uint32_t port =  0;
    uint32_t portsc0 = 0;

    uintptr_t bases[2] = { g_dwc3_base0, g_dwc3_base1 };
    for (int ci = 0; ci < 2; ci++) {
        if (bases[ci] == 0) continue;
        uint32_t caplen = op_r32(bases[ci], XHCI_CAP_VER) & 0xFFU;
        uintptr_t op    = bases[ci] + caplen;
        uint32_t  h1    = op_r32(bases[ci], XHCI_HCSPARAMS1);
        uint8_t   np    = (uint8_t)((h1 >> 24) & 0xFFU);
        for (uint8_t p = 0; p < np; p++) {
            uint32_t sc = op_r32(op, XHCI_OP_PORTSC(p));
            if (sc & XHCI_PORTSC_CCS) {
                ctl = ci; port = p; portsc0 = sc; break;
            }
        }
        if (ctl >= 0) break;
    }

    if (ctl < 0) {
        KLOG_WARN("dwc3_enum: No connected device, skipping enumeration\n");
        return;
    }

    uintptr_t base   = bases[ctl];
    uint32_t  caplen = op_r32(base, XHCI_CAP_VER) & 0xFFU;
    uintptr_t op_b   = base + caplen;
    uint32_t  rtsoff = op_r32(base, XHCI_RTSOFF) & ~0x1FU;
    uintptr_t rt_b   = base + rtsoff;
    uintptr_t ir0    = rt_b + XHCI_IR0_OFF;
    uint32_t  dboff  = op_r32(base, XHCI_DBOFF) & ~0x3U;
    uintptr_t db_b   = base + dboff;

    uint8_t  port_speed = (uint8_t)((portsc0 >> 10) & 0xFU);
    KLOG_INFO("dwc3_enum: [OTG%d] port %u\n", ctl, port + 1U);
    KLOG_INFO("  speed=%u starting enumeration\n", port_speed);
    KLOG_INFO("  dboff=0x%08x  db_b=0x%lx\n", (unsigned)dboff, (unsigned long)db_b);

    /* ── Initialize tracking state for each ring ── */
    ring_t cmd_r;
    evt_t  evt;

    /* 命令环：dwc3.c 在 xhci_start_one 中已建立，PCS=1 无命令入队 */
    ring_attach(&cmd_r, g_xhci_hw[ctl].cmd_ring, XHCI_CMD_RING_TRBS, 1U);
    /* 事件环：消费者从 deq=0 CCS=1 开始 */
    evt_init(&evt, g_xhci_hw[ctl].evt_ring, XHCI_EVT_RING_TRBS, ir0);

    /* 清空因端口连接产生的 PSC 事件 */
    evt_drain(&evt);

    /* EP0 传输环（全新）*/
    ring_t ctrl_r;
    ring_init(&ctrl_r, g_enum.ctrl_ring, CTRL_RING_TRBS, 1U);

    /* ── 步骤 1: 端口复位 ── */
    KLOG_INFO("dwc3_enum: port reset...\n");

    /* 清除 CSC 并置位 PR；保留 PP */
    uint32_t sc = op_r32(op_b, XHCI_OP_PORTSC(port));
    sc  = (sc & 0x0E01FEC0U);   /* 保留 RW 字段，清零 RW1C */
    sc |= XHCI_PORTSC_PR;
    op_w32(op_b, XHCI_OP_PORTSC(port), sc);
    dsb();

    /* 等待 PR 清零且 PED=1（端口已使能）*/
    uint32_t timeout = 10000000U;
    do {
        delay_iters(50U);
        sc = op_r32(op_b, XHCI_OP_PORTSC(port));
        timeout--;
    } while ((sc & XHCI_PORTSC_PR) && timeout);

    if (sc & XHCI_PORTSC_PR) {
        KLOG_ERROR("dwc3_enum: port reset timeout  PORTSC=0x%08x\n", sc);
        return;
    }

    /* 清除 PRC（Port Reset Change，bit21）和 CSC（bit17）*/
    op_w32(op_b, XHCI_OP_PORTSC(port),
           (sc & 0x0E01FEC0U) | (1U << 21) | (1U << 17));
    delay_iters(500000U);   /* 等待枚举速度稳定 */

    /* 重新读取速度（复位后确定）*/
    sc = op_r32(op_b, XHCI_OP_PORTSC(port));
    port_speed = (uint8_t)((sc >> 10) & 0xFU);
    const char *spd_str = (port_speed == 1U) ? "FullSpeed"  :
                          (port_speed == 2U) ? "LowSpeed"   :
                          (port_speed == 3U) ? "HighSpeed"  :
                          (port_speed == 4U) ? "SuperSpeed" : "?";
    KLOG_INFO("dwc3_enum: port reset complete  speed=%s  PORTSC=0x%08x\n",
              spd_str, sc);

    /* 等待所有端口状态变化事件到达，然后排空 */
    delay_iters(2000000U);
    evt_drain(&evt);

    /* ── 步骤 2: Enable Slot Command ── */
    ring_enq(&cmd_r, 0U, 0U, 0U, 9U << 10);
    dsb();
    write32(0U, (void *)db_b);   /* Ring HC doorbell DB[0]=0 */
    dsb();
    KLOG_INFO("dwc3_enum: Enable Slot command issued  cmd_ring[0]=0x%08x  USBSTS=0x%08x\n",
              (unsigned)g_xhci_hw[ctl].cmd_ring[0].control,
              (unsigned)op_r32(op_b, XHCI_OP_USBSTS));

    uint8_t slot_id = 0;
    if (wait_cmd(&evt, &slot_id) != 0) {
        KLOG_ERROR("dwc3_enum: Enable Slot command failed\n");
        return;
    }
    KLOG_INFO("dwc3_enum: slot_id=%u\n", (unsigned)slot_id);

    /* ── 步骤 3: 初始化 Input Context，发 Address Device ── */

    /* 清零上下文区域 */
    memset(&g_enum.in_ctx,  0, sizeof(g_enum.in_ctx));
    memset(&g_enum.out_ctx, 0, sizeof(g_enum.out_ctx));

    in_ctx_t  *in  = &g_enum.in_ctx;
    out_ctx_t *out = &g_enum.out_ctx;

    /* Input Control Context: A0=1(Slot), A1=1(EP0) */
    in->ctrl.w[0] = 0U;
    in->ctrl.w[1] = 0x3U;

    /* Slot Context DW0: Context Entries=1, Speed, Route=0 */
    in->slot.w[0] = (1U << 27) | ((uint32_t)port_speed << 20);
    /* Slot Context DW1: Root Hub Port Number (1-based) */
    in->slot.w[1] = ((uint32_t)(port + 1U) << 16);

    /* EP0 Context (ep[0] = DCI1):
     *   DW0: Interval=0
     *   DW1: MaxPkt=8(FS初始), EP Type=4(Ctrl Bidir), CErr=3
     *   DW2: TR Dequeue Ptr Lo | DCS=1
     *   DW4: Average TRB Length=8 */
    uint16_t ep0_maxpkt = (port_speed == 3U) ? 64U : 8U; /* HS=64, FS=8 */
    in->ep[0].w[0] = 0U;
    /* DW1: MaxPkt[31:16] | CErr=3 [10:8] | EP Type=4 Ctrl Bidir [5:3] | EP State=0 [2:0] */
    in->ep[0].w[1] = ((uint32_t)ep0_maxpkt << 16) | (3U << 8) | (4U << 3);
    in->ep[0].w[2] = ep32(g_enum.ctrl_ring) | 1U;   /* DCS=1 */
    in->ep[0].w[3] = 0U;
    in->ep[0].w[4] = 8U;

    /* 打印 Input Context 关键字段（flush 前确认值正确）*/
    KLOG_INFO("dwc3_enum: in_ctx:\n");
    KLOG_INFO("  ctrl w1=0x%08x\n",       (unsigned)in->ctrl.w[1]);
    KLOG_INFO("  slot w0=0x%08x\n",       (unsigned)in->slot.w[0]);
    KLOG_INFO("  slot w1=0x%08x\n",       (unsigned)in->slot.w[1]);
    KLOG_INFO("  ep0 w1=0x%08x\n",        (unsigned)in->ep[0].w[1]);
    KLOG_INFO("  ep0 w2=0x%08x\n",        (unsigned)in->ep[0].w[2]);
    KLOG_INFO("  ep0 w4=0x%08x\n",        (unsigned)in->ep[0].w[4]);

    /* 设置 DCBAA[slot_id] = 输出设备上下文物理地址 */
    g_xhci_hw[ctl].dcbaa[slot_id] = (uint64_t)ep32(out);
    dsb();
    /* 刷新所有 HC 需要 DMA 读/写的结构到 DRAM */
    clean_dcache_range(in,  sizeof(in_ctx_t));
    clean_dcache_range(out, sizeof(out_ctx_t));   /* HC 将写入 out_ctx，先 clean 避免 dirty 回写覆盖 */
    clean_dcache_range(g_enum.ctrl_ring, sizeof(xhci_trb_t) * CTRL_RING_TRBS); /* HC 读 EP0 TR */
    clean_dcache_range(&g_xhci_hw[ctl].dcbaa[slot_id], sizeof(uint64_t));

    KLOG_INFO("dwc3_enum: in_phys=0x%08x\n",  ep32(in));
    KLOG_INFO("  out_phys=0x%08x\n",           ep32(out));
    KLOG_INFO("  ctrl_ring_phys=0x%08x\n",     ep32(g_enum.ctrl_ring));
    KLOG_INFO("  dcbaa[%u]=0x%08x\n",
              (unsigned)slot_id,
              (unsigned)g_xhci_hw[ctl].dcbaa[slot_id]);

    /* 等待 HC 完成 Enable Slot 内部状态更新后再发 Address Device */
    delay_iters(2000000U);

    /* Address Device Command: type=11, BSR=1(先不发 SET_ADDRESS), slot_id<<24
     * BSR=1: HC 只初始化 Slot/EP0 Context，不向设备发 USB SET_ADDRESS。
     * 用于验证 Input Context 参数正确性；成功后再发 BSR=0 完成地址分配。 */
    ring_enq(&cmd_r, ep32(in), 0U, 0U,
             (11U << 10) | (1U << 9) | ((uint32_t)slot_id << 24));
    dsb();
    write32(0U, (void *)db_b);
    KLOG_INFO("dwc3_enum: AddrDev sent  USBSTS=0x%08x\n",
              (unsigned)op_r32(op_b, XHCI_OP_USBSTS));

    if (wait_cmd(&evt, NULL) != 0) {
        /* 额外诊断：HC 是否在处理命令环？*/
        KLOG_ERROR("dwc3_enum: AddrDev fail  USBSTS=0x%08x\n",
                   (unsigned)op_r32(op_b, XHCI_OP_USBSTS));
        KLOG_ERROR("  CRCR_LO=0x%08x\n",
                   (unsigned)op_r32(op_b, XHCI_OP_CRCR_LO));
        /* 读 DRAM 中的 cmd_ring[1] (Address Device TRB) */
        invalidate_dcache_range(&g_xhci_hw[ctl].cmd_ring[1],
                                sizeof(xhci_trb_t));
        KLOG_ERROR("  cmd_ring[1] ctrl=0x%08x\n",
                   (unsigned)g_xhci_hw[ctl].cmd_ring[1].control);
        KLOG_ERROR("    p_lo=0x%08x\n",
                   (unsigned)g_xhci_hw[ctl].cmd_ring[1].param_lo);
        /* HC 是否写了 out_ctx 的一部分？*/
        invalidate_dcache_range(out, sizeof(out_ctx_t));
        KLOG_ERROR("  out_ctx slot.w[0]=0x%08x\n", (unsigned)out->slot.w[0]);
        KLOG_ERROR("  out_ctx slot.w[3]=0x%08x\n", (unsigned)out->slot.w[3]);
        return;
    }
    invalidate_dcache_range(out, sizeof(out_ctx_t));
    KLOG_INFO("dwc3_enum: device addressed\n");
    /* dump EP0 context to verify TR Dequeue Ptr */
    KLOG_INFO("  out_ctx.ep0.w[0]=0x%08x  w[1]=0x%08x\n",
              (unsigned)out->ep[0].w[0], (unsigned)out->ep[0].w[1]);
    KLOG_INFO("  out_ctx.ep0.w[2]=0x%08x  w[4]=0x%08x\n",
              (unsigned)out->ep[0].w[2], (unsigned)out->ep[0].w[4]);
    KLOG_INFO("  ctrl_ring_phys|DCS=0x%08x\n", ep32(g_enum.ctrl_ring) | 1U);

    /* ── 步骤 3a-check: No-Op 命令（验证命令环在 BSR=1 后仍可工作）── */
    KLOG_INFO("dwc3_enum: sending No-Op command (cmd ring health check)\n");
    ring_enq(&cmd_r, 0U, 0U, 0U, (23U << 10));
    dsb();
    write32(0U, (void *)db_b);
    dsb();
    {
        int nop_rc = wait_cmd(&evt, NULL);
        KLOG_INFO("dwc3_enum: No-Op rc=%d  USBSTS=0x%08x\n",
                  nop_rc, (unsigned)op_r32(op_b, XHCI_OP_USBSTS));
        if (nop_rc != 0) {
            KLOG_ERROR("dwc3_enum: No-Op FAILED rc=%d -- command ring broken\n", nop_rc);
            return;
        }
    }

    /* ── 步骤 3b: Address Device BSR=0 → 发送 SET_ADDRESS ─────────────
     * BSR=1 仅初始化上下文；DWC3 xHCI 要求随后发 BSR=0 才允许 EP0 doorbell
     * ──────────────────────────────────────────────────────────────────*/
    /* 先确认端口仍在 U0 状态 */
    {
        uint32_t sc_before = op_r32(op_b, XHCI_OP_PORTSC(port));
        KLOG_INFO("dwc3_enum: PORTSC before BSR=0=0x%08x  PE=%u  PLS=%u  CCS=%u\n",
                  (unsigned)sc_before,
                  (unsigned)((sc_before >> 1) & 1U),
                  (unsigned)((sc_before >> 5) & 0xFU),
                  (unsigned)(sc_before & 1U));
    }
    KLOG_INFO("dwc3_enum: sending Address Device BSR=0 (SET_ADDRESS)\n");
    /* 重新刷新 in_ctx 到 DRAM（防止缓存被 invalidate 弄脏）*/
    clean_dcache_range(in,  sizeof(in_ctx_t));
    clean_dcache_range(out, sizeof(out_ctx_t));
    {
        uint32_t trb_phys = ring_enq(&cmd_r, ep32(in), 0U, 0U,
                 (11U << 10) | (0U << 9) | ((uint32_t)slot_id << 24));  /* BSR=0 */
        KLOG_INFO("dwc3_enum: BSR=0 TRB phys=0x%08x  enq=%u\n",
                  (unsigned)trb_phys, (unsigned)cmd_r.enq);
    }
    KLOG_INFO("dwc3_enum: CRCR_LO before db=0x%08x\n",
              (unsigned)op_r32(op_b, XHCI_OP_CRCR_LO));
    dsb();
    write32(0U, (void *)db_b);   /* Ring HC doorbell DB[0]=0 */
    dsb();
    delay_iters(10000U);
    KLOG_INFO("dwc3_enum: CRCR_LO after  db=0x%08x  USBSTS=0x%08x\n",
              (unsigned)op_r32(op_b, XHCI_OP_CRCR_LO),
              (unsigned)op_r32(op_b, XHCI_OP_USBSTS));
    {
        int rc = wait_cmd(&evt, NULL);
        /* 打印超时时的 PORTSC 和 out_ctx.slot 状态 */
        {
            uint32_t sc_after = op_r32(op_b, XHCI_OP_PORTSC(port));
            KLOG_INFO("dwc3_enum: PORTSC after BSR=0=0x%08x  PE=%u  PLS=%u  CCS=%u\n",
                      (unsigned)sc_after,
                      (unsigned)((sc_after >> 1) & 1U),
                      (unsigned)((sc_after >> 5) & 0xFU),
                      (unsigned)(sc_after & 1U));
        }
        invalidate_dcache_range(out, sizeof(out_ctx_t));
        KLOG_INFO("dwc3_enum: out_ctx.slot.w[0]=0x%08x  w[3]=0x%08x\n",
                  (unsigned)out->slot.w[0], (unsigned)out->slot.w[3]);
        /* slot state = out->slot.w[3] bits[26:24] */
        KLOG_INFO("  slot_state=%u  usb_addr=%u\n",
                  (unsigned)((out->slot.w[3] >> 27) & 0x1FU),
                  (unsigned)(out->slot.w[3] & 0xFFU));
        KLOG_INFO("dwc3_enum: BSR=0 wait_cmd rc=%d  USBSTS=0x%08x\n",
                  rc, (unsigned)op_r32(op_b, XHCI_OP_USBSTS));
        if (rc != 0) {
            KLOG_ERROR("dwc3_enum: Address Device BSR=0 failed rc=%d\n", rc);
            return;
        }
    }
    KLOG_INFO("dwc3_enum: device SET_ADDRESS done\n");

    /* ── 步骤 4: GET_DESCRIPTOR(Device, 8) → 获取 bMaxPacketSize0 ── */
    memset(g_enum.desc_buf, 0, sizeof(g_enum.desc_buf));
    int n = ctrl_in(&evt, &ctrl_r, db_b, slot_id,
                    SETUP_GET_DEV_DESC(8), 8U, g_enum.desc_buf);
    if (n < 8) {
        KLOG_ERROR("dwc3_enum: GET_DESCRIPTOR(Dev,8) failed n=%d\n", n);
        /* check if HC advanced TR Dequeue Pointer (=TRBs consumed) */
        invalidate_dcache_range(out, sizeof(out_ctx_t));
        KLOG_ERROR("  ep0.w[2] after xfer=0x%08x (expected 0x%08x if advanced)\n",
                   (unsigned)out->ep[0].w[2],
                   (ep32(g_enum.ctrl_ring) + 3U * (uint32_t)sizeof(xhci_trb_t)) | 1U);
        KLOG_ERROR("  USBSTS=0x%08x\n", (unsigned)op_r32(op_b, XHCI_OP_USBSTS));
        return;
    }
    uint8_t bMaxPkt0 = g_enum.desc_buf[7];
    KLOG_INFO("dwc3_enum: bcdUSB=%02x%02x\n", g_enum.desc_buf[3], g_enum.desc_buf[2]);
    KLOG_INFO("  bDevClass=0x%02x bMaxPkt0=%u\n", g_enum.desc_buf[4], bMaxPkt0);

    /* 更新 EP0 MaxPacketSize（如果和初始值不同）*/
    if ((uint16_t)bMaxPkt0 != ep0_maxpkt) {
        /* Evaluate Context Command: type=13, update EP0 maxpkt */
        ep0_maxpkt = bMaxPkt0;
        in->ctrl.w[1] = 0x2U;   /* A1=EP0 only */
        in->ep[0].w[1] = ((uint32_t)ep0_maxpkt << 16) | (4U << 3) | 3U;
        dsb();
        clean_dcache_range(in, sizeof(in_ctx_t));
        ring_enq(&cmd_r, ep32(in), 0U, 0U,
                 (13U << 10) | ((uint32_t)slot_id << 24));
        write32(0U, (void *)db_b);
        if (wait_cmd(&evt, NULL) != 0)
            KLOG_WARN("dwc3_enum: Evaluate Context failed, continuing\n");
        else
            KLOG_INFO("dwc3_enum: EP0 MaxPkt updated to %u\n", bMaxPkt0);
    }

    /* ── 步骤 5: GET_DESCRIPTOR(Device, 18) → 完整设备描述符 ── */
    memset(g_enum.desc_buf, 0, sizeof(g_enum.desc_buf));
    n = ctrl_in(&evt, &ctrl_r, db_b, slot_id,
                SETUP_GET_DEV_DESC(18), 18U, g_enum.desc_buf);
    if (n >= 8) {
        KLOG_INFO("dwc3_enum: VID=0x%04x PID=0x%04x\n",
                  (uint32_t)g_enum.desc_buf[8]  | ((uint32_t)g_enum.desc_buf[9]  << 8),
                  (uint32_t)g_enum.desc_buf[10] | ((uint32_t)g_enum.desc_buf[11] << 8));
    }

    /* ── 步骤 6: GET_DESCRIPTOR(Configuration, 9) → wTotalLength ── */
    memset(g_enum.desc_buf, 0, 9);
    n = ctrl_in(&evt, &ctrl_r, db_b, slot_id,
                SETUP_GET_CFG_DESC(9), 9U, g_enum.desc_buf);
    if (n < 9) {
        KLOG_ERROR("dwc3_enum: GET_DESCRIPTOR(Cfg,9) failed n=%d\n", n);
        return;
    }
    uint16_t total_len = (uint16_t)g_enum.desc_buf[2] |
                         ((uint16_t)g_enum.desc_buf[3] << 8);
    if (total_len > (uint16_t)sizeof(g_enum.desc_buf))
        total_len = (uint16_t)sizeof(g_enum.desc_buf);

    KLOG_INFO("dwc3_enum: Configuration wTotalLength=%u bNumIf=%u\n",
              total_len, g_enum.desc_buf[4]);

    /* ── 步骤 7: GET_DESCRIPTOR(Configuration, wTotalLength) → 解析端点 ── */
    memset(g_enum.desc_buf, 0, sizeof(g_enum.desc_buf));
    n = ctrl_in(&evt, &ctrl_r, db_b, slot_id,
                SETUP_GET_CFG_DESC(total_len), (uint32_t)total_len, g_enum.desc_buf);
    if (n < 9) {
        KLOG_ERROR("dwc3_enum: GET_DESCRIPTOR(Cfg,full) failed n=%d\n", n);
        return;
    }
    parse_config(g_enum.desc_buf, (uint32_t)n);

    if (!g_ep_addr) {
        KLOG_ERROR("dwc3_enum: HID Interrupt IN endpoint not found\n");
        return;
    }

    /* ── 步骤 8: SET_CONFIGURATION(1) ── */
    if (ctrl_nodata(&evt, &ctrl_r, db_b, slot_id,
                    SETUP_SET_CONFIGURATION(1)) < 0) {
        KLOG_ERROR("dwc3_enum: SET_CONFIGURATION failed\n");
        return;
    }
    KLOG_INFO("dwc3_enum: SET_CONFIGURATION(1) OK\n");

    /* ── 步骤 9: SET_PROTOCOL(boot=0) ── */
    if (ctrl_nodata(&evt, &ctrl_r, db_b, slot_id,
                    SETUP_SET_PROTOCOL(g_hid_iface, 0)) < 0) {
        KLOG_WARN("dwc3_enum: SET_PROTOCOL failed (continuing)\n");
    } else {
        KLOG_INFO("dwc3_enum: SET_PROTOCOL(boot) OK  iface=%u\n", g_hid_iface);
    }

    /* ── 步骤 10: 配置 Interrupt IN 端点（Configure Endpoint Command）── */

    /*
     * xHCI 间隔 = ceil(log2(bInterval * 8)) + 1  [FS, 微帧单位]
     * 简化：bInterval <= 8 → interval_xhci = 4 (2^3 * 125µs = 1ms)
     *        bInterval > 8 → interval_xhci = 7 (2^6 * 125µs = 8ms)
     */
    uint8_t interval_xhci = (g_ep_interval <= 8U) ? 4U : 7U;

    ring_t intr_r;
    ring_init(&intr_r, g_enum.intr_ring, INTR_RING_TRBS, 1U);

    prepare_cfg_ep_ctx(&g_enum.in_ctx, &g_enum.out_ctx,
                       g_ep_dci,
                       ep32(g_enum.intr_ring),
                       g_ep_maxpkt, interval_xhci);
    dsb();

    clean_dcache_range(&g_enum.in_ctx, sizeof(in_ctx_t));
    clean_dcache_range(g_enum.intr_ring, sizeof(xhci_trb_t) * INTR_RING_TRBS);
    ring_enq(&cmd_r, ep32(&g_enum.in_ctx), 0U, 0U,
             (12U << 10) | ((uint32_t)slot_id << 24));
    write32(0U, (void *)db_b);

    if (wait_cmd(&evt, NULL) != 0) {
        KLOG_ERROR("dwc3_enum: Configure Endpoint failed\n");
        return;
    }
    KLOG_INFO("dwc3_enum: EP%u IN configured\n", g_ep_addr & 0xFU);
    KLOG_INFO("  maxpkt=%u interval=%u\n", g_ep_maxpkt, interval_xhci);

    /* ── 步骤 11: 预挂起 Interrupt IN TRB，轮询 HID 报告 ── */
    KLOG_INFO("dwc3_enum: === Start reading HID reports (50 times) ===\n");

    for (uint32_t rep = 0; rep < 50U; rep++) {
        /* 清空缓冲区 */
        uint8_t *buf = g_enum.hid_bufs[rep % HID_N_BUFS];
        memset(buf, 0, HID_BUF_SZ);
        clean_dcache_range(buf, HID_BUF_SZ);

        /* 挂起一个 Normal TRB */
        ring_enq(&intr_r,
                 ep32(buf), 0U,
                 (uint32_t)g_ep_maxpkt,
                 (1U << 10) | (1U << 5));   /* type=1(Normal), IOC */
        dsb();
        ring_db(db_b, slot_id, (uint8_t)g_ep_dci);

        /* 等待传输完成事件 */
        int r = wait_xfer(&evt);
        invalidate_dcache_range(buf, HID_BUF_SZ);
        if (r < 0) {
            KLOG_ERROR("dwc3_enum: HID read failed rep=%u r=%d\n", rep, r);
            break;
        }

        uint32_t actual = (uint32_t)g_ep_maxpkt - (uint32_t)r;
        /* HID boot mouse report: byte0=buttons, byte1=dx, byte2=dy, byte3=wheel */
        KLOG_INFO("dwc3_enum: HID[%02u] len=%u\n", rep, actual);
        KLOG_INFO("  btn=0x%02x dx=%d\n", buf[0], (int8_t)buf[1]);
        KLOG_INFO("  dy=%d wh=%d\n", (int8_t)buf[2], (int8_t)buf[3]);

        delay_iters(500000U);   /* ~1.5ms 间隔，避免刷屏 */
    }

    KLOG_INFO("dwc3_enum: === HID enumeration complete ===\n");
}
