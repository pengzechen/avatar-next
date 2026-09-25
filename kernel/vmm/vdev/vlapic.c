/*
 * kernel/vmm/vdev/vlapic.c — x86_64 虚拟 LAPIC（xAPIC / MMIO 模式）
 *
 * 移植自 tgoskits `virtualization/x86_vlapic`（Rust, ~4400 行），只取
 * 「Linux 能启动」所必需的最小子集。上游那些完整实现（vIOAPIC/vPIC/PIT/
 * x2APIC/MSI）留着，等需要外部设备中断时再按同一结构补。
 *
 * 传输方式：MMIO @0xFEE00000。
 *   EPT 把 guest RAM 之外的所有 GPA 都置成无效（x86_ept_enable_mmio_trap），
 *   而 0xFEE00000 正在其中 —— 于是 guest 的每次 LAPIC 访问天然变成
 *   EPT violation，走到这里，**不需要** VMX 的 APIC-access page 机制。
 *
 * 定时器：宿主 tick 驱动（轮询，不注册宿主定时器）。
 *   每次进 guest 前调 vlapic_timer_poll()，看虚拟 deadline 是否已过。
 *   因为开了 PIN_EXTINT，宿主 100Hz 的时钟中断本身就会让 guest 每 10ms
 *   退出来一次 —— 这个频率正好充当轮询节拍，不需要额外的定时器基础设施。
 *
 * 有意没做的（第一版不需要）：
 *   - **x2APIC**：APIC base MSR 的 EXTD 位一律清掉，Linux 探测到之后会
 *     安心走 MMIO 路径（tgoskits 的官方 Linux 配置也是 `nox2apic`）。
 *   - **INIT/SIPI**：空实现 → **必须保持单 vCPU**，SMP 启动流程没做。
 *   - **IRR 排队语义**：上游这个 crate 自己也没写到 IRR（accept_interrupt
 *     直投 ISR），这里保持一致，IRR 只读 0。
 *   - **LVT LINT0/LINT1/THERMAL/PMI/CMCI**：只存值，不产生行为。上游同样，
 *     PIC 的中断不经过 LINT0，而是 hypervisor 主动注入。
 *
 * 移植时改掉的上游坑（照抄会打挂宿主）：
 *   - 上游对未列出的寄存器偏移**直接 panic**；这里一律「读回 0、写忽略」。
 *   - 上游 ICR 用 lowest-priority 模式会 `unimplemented!()`；这里降级为忽略。
 */

#include "vmm/vmm_vlapic.h"
#include "vmm/vmm.h"
#include "klog.h"
#include "string.h"
#include "irq/lapic.h"   /* g_tsc_freq_hz（宿主已标定的 TSC 频率）*/

/* ── 虚拟 APIC timer 的「频率」───────────────────────────────
 *
 * 对齐 tgoskits 的 APIC_TIMER_TICKS_PER_NANO = 1，即 1 个 APIC tick 当
 * 1 纳秒 → 等效 1 GHz（再按 DCR 分频）。
 *
 * 这个值是**约定**不是测量：Linux 启动时会自己校准 LAPIC timer
 * （它数一个 jiffy 内 CurrentCount 掉了多少），所以具体频率不重要，
 * 重要的是**稳定**。别随手改，改了 Linux 那边感觉到的周期会跟着变。
 */
#define VLAPIC_TIMER_HZ   1000000000ULL
#define VLAPIC_PERIODIC_MIN_NS  200000ULL   /* 周期模式最小 200us（照抄 KVM）*/

/* LVT 位 */
#define LVT_MASKED      (1u << 16)
#define LVT_MODE_MASK   (3u << 17)
#define LVT_MODE_ONESHOT   (0u << 17)
#define LVT_MODE_PERIODIC  (1u << 17)
#define LVT_MODE_TSCDEADLINE (2u << 17)
#define LVT_VECTOR(v)   ((v) & 0xff)

/* SVR 位 */
#define SVR_ENABLE      (1u << 8)

