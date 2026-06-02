/* driver/usb/dwc3_enum.c
 *
 * xHCI USB 设备枚举 + HID 鼠标数据读取
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 依赖关系
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 依赖 dwc3.c 的全局状态（g_dwc3_base0/1, g_xhci_hw），通过 extern 声明共享。
 * 必须在 dwc3_xhci_start() 之后调用。
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 硬件约束
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * - AC64=0：所有 DMA 地址必须 < 4 GB（32 位物理地址）
 * - CSZ=1：上下文条目 64 字节（非 32 字节）
 * - mmio_vma=true：所有寄存器地址需加 KERNEL_VMA 偏移
 * - 无中断：完全轮询模式
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 枚举流程
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 阶段 1: 端口发现
 *   - 扫描所有端口，找到已连接设备（CCS=1）
 *
 * 阶段 2: 端口复位
 *   - PORTSC.PR=1，等待 PR 清零且 PED=1
 *   - 复位后确定设备速度（FS/LS/HS/SS）
 *
 * 阶段 3: Enable Slot
 *   - 发送 Enable Slot Command
 *   - 获得 slot_id（1~64）
 *
 * 阶段 4: Address Device（BSR=0）
 *   - BSR=0: xHC 自动发送 SET_ADDRESS，设备进入 Addressed 状态
 *   - 设备获得由 xHC 分配的 USB 地址
 *   - 后续通过 Control EP 发送标准请求
 *
 * 阶段 5: GET_DESCRIPTOR(Device, 8)
 *   - 获取设备描述符前 8 字节
 *   - 提取 bMaxPacketSize0
 *
 * 阶段 6: GET_DESCRIPTOR(Device, 18)
 *   - 获取完整设备描述符
 *   - 提取 VID/PID
 *
 * 阶段 7: GET_DESCRIPTOR(Configuration, wTotalLength)
 *   - 获取完整配置描述符
 *   - 解析端点描述符，找到 HID Interrupt IN 端点
 *
 * 阶段 8: SET_CONFIGURATION(1)
 *   - 激活配置，使能端点
 *
 * 阶段 9: SET_PROTOCOL(boot=0)
 *   - HID 类请求，设置为 Boot Protocol
 *
 * 阶段 10: Configure Endpoint
 *   - 配置 HID Interrupt IN 端点
 *
 * 阶段 11: 读取 HID 报告
 *   - 轮询 Interrupt IN 传输
 *   - 打印鼠标按钮和移动数据
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * TRB Ring 管理
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Producer Ring（命令环 / 传输环）:
 *   - 软件写入 TRB，设置 cycle bit = PCS
 *   - 硬件消费 TRB，检查 cycle bit == CCS
 *   - Link TRB cycle bit 必须与当前 PCS 相同（TC=1 时）
 *
 * Consumer Ring（事件环）:
 *   - 硬件写入事件 TRB，cycle bit = PCS
 *   - 软件读取 TRB，检查 cycle bit == CCS
 *   - 更新 ERDP 告知硬件消费进度
 *   - 清除 IMAN.IP 使能新事件
 */

#include "usb/dwc3.h"
#include "mmio.h"
#include "klog.h"
#include "mm_vm.h"
#include "string.h"
#include "cache.h"
#include "timer/timer.h"
/* ─── 枚举资源数据结构 ───────────────────────────────────────────────── */

#define CTX_SZ          64U     /* CSZ=1: 64 字节上下文条目 */
#define CTRL_RING_TRBS  16U     /* EP0 传输环：15 数据 + 1 Link */
#define INTR_RING_TRBS  16U     /* EP1 IN 环 */
#define HID_N_BUFS      4U      /* 预挂起的 Interrupt IN 缓冲区数 */
#define HID_BUF_SZ      8U      /* HID boot 报告最大字节数 */
#define XHCI_NUM_EP_CTX 31U     /* Device Context: EP Context DCI 1..31 */

/* 64 字节上下文条目 */
typedef union {
    uint32_t w[16];
    uint8_t  b[64];
} __attribute__((aligned(64))) ctx_t;

/* Input Context: Control Context + full Device Context (Slot + DCI 1..31) */
typedef struct {
    ctx_t ctrl;       /* Input Control Context */
    ctx_t slot;       /* Slot Context */
    ctx_t ep[XHCI_NUM_EP_CTX];
} __attribute__((aligned(64))) in_ctx_t;