/* ICR 位 */
#define ICR_DELIVERY_MASK  (7u << 8)
#define ICR_MODE_FIXED     (0u << 8)
#define ICR_MODE_INIT      (5u << 8)
#define ICR_MODE_SIPI      (6u << 8)
#define ICR_DEST_MASK      (7u << 18)
#define ICR_DEST_SELF      (1u << 18)
#define ICR_DEST_ALL       (2u << 18)
#define ICR_DEST_ALL_BUT_SELF (3u << 18)
#define ICR_LEVEL_ASSERT   (1u << 14)
#define ICR_TRIGGER_LEVEL  (1u << 15)

typedef struct {
    uint32_t r[VLAPIC_REG_COUNT];   /* 按 (addr >> 4) 索引，与 MMIO 布局一致 */
    uint32_t isr[8];                /* 256 位 ISR */
    uint32_t tmr[8];                /* 256 位 TMR（触发方式，只读回）*/

    /* 定时器 */
    uint64_t t_deadline_ns;
    uint64_t t_interval_ns;
    uint32_t t_shift;               /* DCR → 分频指数 */
    int      t_active;
    int      t_periodic;

    /* 待注入 guest 的中断（VMM 在 VM-entry 前取走）*/
    uint32_t pending_vec;

    /* IA32_APIC_BASE（来自 MSR 影子，不是 MMIO 寄存器）*/
    uint64_t apic_base;
} vlapic_t;

static vlapic_t g_vlapic[MAX_VCPUS];
static int      s_enabled;

/* ── 时间源 ─────────────────────────────────────────────────
 *
 * 用宿主已标定的 TSC（g_tsc_freq_hz，由 lapic_timer_init 经 PIT 测出）
 * 换算成纳秒。毫秒级的 timer_get_uptime_ms() 太粗 —— Linux 的 LAPIC
 * timer 周期通常只有 1~10ms，用毫秒粒度会让它的校准量出错误的值。
 *
 * 除数先化成 MHz 再乘，避免 tsc(2.4e12) * 1e9 溢出。
 */
uint64_t vlapic_now_ns(void)
{
    uint32_t lo, hi;
    uint64_t tsc, mhz;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    tsc = lo | ((uint64_t)hi << 32);

    if (g_tsc_freq_hz == 0)
        return 0;                        /* 还没标定：调用方按 0 处理 */

    mhz = g_tsc_freq_hz / 1000000ULL;
    if (mhz == 0)
        return 0;
    return (tsc / mhz) * 1000ULL;
}

/* ── ISR / 优先级 ─────────────────────────────────────────── */


/* 找到 ISR 中编号最大的已置位向量（EOI 从这里开始退）*/
static int isr_highest(const vlapic_t *v)
{
    for (int i = 7; i >= 0; i--) {
        if (v->isr[i])
            return i * 32 + (31 - __builtin_clz(v->isr[i]));
    }
    return -1;
}

static uint8_t priority_class(uint32_t vec)
{
    return (uint8_t)(vec >> 4);
}

/* PPR = max(TPR, 当前正在服务的最高优先级) */
static void update_ppr(vlapic_t *v)
{
    uint8_t tpr = (uint8_t)(v->r[VLAPIC_REG_TPR] & 0xff);
    int     top = isr_highest(v);
    uint8_t isrv = (top >= 0) ? priority_class((uint32_t)top) : 0;

    v->r[VLAPIC_REG_PPR] = (tpr > isrv) ? tpr : isrv;
}

void vlapic_accept_interrupt(uint32_t vector, int level_triggered)
{
    vlapic_t *v = &g_vlapic[0];

    if (vector == 0 || vector > 255)
        return;
    v->isr[vector / 32] |= (1u << (vector % 32));
    if (level_triggered)
        v->tmr[vector / 32] |= (1u << (vector % 32));
    update_ppr(v);
}

static void process_eoi(vlapic_t *v)
{
    int top = isr_highest(v);

    if (top < 0)
        return;
    v->isr[(uint32_t)top / 32] &= ~(1u << ((uint32_t)top % 32));
    v->tmr[(uint32_t)top / 32] &= ~(1u << ((uint32_t)top % 32));
    update_ppr(v);
}

/* ── 定时器 ───────────────────────────────────────────────── */

/* DCR 编码 → 分频指数（Intel SDM 表 11-19）*/
static uint32_t dcr_to_shift(uint32_t dcr)
{
    switch (dcr & 0xb) {
    case 0x0: return 1;   /* ÷2  */
    case 0x1: return 0;   /* ÷1  */
    case 0x2: return 2;   /* ÷4  */
    case 0x3: return 3;   /* ÷8  */
    case 0x8: return 4;   /* ÷16 */
    case 0x9: return 5;   /* ÷32 */
    case 0xa: return 6;   /* ÷64 */
    default:  return 7;   /* ÷128 */
    }
}

static void timer_start(vlapic_t *v)
{
    uint32_t init = v->r[VLAPIC_REG_TIMER_INIT];
    uint64_t ticks;

    v->t_active   = 0;
    v->t_periodic = ((v->r[VLAPIC_REG_LVT_TIMER] & LVT_MODE_MASK)
                     == LVT_MODE_PERIODIC);

    if (init == 0)
        return;                             /* 写 0 = 停 */
    if (v->r[VLAPIC_REG_LVT_TIMER] & LVT_MASKED)
        return;                             /* LVT 屏蔽着：不启动 */

    /* interval_ns = (init << shift) / VLAPIC_TIMER_HZ * 1e9
     * 由于 VLAPIC_TIMER_HZ 就是 1e9，tick 数直接等于纳秒数。*/
    ticks = (uint64_t)init << v->t_shift;

    if (v->t_periodic && ticks < VLAPIC_PERIODIC_MIN_NS)
        ticks = VLAPIC_PERIODIC_MIN_NS;     /* 周期模式的下限，照抄 KVM */

    v->t_interval_ns = ticks;
    v->t_deadline_ns = vlapic_now_ns() + ticks;
    v->t_active      = 1;
}

static void timer_stop(vlapic_t *v)
{
    v->t_active = 0;
}

/* 定时器到点 → 排一个待注入向量 */
static void timer_fire(vlapic_t *v)
{
    uint32_t vec = LVT_VECTOR(v->r[VLAPIC_REG_LVT_TIMER]);

    if (vec >= 16)                          /* 0-15 是异常，不能当普通中断投 */
        v->pending_vec = vec;

    if (v->t_periodic) {
        /* 追赶：落后多个周期时不要补发一堆，直接跳到未来的第一个点 */
        uint64_t now = vlapic_now_ns();
        do {
            v->t_deadline_ns += v->t_interval_ns;
        } while (v->t_deadline_ns <= now);
    } else {
        v->t_active = 0;
    }
}

/*
 * vlapic_timer_poll — 每次进 guest 前调用
 *
 * 返回 1 表示这次轮询产生了待注入的定时器中断。
 */
int vlapic_timer_poll(void)
{
    vlapic_t *v = &g_vlapic[0];
    int fired = 0;

    if (!s_enabled || !(v->r[VLAPIC_REG_SVR] & SVR_ENABLE))
        return 0;                           /* APIC 被软件关掉：什么都不投 */
    if (!v->t_active)
        return 0;

    while (v->t_active && vlapic_now_ns() >= v->t_deadline_ns) {
        timer_fire(v);
        fired = 1;
    }
    return fired;
}

/* ── 初始化 ───────────────────────────────────────────────── */

void vlapic_init(uint32_t vcpu_id)
{
    vlapic_t *v = &g_vlapic[vcpu_id & (MAX_VCPUS - 1)];

    memset(v, 0, sizeof(*v));

    /* ID：Linux 用它做拓扑/自检。tgoskits 放的是 vcpu_id << 24。
     * 注意 ID 寄存器**只读** —— 写它必须被忽略，否则 Linux 会认为
     * 「APIC ID 可写」而走进不该走的分支。*/
    v->r[VLAPIC_REG_ID] = (vcpu_id & 0xff) << 24;

    /* VERSION：0x14 = 版本 20，bits 16-23 = 最大 LVT 项编号 6。
     * Linux 用 bit24 判断「EOI 广播抑制」是否支持 —— 不置位。*/
    v->r[VLAPIC_REG_VERSION] = 0x14 | (6u << 16);

    v->r[VLAPIC_REG_DFR] = 0xf0000000u;     /* flat 模型 */
    v->r[VLAPIC_REG_LDR] = 0x01000000u;

    /* 上电默认值：基址 0xFEE00000 | EN(11) | BSP(8)。EXT D 位恒 0 —— 本
     * 实现不做 x2APIC，Linux 探测到之后会安心走 MMIO 路径。*/
    v->apic_base = VLAPIC_MMIO_BASE | VLAPIC_BASE_ENABLE | (1ULL << 8);

    update_ppr(v);
    s_enabled = 1;
}