/* Output Device Context: Slot + full EP context array */
typedef struct {
    ctx_t slot;
    ctx_t ep[XHCI_NUM_EP_CTX];
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

STATIC_ASSERT(sizeof(ctx_t) == CTX_SZ, xhci_ctx_is_64_bytes);
STATIC_ASSERT(sizeof(in_ctx_t) == (CTX_SZ * (2U + XHCI_NUM_EP_CTX)), xhci_input_context_size);
STATIC_ASSERT(sizeof(out_ctx_t) == (CTX_SZ * (1U + XHCI_NUM_EP_CTX)), xhci_output_context_size);


/* 强制 64 字节对齐：使用多重对齐属性确保链接器正确放置 */
static enum_res_t g_enum __attribute__((section(".data"))) __attribute__((aligned(64)));

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

/* ─── 忙等（使用硬件定时器计数器）──────────────────────────────────────── */

static inline void delay_us(uint32_t us)
{
    timer_delay_us(us);
}

static inline void delay_ms(uint32_t ms)
{
    timer_delay_ms(ms);
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

    /* 清零所有 TRB，防止控制器预取垃圾数据 */
    for (uint32_t i = 0; i < cap; i++) {
        trbs[i].param_lo = 0U;
        trbs[i].param_hi = 0U;
        trbs[i].status   = 0U;
        trbs[i].control  = 0U;
    }

    /* 设置 Link TRB（最后一个）*/
    uint32_t lp = cap - 1U;
    trbs[lp].param_lo = ep32(trbs);
    trbs[lp].param_hi = 0U;
    trbs[lp].status   = 0U;
    /* 🔥 Link TRB cycle bit 设置（关键！）
     *
     * xHCI 规范（TC=1 时）：Link TRB 的 cycle bit 必须与 ring 的 PCS "相同"
     *
     * 原理：
     * - 当 PCS=1 时，Link TRB 的 cycle=1（可执行）
     * - Controller 执行完 Link TRB 后，自动 toggle PCS 并跳转到 ring 起始
     * - TC=1 告诉 controller："执行完我后切换 cycle state"
     *
     * 旧错误理解：cycle=~pcs（"环满时才更新"）
     *  - 但 ring_enq 只在 enq==cap-1 时才更新 Link TRB
     *  - 少量 TRB（如 SETUP/DATA/STATUS）根本不触发 wrap
     *  - 结果：Link TRB cycle=0 但 PCS=1，controller 跳过它 → ring 卡死
     */
    trbs[lp].control  = XHCI_TRB_TYPE_LINK | XHCI_TRB_LINK_TC | (uint32_t)init_pcs;

    /* 刷新整个环到内存 */
    clean_dcache_range(trbs, sizeof(xhci_trb_t) * cap);
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

    /* 同步初始 ERDP 到事件环起始，确保硬件和软件状态一致 */
    uint32_t evt_phys = ep32(ring);
    rt_w32(ir0, XHCI_IR_ERDP_LO, evt_phys);
    rt_w32(ir0, XHCI_IR_ERDP_HI, 0U);
    dsb();
}

/* 尝试从事件环取一个 TRB；无事件返回 -1 */
static int evt_try(evt_t *e, xhci_trb_t *out)
{
    /*
     * Invalidate 整个缓存行（64 字节），确保读取到硬件最新写入的数据。
     * 一个缓存行可能包含多个 TRB（16 字节），所以需要失效更大范围。
     * 对齐到缓存行边界，失效 64 字节。
     */
    uintptr_t addr = (uintptr_t)&e->ring[e->deq];
    uintptr_t aligned_addr = addr & ~63ULL;  /* 对齐到 64 字节边界 */
    invalidate_dcache_range((void *)aligned_addr, 64);

    /*
     * 使用 memcpy 确保原子性读取整个 TRB。
     * 这避免了编译器将读取拆分成多个单独的加载操作。
     */
    xhci_trb_t tmp;
    __builtin_memcpy(&tmp, &e->ring[e->deq], sizeof(xhci_trb_t));

    /* 检查 cycle bit 是否匹配 */
    uint32_t tmp_cycle = tmp.control & 1U;
    if (tmp_cycle != (uint32_t)e->ccs) {
        // KLOG_DEBUG("evt_try: deq=%u cycle mismatch: tmp.c=%u ccs=%u ctrl=0x%08x\n",
        //            (unsigned)e->deq, (unsigned)tmp_cycle, (unsigned)e->ccs, (unsigned)tmp.control);
        return -1;
    }

    /* 数据依赖屏障，确保后续操作看到完整的 TRB */
    dsb();

    /* 复制到输出 */
    *out = tmp;

    /* 推进 dequeue 指针 */
    uint32_t old_deq = e->deq;
    e->deq++;
    if (e->deq == e->cap) { e->deq = 0U; e->ccs ^= 1U; }

    KLOG_DEBUG("evt_try: read evt[%u] type=%u ctrl=0x%08x, deq: %u->%u ccs=%u\n",
               (unsigned)old_deq, (unsigned)((tmp.control >> 10) & 0x3F),
               (unsigned)tmp.control, (unsigned)old_deq, (unsigned)e->deq, (unsigned)e->ccs);

    /* 更新 ERDP（告知控制器消费者进度）*/
    uint32_t erdp = ep32(&e->ring[e->deq]) | XHCI_ERDP_EHB;
    KLOG_DEBUG("evt_try: updating ERDP: 0x%08x -> 0x%08x (deq=%u)\n",
               (unsigned)rt_r32(e->ir0, XHCI_IR_ERDP_LO), (unsigned)erdp, (unsigned)e->deq);
    rt_w32(e->ir0, XHCI_IR_ERDP_LO, erdp);
    rt_w32(e->ir0, XHCI_IR_ERDP_HI, 0U);
    /* 强制硬件同步 */
    dsb();

    /* 关键修复：清除 IMAN.IP 位，否则 xHC 可能停止生成新事件
     * 根据 xHCI 规范 5.5.2.1，写 1 到 IP 位会清除它 */
    uint32_t iman = rt_r32(e->ir0, XHCI_IR_IMAN);
    if (iman & 1U) {
        KLOG_DEBUG("evt_try: clearing IMAN.IP (was 0x%08x)\n", (unsigned)iman);
        rt_w32(e->ir0, XHCI_IR_IMAN, iman | 2U);  /* 保持 IE=1，写 IP=1 清除 */
        dsb();
    }
    /* 验证 ERDP 更新成功 */
    uint32_t erdp_readback = rt_r32(e->ir0, XHCI_IR_ERDP_LO);
    KLOG_DEBUG("evt_try: ERDP readback: 0x%08x\n", (unsigned)erdp_readback);
    return 0;
}

/* 轮询事件环，最多等待 max_iters 次空轮询 */
static int evt_poll(evt_t *e, xhci_trb_t *out, uint32_t max_iters)
{
    for (uint32_t i = 0; i < max_iters; i++) {
        /* 每次轮询时多次检查，避免缓存一致性问题 */
        for (int retry = 0; retry < 3; retry++) {
            if (evt_try(e, out) == 0) return 0;
            /* 短暂延迟后重试 */
            if (retry < 2) delay_us(10U);
        }
        delay_us(100U);
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
    KLOG_DEBUG("wait_cmd: start deq=%u ccs=%u\n", (unsigned)e->deq, (unsigned)e->ccs);
    for (uint32_t tries = 0; tries < 50U; tries++) {
        if (evt_poll(e, &ev, 500U) == 0) {  /* 50ms per try, 2.5s total */
            uint32_t t = (ev.control >> 10) & 0x3FU;
            if (t == 33U) {
                uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFU);
                if (slot_out)
                    *slot_out = (uint8_t)((ev.control >> 24) & 0xFFU);
                if (cc != 1U) {
                    KLOG_ERROR("dwc3_enum: cmd CC=%u\n", (unsigned)cc);
                    return -(int)(unsigned)cc;
                }
                KLOG_DEBUG("wait_cmd: success deq=%u\n", (unsigned)e->deq);
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
    /* 检查 USBSTS 中的错误标志 */
    {
        uint32_t usbsts = rt_r32(e->ir0 - XHCI_IR0_OFF + XHCI_RTSOFF - XHCI_DBOFF, XHCI_OP_USBSTS);
        KLOG_ERROR("  USBSTS=0x%08x\n", usbsts);
        if (usbsts & XHCI_USBSTS_HSE) {
            KLOG_ERROR("  Host System Error (HSE) detected!\n");
        }
        if (usbsts & XHCI_USBSTS_HCH) {
            KLOG_ERROR("  Controller Halted (HCH)!\n");
        }
    }
    return -1;
}

/* 等待传输完成事件（TRB type=32）
 * setup_pa: SETUP TRB 物理地址（非零时跳过 SETUP 完成事件，继续等 STATUS）
 * 返回剩余字节数（>=0）或 <0 表示错误 */
static int wait_xfer_ex(evt_t *e, uint32_t setup_pa)
{
    xhci_trb_t ev;
    for (uint32_t tries = 0; tries < 50U; tries++) {
        if (evt_poll(e, &ev, 500U) == 0) {  /* 50ms per try, 2.5s total */
            uint32_t t = (ev.control >> 10) & 0x3FU;
            if (t == 32U) {
                uint8_t  cc  = (uint8_t)((ev.status >> 24) & 0xFFU);
                uint32_t rem = ev.status & 0x00FFFFFFU;
                /* SETUP TRB 的 IOC 完成事件：记录后继续等 STATUS */
                if (setup_pa && ev.param_lo == setup_pa) {
                    KLOG_INFO("dwc3_enum: SETUP stage done  CC=%u  rem=%u  (waiting for STATUS)\n",
                              (unsigned)cc, (unsigned)rem);
                    if (cc != 1U) {
                        KLOG_ERROR("  SETUP CC=%u → abort\n", (unsigned)cc);
                        return -(int)(unsigned)cc;
                    }
                    continue;
                }
                if (cc == 1U || cc == 13U) /* Success or Short Packet */
                    return (int)rem;
                KLOG_ERROR("dwc3_enum: xfer CC=%u  ev: p0=0x%08x p1=0x%08x st=0x%08x ctrl=0x%08x\n",
                          (unsigned)cc,
                          (unsigned)ev.param_lo, (unsigned)ev.param_hi,
                          (unsigned)ev.status, (unsigned)ev.control);
                return -(int)(unsigned)cc;
            }
            KLOG_INFO("dwc3_enum: xfer non-xfer evt type=%u ctrl=0x%08x st=0x%08x\n",
                      (unsigned)t, (unsigned)ev.control, (unsigned)ev.status);
        }
    }
    KLOG_ERROR("dwc3_enum: xfer timeout  deq=%u\n", (unsigned)e->deq);
    KLOG_ERROR("  ERDP=0x%08x  IMAN=0x%08x\n",
              (unsigned)rt_r32(e->ir0, XHCI_IR_ERDP_LO),
              (unsigned)rt_r32(e->ir0, XHCI_IR_IMAN));
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
    uintptr_t addr = db_base + (uintptr_t)slot * 4U;
    KLOG_DEBUG("ring_db: slot=%u dci=%u addr=0x%08lx\n", (unsigned)slot, (unsigned)dci, addr);
    write32((uint32_t)dci, (void *)addr);
    dsb();
    /* 🔍 调试 3️⃣: 立即读回 doorbell 和 event ring，确认写入生效 */
    uint32_t db_readback = read32((void *)addr);
    KLOG_DEBUG("ring_db: readback=0x%08x\n", (unsigned)db_readback);
}

/* ─── 控制传输（Setup + [Data IN] + Status）───────────────────────────── */

/* IN 数据方向控制传输（GET_DESCRIPTOR 等）*/
static int ctrl_in(evt_t *e, ring_t *cr, uintptr_t db_base,
                   uint8_t slot_id,
                   uint32_t setup_p0, uint32_t setup_p1, uint32_t len,
                   void *buf, out_ctx_t *out_diag)
{
    uint32_t buf_phys = ep32(buf);

    /* Setup Stage TRB: type=2, TRT=3(IN data), IDT=1, IOC=1, TRBLen=8
     * IOC 使 SETUP 完成时产生事件，可判断 xHC 是否开始处理 TRB */
    uint32_t setup_pa = ring_enq(cr, setup_p0, setup_p1, 8U,
             (2U << 10) | (1U << 5) | (1U << 6) | (3U << 16));

    /* Data Stage TRB: type=3, DIR=IN(bit16=1) */
    ring_enq(cr, buf_phys, 0U, len,
             (3U << 10) | (1U << 16));

    /* Status Stage TRB: type=4, DIR=OUT(bit16=0, per spec for IN data), IOC=1 */
    ring_enq(cr, 0U, 0U, 0U,
             (4U << 10) | (1U << 5));

    dsb();

    /* 刷新整个控制环到 DRAM，确保 xHC 看到完整的 TRB 环（含 Link TRB）*/
    clean_dcache_range(cr->ring, sizeof(xhci_trb_t) * cr->cap);
    dsb();

    /* DMA 方向 device→host：doorbell 前先 invalidate 目标 buf，
     * 防止 CPU 脏缓存行在 DMA 完成后被回写而覆盖 DMA 数据。*/
    clean_and_invalidate_dcache_range(buf, len);
    dsb();

    ring_db(db_base, slot_id, 1U);   /* EP0 DCI=1 */

    /* 诊断：doorbell 后 100ms 轮询 EP0 TR dequeue pointer 变化
     * 若 xHC 开始取 TRB，out_ctx EP0.w[2] 会推进 */
    if (out_diag) {
        invalidate_dcache_range(&out_diag->ep[0], sizeof(ctx_t));
        uint32_t prev_deq = out_diag->ep[0].w[2];
        KLOG_INFO("ctrl_in: ep0.w[2] at doorbell = 0x%08x  setup_pa=0x%08x\n",
                  (unsigned)prev_deq, (unsigned)setup_pa);
        for (uint32_t pi = 0U; pi < 200U; pi++) {
            delay_us(500U);
            invalidate_dcache_range(&out_diag->ep[0], sizeof(ctx_t));
            uint32_t cur_deq = out_diag->ep[0].w[2];
            if (cur_deq != prev_deq) {
                KLOG_INFO("ctrl_in: ep0.w[2] moved 0x%08x→0x%08x (+%ums)\n",
                          (unsigned)prev_deq, (unsigned)cur_deq,
                          (unsigned)((pi + 1U) / 2U));
                prev_deq = cur_deq;
            }
        }
        KLOG_INFO("ctrl_in: ep0.w[2] after 100ms = 0x%08x\n",
                  (unsigned)out_diag->ep[0].w[2]);
    }

    int r = wait_xfer_ex(e, setup_pa);
    /* 传输完成后再次 invalidate，确保 CPU 读到 DMA 写入的最新数据 */
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

    clean_dcache_range(cr->ring, sizeof(xhci_trb_t) * cr->cap);
    dsb();
    ring_db(db_base, slot_id, 1U);

    return wait_xfer_ex(e, 0U);
}

/* ─── USB Setup 包宏（xHCI TRB param_lo/param_hi 编码）
 *
 * param_lo = {wValue_hi, wValue_lo, bRequest, bmRequestType} (bytes 3,2,1,0)
 * param_hi = {wLength_hi, wLength_lo, wIndex_hi, wIndex_lo}  (bytes 7,6,5,4)
 * ─────────────────────────────────────────────────────────────────────────*/

/* GET_DESCRIPTOR(Device, len) */
#define SETUP_GET_DEV_DESC(len) \
    0x01000680U, (((uint32_t)(len) & 0xFFU) << 16)

/* SET_ADDRESS(addr) - 在 BSR=1 模式下需要手动设置设备地址 */
#define SETUP_SET_ADDRESS(addr) \
    ((uint32_t)((addr) & 0xFFU) << 16) | 0x0500U, 0U

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
static int      g_is_camera    = 0;    /* 1 = UVC 摄像头设备 */

/* 遍历完整配置描述符，找到 HID boot 中断 IN 端点或 UVC 接口 */
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
            /* bInterfaceClass=0x0E: USB Video Class (UVC) */
            if (buf[pos + 5U] == 0x0EU) {
                g_is_camera = 1;
                KLOG_INFO("dwc3_enum: UVC Video interface found (if=%u subclass=0x%02x)\n",
                          (unsigned)cur_if, (unsigned)buf[pos + 6U]);
            }
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

/* ──────────────────────────────────────────────────────────────────────────────
 * 主入口：HID 设备枚举
 * ──────────────────────────────────────────────────────────────────────────────
 *
 * 输出示例：
 *   dwc3_enum: === USB HID enum start ===
 *   dwc3_enum: [OTG1] port 1
 *   dwc3_enum:   speed=1 starting enumeration
 *   dwc3_enum: CRCR readback=0x00000000 (DWC3 quirk: always 0 before 1st TRB)
 *   dwc3_enum: drain done  n=1  deq=1
 *   dwc3_enum: port reset...
 *   dwc3_enum: PR=0 detected  PORTSC=0x00220e03  PED=1  PLS=0  Speed=3
 *   dwc3_enum: port reset complete  speed=HighSpeed  PORTSC=0x00000e03
 *   dwc3_enum: slot_id=1
 *   dwc3_enum: Address Device BSR=1 (testing...)...
 *   dwc3_enum: No Op command succeeded
 *   dwc3_enum: BSR=1 wait_cmd rc=0  USBSTS=0x00000018
 *   dwc3_enum: Trying GET_DESCRIPTOR first (device at addr 0)...
 *   dwc3_enum: bcdUSB=0200
 *   dwc3_enum:   bDevClass=0x00 bMaxPkt0=64
 *   dwc3_enum: HID EP addr=0x81 maxpkt=4
 *   dwc3_enum:   interval=10 dci=3
 *   dwc3_enum: SET_CONFIGURATION(1) OK
 *   dwc3_enum: SET_PROTOCOL(boot) OK  iface=0
 *   dwc3_enum: EP1 IN configured
 *   dwc3_enum:   maxpkt=4 interval=4
 *   dwc3_enum: === Start reading HID reports (50 times) ===
 *   dwc3_enum: HID[00] len=4
 *   dwc3_enum:   btn=0x00 dx=0
 *   dwc3_enum:   dy=0 wh=0
 *   dwc3_enum: === HID enumeration complete ===
 * ──────────────────────────────────────────────────────────────────────────────*/

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

    /* 诊断：打印 DWC3 关键寄存器，验证 Host mode 和 GFLADJ 是否正确 */
    {
        uint32_t gsts  = read32((void *)(base + DWC3_GSTS));
        uint32_t gfladj = read32((void *)(base + DWC3_GFLADJ));
        uint32_t curmod = gsts & 0x3U;
        KLOG_INFO("dwc3_enum: DWC3 diag: GSTS=0x%08x CURMOD=%u(%s) GFLADJ=0x%08x\n",
                  (unsigned)gsts, (unsigned)curmod,
                  curmod == 0U ? "Device" : curmod == 1U ? "Host" :
                  curmod == 2U ? "DRD" : "?",
                  (unsigned)gfladj);
        if (curmod != 1U)
            KLOG_ERROR("dwc3_enum: CURMOD != Host!  EP0 传输将沉默\n");
    }

    /* ── Initialize tracking state for each ring ── */
    ring_t cmd_r;
    evt_t  evt;

    /* 命令环追踪状态：xhci_start_one 始终以 PCS=1 初始化命令环（写 CRCR.RCS=1）。
     * DWC3 v3.00a 特性：CRCR 指针分 readback 在消费第一个 TRB 前始终返回 0，
     * 不能用读回值推算 PCS，直接使用确定的 PCS=1。 */
    uint32_t crcr_lo_diag = op_r32(op_b, XHCI_OP_CRCR_LO);
    KLOG_INFO("dwc3_enum: CRCR readback=0x%08x (DWC3 quirk: always 0 before 1st TRB)  using PCS=1\n",
              (unsigned)crcr_lo_diag);
    ring_attach(&cmd_r, g_xhci_hw[ctl].cmd_ring, XHCI_CMD_RING_TRBS, 1U);
    /* 事件环：消费者从 deq=0 CCS=1 开始 */
    evt_init(&evt, g_xhci_hw[ctl].evt_ring, XHCI_EVT_RING_TRBS, ir0);

    /* 清空因端口连接产生的 PSC 事件 */
    evt_drain(&evt);

    /* drain 后状态已同步：
     * - deq 指向下一个要读取的 TRB
     * - ccs 与硬件写入的 cycle bit 匹配
     * - ERDP 已被 evt_try() 更新到正确位置
     *
     * 不要调用 evt_init() 重新初始化！这会将 deq 重置为 0，
     * 导致软件 deq 与硬件 ERDP 不一致，进而导致控制器停止写入事件。 */

    /* 打印事件环状态 */
    KLOG_INFO("dwc3_enum: After drain: evt.deq=%u evt.ccs=%u\n",
              (unsigned)evt.deq, (unsigned)evt.ccs);
    /* 打印事件环前几个 TRB 的状态 */
    for (int i = 0; i < 4; i++) {
        invalidate_dcache_range(&evt.ring[i], sizeof(xhci_trb_t));
        KLOG_INFO("dwc3_enum:   evt_ring[%u] ctrl=0x%08x\n",
                  i, (unsigned)evt.ring[i].control);
    }

    /* EP0 传输环（全新）*/
    ring_t ctrl_r;
    ring_init(&ctrl_r, g_enum.ctrl_ring, CTRL_RING_TRBS, 1U);

    /* ── DWC3 quirk: 枚举期间禁止 SUSPHY，防止 PHY 进入 Suspend 后 SET_ADDRESS 挂死 ──
     * 同时清除 U2_FREECLK_EXISTS (bit30)：RK3588 USB2 PHY 没有自由运行时钟 (Linux quirk) */
    uint32_t phycfg_saved = read32((void *)(base + DWC3_GUSB2PHYCFG0));
    {
        uint32_t phycfg = phycfg_saved & ~(DWC3_GUSB2PHYCFG_SUSPHY |
                                           DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS);
        write32(phycfg, (void *)(base + DWC3_GUSB2PHYCFG0));
        KLOG_INFO("dwc3_enum: SUSPHY+FREECLK cleared  GUSB2PHYCFG0=0x%08x→0x%08x\n",
                  (unsigned)phycfg_saved, (unsigned)phycfg);
    }

    /* ── 步骤 1: 端口复位 ── */
    KLOG_INFO("dwc3_enum: port reset...\n");

    /* 在端口复位前，确保控制器正在运行 */
    {
        uint32_t usbsts = op_r32(op_b, XHCI_OP_USBSTS);
        KLOG_INFO("dwc3_enum: USBSTS before port reset: 0x%08x\n", (unsigned)usbsts);
        if (usbsts & XHCI_USBSTS_HCH) {
            KLOG_ERROR("dwc3_enum: Controller halted before port reset, cannot continue\n");
            return;
        }
        if (usbsts & XHCI_USBSTS_HSE) {
            KLOG_WARN("dwc3_enum: Clearing HSE before port reset\n");
            op_w32(op_b, XHCI_OP_USBSTS, XHCI_USBSTS_HSE);
            delay_us(100U);
        }
        delay_ms(1U);
    }

    /* 清除 CSC 并置位 PR；保留 PP */
    uint32_t sc = op_r32(op_b, XHCI_OP_PORTSC(port));
    KLOG_INFO("dwc3_enum: PORTSC before reset: 0x%08x\n", sc);
    sc  = (sc & 0x0E01FEC0U);   /* 保留 RW 字段，清零 RW1C */
    sc |= XHCI_PORTSC_PR;
    op_w32(op_b, XHCI_OP_PORTSC(port), sc);
    dsb();

    /* 等待 PR 清零且 PED=1（端口已使能）*/
    uint32_t timeout = 500U;  /* 500ms max */
    do {
        delay_ms(1U);
        sc = op_r32(op_b, XHCI_OP_PORTSC(port));
        timeout--;
    } while ((sc & XHCI_PORTSC_PR) && timeout);

    if (sc & XHCI_PORTSC_PR) {
        KLOG_ERROR("dwc3_enum: port reset timeout  PORTSC=0x%08x\n", sc);
        return;
    }
    KLOG_INFO("dwc3_enum: PR=0 detected  PORTSC=0x%08x  PED=%u  PLS=%u  Speed=%u\n",
              sc, (unsigned)((sc >> 1) & 1U),
              (unsigned)((sc >> 5) & 0xFU),
              (unsigned)((sc >> 10) & 0xFU));

    /* 清除 PRC（Port Reset Change，bit21）和 CSC（bit17）
     * 只写 RW1C 位（PRC/CSC），不写其他位（避免 DWC3 quirk）。
     * DWC3 标准 xHCI：PED(bit1) 是 RW1C（写 1=disable），写 0=无效果。*/
    {
        uint32_t sc_prc = op_r32(op_b, XHCI_OP_PORTSC(port));
        KLOG_INFO("dwc3_enum: sc_prc before PRC/CSC clear: 0x%08x  PED=%u  PLS=%u\n",
                  sc_prc, (unsigned)((sc_prc >> 1) & 1U),
                  (unsigned)((sc_prc >> 5) & 0xFU));
        /* 只写 PRC 和 CSC；PP(9) 和 PLS(8:5) 保留（避免意外 PLS 变更）*/
        op_w32(op_b, XHCI_OP_PORTSC(port),
               (sc_prc & 0x000003E0U)   /* 保留 PLS(8:5) + PP(9)，不含 PED */
               | (1U << 21)              /* 清除 PRC */
               | (1U << 17));            /* 清除 CSC */
    }
    /* 等待 PED=1（端口使能，表示 HS chirp 完成）
     * 注意：CCS 在整个过程都可能为 1，不能用来判断就绪 */
    for (uint32_t ws = 0; ws < 10000000U; ws++) {
        sc = op_r32(op_b, XHCI_OP_PORTSC(port));
        if ((sc >> 1) & 1U) break;   /* PED=1 */
        delay_us(100U);
    }
    KLOG_INFO("dwc3_enum: after PED wait  PORTSC=0x%08x  PED=%u  CCS=%u  PLS=%u\n",
              sc, (unsigned)((sc >> 1) & 1U),
              (unsigned)(sc & 1U),
              (unsigned)((sc >> 5) & 0xFU));
    if (!((sc >> 1) & 1U)) {
        KLOG_WARN("dwc3_enum: PED still 0 after wait  PORTSC=0x%08x\n", sc);
    }

    /* 检查控制器状态，确保没有错误 */
    {
        uint32_t usbsts = op_r32(op_b, XHCI_OP_USBSTS);
        KLOG_INFO("dwc3_enum: USBSTS after port reset: 0x%08x\n", (unsigned)usbsts);
        /* 解析 USBSTS 标志 */
        KLOG_INFO("  HCHalted=%u HSE=%u EINT=%u PCD=%u\n",
                  (unsigned)(usbsts & 1U),
                  (unsigned)((usbsts >> 2) & 1U),
                  (unsigned)((usbsts >> 3) & 1U),
                  (unsigned)((usbsts >> 4) & 1U));
        if (usbsts & XHCI_USBSTS_HSE) {
            KLOG_ERROR("dwc3_enum: Host System Error (HSE) after port reset!\n");
            op_w32(op_b, XHCI_OP_USBSTS, XHCI_USBSTS_HSE);
            delay_us(100U);
            usbsts = op_r32(op_b, XHCI_OP_USBSTS);
            KLOG_INFO("  USBSTS after HSE clear: 0x%08x\n", (unsigned)usbsts);
        }
        if (usbsts & XHCI_USBSTS_HCH) {
            KLOG_ERROR("dwc3_enum: Controller Halted after port reset!\n");
            sc = op_r32(op_b, XHCI_OP_PORTSC(port));
            KLOG_ERROR("  PORTSC=0x%08x\n", sc);
            KLOG_ERROR("dwc3_enum: Cannot continue with halted controller\n");
            return;
        }
    }

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
    delay_ms(10U);
    evt_drain(&evt);

    /* 确保控制器准备好处理新命令 - 等待 USBSTS 稳定 */
    {
        uint32_t usbsts;
        for (int i = 0; i < 100; i++) {
            usbsts = op_r32(op_b, XHCI_OP_USBSTS);
            if (!(usbsts & XHCI_USBSTS_CNR))
                break;
            delay_ms(1U);
        }
        KLOG_INFO("dwc3_enum: USBSTS before Enable Slot: 0x%08x\n", (unsigned)usbsts);
        /* 检查控制器是否已停止或出错 */
        if (usbsts & XHCI_USBSTS_HCH) {
            KLOG_ERROR("dwc3_enum: Controller Halted before Enable Slot!\n");
            return;
        }
        if (usbsts & XHCI_USBSTS_HSE) {
            KLOG_ERROR("dwc3_enum: Host System Error before Enable Slot!\n");
            return;
        }
    }

    /* ── 步骤 2: 初始化 DMA 结构 ── */

    memset(&g_enum, 0, sizeof(enum_res_t));
    ring_init(&ctrl_r, g_enum.ctrl_ring, CTRL_RING_TRBS, 1U);

    in_ctx_t  *in  = &g_enum.in_ctx;
    out_ctx_t *out = &g_enum.out_ctx;

    /* Input Control Context: A0=1(Slot), A1=1(EP0) */
    in->ctrl.w[0] = 0U;
    in->ctrl.w[1] = 0x3U;

    /* Slot Context */
    in->slot.w[0] = (1U << 27) | ((uint32_t)port_speed << 20);
    in->slot.w[1] = ((uint32_t)(port + 1U) << 16);
    in->slot.w[2] = 0U;
    in->slot.w[3] = 0U;

    /* EP0 Context */
    uint16_t ep0_maxpkt;
    switch (port_speed) {
        case 3U: ep0_maxpkt = 64U; break;
        case 1U: case 2U: default: ep0_maxpkt = 8U; break;
    }
    in->ep[0].w[0] = 0U;
    /* DW1: MaxPkt | EP Type=4(Control Bidir) | CErr=3 (bits[2:1] of DW1) */
    in->ep[0].w[1] = ((uint32_t)ep0_maxpkt << 16) | (4U << 3) | (3U << 1);
    in->ep[0].w[2] = ep32(g_enum.ctrl_ring) | 1U;
    in->ep[0].w[3] = 0U;
    in->ep[0].w[4] = 8U;

    /* 刷新 Input/Output Context 和 ctrl_ring 到 DRAM */
    clean_dcache_range(in,  sizeof(in_ctx_t));
    clean_dcache_range(out, sizeof(out_ctx_t));
    clean_dcache_range(g_enum.ctrl_ring, sizeof(xhci_trb_t) * CTRL_RING_TRBS);
    dsb();

    KLOG_INFO("dwc3_enum: in=0x%08x out=0x%08x ep0_maxpkt=%u\n",
              ep32(in), ep32(out), ep0_maxpkt);
    KLOG_INFO("  in_ctx: slot.w[0]=0x%08x slot.w[1]=0x%08x\n",
              (unsigned)in->slot.w[0], (unsigned)in->slot.w[1]);
    KLOG_INFO("  in_ctx: ep0.w[1]=0x%08x ep0.w[2]=0x%08x\n",
              (unsigned)in->ep[0].w[1], (unsigned)in->ep[0].w[2]);

    /* ── 步骤 3: Enable Slot（独立门铃）── */
    ring_enq(&cmd_r, 0U, 0U, 0U, 9U << 10);
    clean_dcache_range(g_xhci_hw[ctl].cmd_ring, sizeof(xhci_trb_t) * XHCI_CMD_RING_TRBS);
    dsb();

    KLOG_INFO("dwc3_enum: Enable Slot: cmd[0]=0x%08x  enq=%u\n",
              g_xhci_hw[ctl].cmd_ring[0].control, (unsigned)cmd_r.enq);

    write32(0U, (void *)db_b);
    dsb();

    uint8_t slot_id;
    {
        uint8_t actual_slot = 0;
        if (wait_cmd(&evt, &actual_slot) != 0) {
            KLOG_ERROR("dwc3_enum: Enable Slot command failed\n");
            return;
        }
        slot_id = actual_slot;
        KLOG_INFO("dwc3_enum: slot_id=%u\n", (unsigned)slot_id);
    }

    /* DCBAA[slot_id] → Output Context，在 Address Device 前写入 */
    g_xhci_hw[ctl].dcbaa[slot_id] = (uint64_t)ep32(out);
    clean_dcache_range(g_xhci_hw[ctl].dcbaa, sizeof(g_xhci_hw[ctl].dcbaa));
    dsb();
    KLOG_INFO("dwc3_enum: DCBAA[%u]=0x%08x (out_ctx phys)\n",
              (unsigned)slot_id, ep32(out));

    /* ── 步骤 4: Address Device BSR=0（SET_ADDRESS，分配 USB 地址）──
     * Linux DWC3 对普通设备只用 BSR=0。BSR=1 仅在 Hub 设备迁移时使用。
     * BSR=0 让 xHC 在 USB 总线上发 SET_ADDRESS，激活 EP0 的 USB 调度器。
     * 只有完成 BSR=0 后，doorbell DB[slot] DCI=1 才会触发实际 USB 事务。 */
    ring_enq(&cmd_r, ep32(in), 0U, 0U,
             (11U << 10) | (0U << 9) | ((uint32_t)slot_id << 24));   /* BSR=0 */
    clean_dcache_range(g_xhci_hw[ctl].cmd_ring, sizeof(xhci_trb_t) * XHCI_CMD_RING_TRBS);
    dsb();
    /* 诊断：打印即将提交的 BSR=0 TRB */
    {
        xhci_trb_t *t = &g_xhci_hw[ctl].cmd_ring[1];
        KLOG_INFO("dwc3_enum: BSR=0 TRB: p0=0x%08x ctrl=0x%08x  in_ctx_phys=0x%08x\n",
                  (unsigned)t->param_lo, (unsigned)t->control, ep32(in));
        KLOG_INFO("  PORTSC=0x%08x  USBSTS=0x%08x\n",
                  (unsigned)op_r32(op_b, XHCI_OP_PORTSC(port)),
                  (unsigned)op_r32(op_b, XHCI_OP_USBSTS));
    }
    write32(0U, (void *)db_b);
    dsb();
    /* CRCR.CRR(bit3)=1 表示 xHC 已接收命令并正在执行 */
    {
        uint32_t crcr = op_r32(op_b, XHCI_OP_CRCR_LO);
        KLOG_INFO("dwc3_enum: after BSR=0 doorbell  CRCR=0x%08x CRR=%u\n",
                  (unsigned)crcr, (unsigned)((crcr >> 3) & 1U));
    }

    {
        int rc = wait_cmd(&evt, NULL);
        KLOG_INFO("dwc3_enum: Address Device BSR=0 rc=%d  USBSTS=0x%08x  PORTSC=0x%08x\n",
                  rc, (unsigned)op_r32(op_b, XHCI_OP_USBSTS),
                  (unsigned)op_r32(op_b, XHCI_OP_PORTSC(port)));
        if (rc != 0) {
            KLOG_ERROR("dwc3_enum: BSR=0 failed (rc=%d)\n", rc);
            /* 失败时再打一次 CRCR 看 CRR 是否还在 */
            KLOG_ERROR("  CRCR=0x%08x\n", (unsigned)op_r32(op_b, XHCI_OP_CRCR_LO));
            return;
        }
        invalidate_dcache_range(out, sizeof(out_ctx_t));
        KLOG_INFO("dwc3_enum: BSR=0 OK: slot_state=%u  usb_addr=%u  ep0_state=%u\n",
                  (unsigned)((out->slot.w[3] >> 27) & 0x1FU),
                  (unsigned)(out->slot.w[3] & 0xFFU),
                  (unsigned)(out->ep[0].w[0] & 0x7U));
    }

    /* 清除 USBSTS 中的 EINT/PCD 残留标志（RW1C），避免影响后续传输事件检测 */
    {
        uint32_t sts = op_r32(op_b, XHCI_OP_USBSTS);
        if (sts & 0x18U)
            op_w32(op_b, XHCI_OP_USBSTS, sts & 0x18U);
    }

    KLOG_INFO("dwc3_enum: GET_DESCRIPTOR...\n");

    /* ── 步骤 5: GET_DESCRIPTOR(Device, 8) → 获取 bMaxPacketSize0 ── */
    memset(g_enum.desc_buf, 0, sizeof(g_enum.desc_buf));
    int n = ctrl_in(&evt, &ctrl_r, db_b, slot_id,
                    SETUP_GET_DEV_DESC(8), 8U, g_enum.desc_buf, out);
    if (n < 8) {
        KLOG_ERROR("dwc3_enum: GET_DESCRIPTOR(Dev,8) failed n=%d\n", n);
        /* check if HC advanced TR Dequeue Pointer (=TRBs consumed) */
        invalidate_dcache_range(out, sizeof(out_ctx_t));
        KLOG_ERROR("  ep0.w[0]=0x%08x ep_state=%u  ep0.w[2]=0x%08x (expected 0x%08x if advanced)\n",
                   (unsigned)out->ep[0].w[0],
                   (unsigned)(out->ep[0].w[0] & 0x7U),
                   (unsigned)out->ep[0].w[2],
                   (ep32(g_enum.ctrl_ring) + 3U * (uint32_t)sizeof(xhci_trb_t)) | 1U);
        KLOG_ERROR("  PORTSC=0x%08x  USBSTS=0x%08x  IMAN=0x%08x\n",
                   (unsigned)op_r32(op_b, XHCI_OP_PORTSC(port)),
                   (unsigned)op_r32(op_b, XHCI_OP_USBSTS),
                   (unsigned)rt_r32(evt.ir0, XHCI_IR_IMAN));
        /* 打印 ctrl_ring TRBs (invalidate 以确保读的是 DRAM) */
        invalidate_dcache_range(g_enum.ctrl_ring, sizeof(xhci_trb_t) * CTRL_RING_TRBS);
        for (uint32_t ti = 0; ti < 4U; ti++)
            KLOG_ERROR("  ctrl_ring[%u] p0=0x%08x p1=0x%08x st=0x%08x ctrl=0x%08x\n",
                       (unsigned)ti,
                       (unsigned)g_enum.ctrl_ring[ti].param_lo,
                       (unsigned)g_enum.ctrl_ring[ti].param_hi,
                       (unsigned)g_enum.ctrl_ring[ti].status,
                       (unsigned)g_enum.ctrl_ring[ti].control);
        /* 打印 Link TRB（最后一个 TRB）*/
        KLOG_ERROR("  ctrl_ring[%u] Link TRB: p0=0x%08x ctrl=0x%08x\n",
                  (unsigned)(CTRL_RING_TRBS - 1U),
                  (unsigned)g_enum.ctrl_ring[CTRL_RING_TRBS - 1U].param_lo,
                  (unsigned)g_enum.ctrl_ring[CTRL_RING_TRBS - 1U].control);
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
        in->ep[0].w[1] = ((uint32_t)ep0_maxpkt << 16) | (4U << 3) | (3U << 1);
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
    uint16_t dev_vid = 0, dev_pid = 0;
    memset(g_enum.desc_buf, 0, sizeof(g_enum.desc_buf));
    n = ctrl_in(&evt, &ctrl_r, db_b, slot_id,
                SETUP_GET_DEV_DESC(18), 18U, g_enum.desc_buf, NULL);
    if (n >= 12) {
        dev_vid = (uint16_t)g_enum.desc_buf[8]  | ((uint16_t)g_enum.desc_buf[9]  << 8);
        dev_pid = (uint16_t)g_enum.desc_buf[10] | ((uint16_t)g_enum.desc_buf[11] << 8);
        KLOG_INFO("dwc3_enum: VID=0x%04x PID=0x%04x\n",
                  (unsigned)dev_vid, (unsigned)dev_pid);
    }

    /* ── 步骤 6: GET_DESCRIPTOR(Configuration, 9) → wTotalLength ── */
    memset(g_enum.desc_buf, 0, 9);
    n = ctrl_in(&evt, &ctrl_r, db_b, slot_id,
                SETUP_GET_CFG_DESC(9), 9U, g_enum.desc_buf, NULL);
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
                SETUP_GET_CFG_DESC(total_len), (uint32_t)total_len, g_enum.desc_buf, NULL);
    if (n < 9) {
        KLOG_ERROR("dwc3_enum: GET_DESCRIPTOR(Cfg,full) failed n=%d\n", n);
        return;
    }
    g_is_camera = 0;
    parse_config(g_enum.desc_buf, (uint32_t)n);

    if (g_is_camera) {
        KLOG_INFO("dwc3_enum: *** UVC Camera detected! VID=0x%04x PID=0x%04x ***\n",
                  (unsigned)dev_vid, (unsigned)dev_pid);
        KLOG_INFO("dwc3_enum: === Camera enumeration complete ===\n");
        write32(phycfg_saved, (void *)(base + DWC3_GUSB2PHYCFG0));
        return;
    }

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
        int r = wait_xfer_ex(&evt, 0U);
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

        delay_ms(2U);
    }

    KLOG_INFO("dwc3_enum: === HID enumeration complete ===\n");

    write32(phycfg_saved, (void *)(base + DWC3_GUSB2PHYCFG0));
}