uint64_t vlapic_apic_base(void)
{
    return g_vlapic[0].apic_base;
}

void vlapic_set_apic_base(uint64_t val)
{
    g_vlapic[0].apic_base = val;
}

/* 待注入向量取走（取走后清零）*/
int vlapic_take_pending(uint32_t *vec)
{
    if (!g_vlapic[0].pending_vec)
        return 0;
    *vec = g_vlapic[0].pending_vec;
    g_vlapic[0].pending_vec = 0;
    return 1;
}

int vlapic_sw_enabled(void)
{
    return s_enabled && (g_vlapic[0].r[VLAPIC_REG_SVR] & SVR_ENABLE) != 0;
}

/* ── MMIO 访问 ────────────────────────────────────────────── */

static uint32_t reg_read(vlapic_t *v, uint32_t idx)
{
    switch (idx) {
    case VLAPIC_REG_ID:
        return v->r[VLAPIC_REG_ID];
    case VLAPIC_REG_VERSION:
    case VLAPIC_REG_APR:
    case VLAPIC_REG_RRR:
        return 0;                       /* 只读占位，恒 0 */
    case VLAPIC_REG_PPR:
        update_ppr(v);
        return v->r[VLAPIC_REG_PPR];
    case VLAPIC_REG_ISR + 0: case VLAPIC_REG_ISR + 1:
    case VLAPIC_REG_ISR + 2: case VLAPIC_REG_ISR + 3:
    case VLAPIC_REG_ISR + 4: case VLAPIC_REG_ISR + 5:
    case VLAPIC_REG_ISR + 6: case VLAPIC_REG_ISR + 7:
        return v->isr[idx - VLAPIC_REG_ISR];
    case VLAPIC_REG_TMR + 0: case VLAPIC_REG_TMR + 1:
    case VLAPIC_REG_TMR + 2: case VLAPIC_REG_TMR + 3:
    case VLAPIC_REG_TMR + 4: case VLAPIC_REG_TMR + 5:
    case VLAPIC_REG_TMR + 6: case VLAPIC_REG_TMR + 7:
        return v->tmr[idx - VLAPIC_REG_TMR];
    case VLAPIC_REG_IRR + 0: case VLAPIC_REG_IRR + 1:
    case VLAPIC_REG_IRR + 2: case VLAPIC_REG_IRR + 3:
    case VLAPIC_REG_IRR + 4: case VLAPIC_REG_IRR + 5:
    case VLAPIC_REG_IRR + 6: case VLAPIC_REG_IRR + 7:
        return 0;                       /* 上游同样没实现 IRR 队列 */
    case VLAPIC_REG_TIMER_CUR: {
        /* 当前计数 = 剩余纳秒 >> shift（与写入时的换算互逆）*/
        uint64_t now = vlapic_now_ns();
        uint64_t left = (v->t_active && v->t_deadline_ns > now)
                      ? (v->t_deadline_ns - now) : 0;
        return (uint32_t)(left >> v->t_shift);
    }
    default:
        /* 未实现/保留偏移：读回 0，**绝不 panic**（上游会打挂宿主）*/
        if (idx < VLAPIC_REG_COUNT)
            return v->r[idx];
        return 0;
    }
}

static void reg_write(vlapic_t *v, uint32_t idx, uint32_t val)
{
    switch (idx) {
    case VLAPIC_REG_ID:
        return;                          /* 只读 */

    case VLAPIC_REG_TPR:
        v->r[VLAPIC_REG_TPR] = val & 0xff;
        update_ppr(v);
        return;

    case VLAPIC_REG_EOI:
        process_eoi(v);
        return;

    case VLAPIC_REG_SVR:
        v->r[VLAPIC_REG_SVR] = val;
        if (!(val & SVR_ENABLE)) {
            /* 软件关掉 APIC：定时器一起停（上游 write_svr 同此语义）*/
            timer_stop(v);
            v->pending_vec = 0;
        } else if (v->r[VLAPIC_REG_TIMER_INIT] &&
                   !(v->r[VLAPIC_REG_LVT_TIMER] & LVT_MASKED)) {
            timer_start(v);              /* 重新使能 → 按原初值重启 */
        }
        return;

    case VLAPIC_REG_ESR:
        v->r[VLAPIC_REG_ESR] = 0;        /* 写 = 清挂起位 */
        return;

    case VLAPIC_REG_ICR_HI:
        v->r[VLAPIC_REG_ICR_HI] = val;
        return;

    case VLAPIC_REG_ICR_LO: {
        uint32_t mode = val & ICR_DELIVERY_MASK;
        uint32_t dest = val & ICR_DEST_MASK;

        v->r[VLAPIC_REG_ICR_LO] = val;

        if (mode == ICR_MODE_INIT || mode == ICR_MODE_SIPI) {
            /* INIT/SIPI 会启从核 —— 本 VMM 单 vCPU，明确不支持 */
            KLOG_WARN_ONCE("[vlapic] INIT/SIPI ignored (single vCPU only)\n");
            return;
        }
        if (mode != ICR_MODE_FIXED)
            return;                      /* lowest-priority 等：忽略（上游会 panic）*/

        /* 单 vCPU：只有「发给自己」是有意义的 */
        if (dest == ICR_DEST_SELF || dest == ICR_DEST_ALL ||
            dest == ICR_DEST_ALL_BUT_SELF)
            v->pending_vec = LVT_VECTOR(val);
        return;
    }

    case VLAPIC_REG_LVT_TIMER:
        v->r[VLAPIC_REG_LVT_TIMER] = val;
        if (val & LVT_MASKED)
            timer_stop(v);
        else
            timer_start(v);              /* 解除屏蔽 → 按初值重启 */
        return;

    case VLAPIC_REG_TIMER_INIT:
        v->r[VLAPIC_REG_TIMER_INIT] = val;
        timer_start(v);
        return;

    case VLAPIC_REG_TIMER_DCR:
        v->r[VLAPIC_REG_TIMER_DCR] = val;
        v->t_shift = dcr_to_shift(val);
        return;

    default:
        if (idx < VLAPIC_REG_COUNT)
            v->r[idx] = val;             /* LVT LINT0/1、error 等：只存值 */
        return;
    }
}

/*
 * vlapic_mmio_handle — EPT violation 转发进来的 LAPIC 访问
 *
 * 返回 1 = 已处理。addr 是 guest 物理地址（0xFEE00000 起）。
 */
int vlapic_mmio_handle(uint64_t addr, int is_write, uint8_t size, uint64_t *val)
{
    vlapic_t *v = &g_vlapic[0];
    uint32_t  off = (uint32_t)(addr & 0xfff);
    uint32_t  idx = off >> 4;            /* 16 字节步长 → 寄存器编号 */

    if (off & 0xf)
        return 0;                        /* 非对齐访问：不是 LAPIC 语义 */

    if (size != 4)
        return 0;                        /* xAPIC 只支持 32 位访问 */

    if (idx >= VLAPIC_REG_COUNT)
        return 1;                        /* 保留区：静默吞掉，读 0 */

    if (is_write)
        reg_write(v, idx, (uint32_t)*val);
    else
        *val = reg_read(v, idx);

    /* TEMP-DBG：前 60 次 LAPIC 访问 —— 看内核到底读/写了哪些寄存器、
     * 我的 vLAPIC 回了什么。APIC_ID(0x20) / SPIV(0xf0) / LVT0(0x35) /
     * LVT1(0x36) / TPR(0x08) / SVR(0x0f) 是关键。*/
    {
        static unsigned n;
        if (n < 60) {
            n++;
            KLOG_WARN("[LAPIC] #%u off=0x%03x idx=0x%02x %s val=0x%llx\n",
                      n, off, idx, is_write ? "WR" : "RD",
                      (unsigned long long)(is_write ? *val : *val));
        }
    }
    return 1;
}
