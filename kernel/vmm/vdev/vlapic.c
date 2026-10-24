/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/vmm/vdev/vlapic.c — x86_64 虚拟 LAPIC(xAPIC / MMIO 模式)

   移植自 tgoskits `virtualization/x86_vlapic`(Rust, ~4400 行), 只取
   「Linux 能启动」所必需的最小子集。上游那些完整实现(vIOAPIC/vPIC/PIT/
   x2APIC/MSI)留着, 等需要外部设备中断时再按同一结构补。

   传输方式：MMIO @0xFEE00000。
     EPT 把 guest RAM 之外的所有 GPA 都置成无效(x86_ept_enable_mmio_trap),
     而 0xFEE00000 正在其中 —— 于是 guest 的每次 LAPIC 访问天然变成
     EPT violation, 走到这里, 不需要 VMX 的 APIC-access page 机制。

   定时器：宿主 tick 驱动(轮询, 不注册宿主定时器)。
     每次进 guest 前调 vlapic_timer_poll(), 看虚拟 deadline 是否已过。
     因为开了 PIN_EXTINT, 宿主 100Hz 的时钟中断本身就会让 guest 每 10ms
     退出来一次 —— 这个频率正好充当轮询节拍, 不需要额外的定时器基础设施。

   多 vCPU（x86 SMP guest）：
     - INIT/SIPI 由 guest 写 ICR 触发, SIPI 经 vmm_vcpu_start() 放行
       睡在 completion 上的 AP 任务(实模式 CS base = vector << 12)；
     - fixed/lowest IPI 按 physical、logical flat 与 shorthand 解析目标,
       经 vmm_vcpu_inject_vector() 投递(持目标 irq_lock 后 kick)；
     - 每个 vCPU 一份 LAPIC 状态(ID/LDR/IRR/ISR/定时器), 由 vlapic_of()
       按 (slot, vcpu_id) 取。

   有意没做的(第一版不需要)：
     - x2APIC：APIC base MSR 的 EXTD 位一律清掉, Linux 探测到之后会
       安心走 MMIO 路径(tgoskits 的官方 Linux 配置也是 `nox2apic`)。
     - 对已 RUNNING 的 vCPU 的 INIT(复位/热插拔)：不复位 VMCS。
     - TPR/PPR 门控：IRR → 投递这一步不看 TPR/PPR(上游也不看), 只按
       向量编号取最大的。Linux 的 TPR 常为 0, 等价；真要做实时优先级, 得把
       update_ppr() 的结果接进 vlapic_take_pending()。
       注意 accept_interrupt 是在投递那一刻置 ISR 位的(不是 guest 取走
       之后), 所以 ISR 反映的是「我们发过, 还没收到 EOI」。
     - LVT LINT0/LINT1/THERMAL/PMI/CMCI：只存值, 不产生行为。上游同样,
       PIC 的中断不经过 LINT0, 而是 hypervisor 主动注入。

   移植时改掉的上游坑(照抄会打挂宿主)：
     - 上游对未列出的寄存器偏移直接 panic；这里一律「读回 0, 写忽略」。
     - 上游 ICR 用 lowest-priority 模式会 `unimplemented!()`；这里降级为忽略。
*/

#include "vmm/vmm_vlapic.h"
#include "vmm/vmm.h" /* vm_t：vlapic_state_t 的宿主 */
#include "vmm/vmm.h"
#include "klog.h"
#include "string.h"
#include "irq/lapic.h" /* g_tsc_freq_hz(宿主已标定的 TSC 频率) */

/* ── 虚拟 APIC timer 的「频率」───────────────────────────────

   对齐 tgoskits 的 APIC_TIMER_TICKS_PER_NANO = 1, 即 1 个 APIC tick 当
   1 纳秒 → 等效 1 GHz(再按 DCR 分频)。

   这个值是约定不是测量：Linux 启动时会自己校准 LAPIC timer
   (它数一个 jiffy 内 CurrentCount 掉了多少), 所以具体频率不重要,
   重要的是稳定。别随手改, 改了 Linux 那边感觉到的周期会跟着变。
*/
#define VLAPIC_TIMER_HZ        1000000000ULL
#define VLAPIC_PERIODIC_MIN_NS 200000ULL /* 周期模式最小 200us(照抄 KVM) */

/* LVT 位 */
#define LVT_MASKED           (1u << 16)
#define LVT_MODE_MASK        (3u << 17)
#define LVT_MODE_ONESHOT     (0u << 17)
#define LVT_MODE_PERIODIC    (1u << 17)
#define LVT_MODE_TSCDEADLINE (2u << 17)
#define LVT_VECTOR(v)        ((v) & 0xff)

/* SVR 位 */
#define SVR_ENABLE (1u << 8)

/* ICR 位 */
#define ICR_DELIVERY_MASK     (7u << 8)
#define ICR_MODE_FIXED        (0u << 8)
#define ICR_MODE_LOWEST       (1u << 8)
#define ICR_MODE_INIT         (5u << 8)
#define ICR_MODE_SIPI         (6u << 8)
#define ICR_DEST_LOGICAL      (1u << 11) /* 0 = physical, 1 = logical */
#define ICR_DEST_MASK         (7u << 18)
#define ICR_DEST_SELF         (1u << 18)
#define ICR_DEST_ALL          (2u << 18)
#define ICR_DEST_ALL_BUT_SELF (3u << 18)
#define ICR_LEVEL_ASSERT      (1u << 14)
#define ICR_TRIGGER_LEVEL     (1u << 15)

/*
   ── 每 VM × 每 vCPU 一份的设备槽位 ──────────────────────────────────

   注意： 从前这两行是：
       static vlapic_t g_vlapic[MAX_VCPUS];
       static int      s_enabled;
   按 vcpu_id 索引 —— 而 vcpu_id 是每个 VM 内部从 0 开始的编号, 于是两个
   VM 的 vcpu0 指向同一份 LAPIC。第二个 VM 一 init 就把第一个 VM 的寄存器清零,
   之后两个 VM 的 LAPIC 访问互相踩。后来嵌进 vm_t(每 VM 一份), 现在按
   (slot, vcpu_id) 索引成静态池 —— 与 stage2/gstage/ept 同模式。

   注意本设备没有 mmio_device_t(不走 MMIO 总线, 由 vmx.c 直接调
   vlapic_mmio_handle), 也没有锁(只有 guest 一条访问路径, 与 vGIC 后端
   同理)—— 所以槽位比 vpl011/uart16550 少两样东西。
*/
typedef struct {
    struct vm *owner; /* of() 的守卫, 见下 */
    vlapic_state_t st;
} vlapic_slot_t;

static vlapic_slot_t g_vlapic[MAX_VMS][VLAPIC_MAX_VCPUS];

/*
   取 (vm, vcpu_id) 的 LAPIC 状态。

   除范围检查还要比对 owner —— vm_free 会把整个 vm_t memset 成 0(含 slot),
   并发窗口里已释放的 vm 指针会索引到 0 号槽(另一个 VM 的 LAPIC)。
   owner 由 init 写入, destroy 清成 NULL。

   返回 NULL 时调用方必须能容忍(见各调用点的守卫)。
*/
static vlapic_state_t *vlapic_of(const vm_t *vm, uint32_t vcpu_id)
{
    vlapic_slot_t *d;

    if (!vm || (unsigned)vm->slot >= (unsigned)MAX_VMS)
        return NULL;
    d = &g_vlapic[vm->slot][vcpu_id % VLAPIC_MAX_VCPUS];
    if (d->owner != vm)
        return NULL;
    return &d->st;
}

/* ── 时间源 ─────────────────────────────────────────────────

   用宿主已标定的 TSC(g_tsc_freq_hz, 由 lapic_timer_init 经 PIT 测出)
   换算成纳秒。毫秒级的 timer_get_uptime_ms() 太粗 —— Linux 的 LAPIC
   timer 周期通常只有 1~10ms, 用毫秒粒度会让它的校准量出错误的值。

   除数先化成 MHz 再乘, 避免 tsc(2.4e12) * 1e9 溢出。
*/
uint64_t vlapic_now_ns(void)
{
    uint32_t lo, hi;
    uint64_t tsc, mhz;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    tsc = lo | ((uint64_t)hi << 32);

    if (g_tsc_freq_hz == 0)
        return 0; /* 还没标定：调用方按 0 处理 */

    mhz = g_tsc_freq_hz / 1000000ULL;
    if (mhz == 0)
        return 0;
    return (tsc / mhz) * 1000ULL;
}

/*
   Guest 执行时间累加。定时器的一切 "now" 都用这个时钟，不用墙钟 ——
   原因见 vmm_vlapic.h 里 exec_ns 的说明。
*/
void vlapic_add_exec_ns(vm_t *vm, uint32_t vcpu_id, uint64_t ns)
{
    vlapic_state_t *v = vlapic_of(vm, vcpu_id);

    if (v && ns)
        v->exec_ns += ns;
}

/* ── ISR / 优先级 ─────────────────────────────────────────── */

/* 在 256 位位图里找编号最大的置位向量(-1 = 一个都没有) */
static int vec_highest(const uint32_t *bits)
{
    for (int i = 7; i >= 0; i--) {
        if (bits[i])
            return i * 32 + (31 - __builtin_clz(bits[i]));
    }
    return -1;
}

/* 找到 ISR 中编号最大的已置位向量(EOI 从这里开始退) */
static int isr_highest(const vlapic_state_t *v)
{
    return vec_highest(v->isr);
}

static uint8_t priority_class(uint32_t vec)
{
    return (uint8_t)(vec >> 4);
}

/* PPR = max(TPR, 当前正在服务的最高优先级) */
static void update_ppr(vlapic_state_t *v)
{
    uint8_t tpr = (uint8_t)(v->r[VLAPIC_REG_TPR] & 0xff);
    int top = isr_highest(v);
    uint8_t isrv = (top >= 0) ? priority_class((uint32_t)top) : 0;

    v->r[VLAPIC_REG_PPR] = (tpr > isrv) ? tpr : isrv;
}

void vlapic_accept_interrupt(vm_t *vm, uint32_t vcpu_id, uint32_t vector, int level_triggered)
{
    vlapic_state_t *v = vlapic_of(vm, vcpu_id);

    if (!v || vector == 0 || vector > 255)
        return;
    v->isr[vector / 32] |= (1u << (vector % 32));
    if (level_triggered)
        v->tmr[vector / 32] |= (1u << (vector % 32));
    update_ppr(v);
}

static void process_eoi(vlapic_state_t *v)
{
    int top = isr_highest(v);

    if (top < 0)
        return;
    v->isr[(uint32_t)top / 32] &= ~(1u << ((uint32_t)top % 32));
    v->tmr[(uint32_t)top / 32] &= ~(1u << ((uint32_t)top % 32));
    update_ppr(v);
}

/* ── 定时器 ───────────────────────────────────────────────── */

/* DCR 编码 → 分频指数(Intel SDM Vol 3A 表 11-19)
 *
 * 注意：这张表曾经整体错位一格(0x3 当成 ÷8)，于是 Linux 按 ÷16 算出来的
 * INIT 值在这里被当成 ÷8 执行 —— guest 的 tick 快一倍，jiffies 也快一倍。
 * 单 vCPU 时只是"有点怪"，多 vCPU 共享宿主核时会把 CPU 全吃在 tick /
 * softirq 上，guest 的 CPU bring-up 迟迟推进不了。编码 0xB 是 ÷1。
 */
static uint32_t dcr_to_shift(uint32_t dcr)
{
    switch (dcr & 0xb) {
    case 0x0:
        return 1; /* ÷2 */
    case 0x1:
        return 2; /* ÷4 */
    case 0x2:
        return 3; /* ÷8 */
    case 0x3:
        return 4; /* ÷16 */
    case 0x8:
        return 5; /* ÷32 */
    case 0x9:
        return 6; /* ÷64 */
    case 0xa:
        return 7; /* ÷128 */
    case 0xb:
        return 0; /* ÷1 */
    default:
        return 0; /* 保留编码：按 ÷1 */
    }
}

static void timer_start(vlapic_state_t *v)
{
    uint32_t init = v->r[VLAPIC_REG_TIMER_INIT];
    uint64_t ticks;

    v->t_active = 0;
    v->t_periodic = ((v->r[VLAPIC_REG_LVT_TIMER] & LVT_MODE_MASK) == LVT_MODE_PERIODIC);

    if (init == 0)
        return; /* 写 0 = 停 */
    if (v->r[VLAPIC_REG_LVT_TIMER] & LVT_MASKED)
        return; /* LVT 屏蔽着：不启动 */

    /* interval_ns = (init << shift) / VLAPIC_TIMER_HZ * 1e9
       由于 VLAPIC_TIMER_HZ 就是 1e9, tick 数直接等于纳秒数。
    */
    ticks = (uint64_t)init << v->t_shift;

    if (v->t_periodic && ticks < VLAPIC_PERIODIC_MIN_NS)
        ticks = VLAPIC_PERIODIC_MIN_NS; /* 周期模式的下限, 照抄 KVM */

    v->t_interval_ns = ticks;
    v->t_deadline_ns = v->exec_ns + ticks;
    v->t_active = 1;
}

static void timer_stop(vlapic_state_t *v)
{
    v->t_active = 0;
}

/* 定时器到点 → 排一个待注入向量(投给拥有这个定时器的那颗 vCPU) */
static void timer_fire(vm_t *vm, uint32_t vcpu_id, vlapic_state_t *v)
{
    uint32_t vec = LVT_VECTOR(v->r[VLAPIC_REG_LVT_TIMER]);

    vlapic_raise_irq(vm, vcpu_id, vec); /* 向量合法性(≥16)在内部把关 */

    if (v->t_periodic) {
        /* 追赶：落后多个周期时不要补发一堆, 直接跳到未来的第一个点 */
        do {
            v->t_deadline_ns += v->t_interval_ns;
        } while (v->t_deadline_ns <= v->exec_ns);
    } else {
        v->t_active = 0;
    }
}

/*
   vlapic_timer_poll — 每次进 guest 前调用

   返回 1 表示这次轮询产生了待注入的定时器中断。
*/
int vlapic_timer_poll(vm_t *vm, uint32_t vcpu_id)
{
    vlapic_state_t *v = vlapic_of(vm, vcpu_id);
    int fired = 0;

    if (!v)
        return 0;
    if (!v->enabled || !(v->r[VLAPIC_REG_SVR] & SVR_ENABLE))
        return 0; /* APIC 被软件关掉：什么都不投 */
    if (!v->t_active)
        return 0;

    while (v->t_active && v->exec_ns >= v->t_deadline_ns) {
        timer_fire(vm, vcpu_id, v);
        fired = 1;
    }
    return fired;
}

/* ── 初始化 ───────────────────────────────────────────────── */

void vlapic_init(vm_t *vm, uint32_t vcpu_id)
{
    vlapic_slot_t *d;
    vlapic_state_t *v;

    if (!vm || (unsigned)vm->slot >= (unsigned)MAX_VMS)
        return;
    /* 直接取槽位：of() 要校验 owner, 而 owner 正是本函数要写的 */
    d = &g_vlapic[vm->slot][vcpu_id % VLAPIC_MAX_VCPUS];
    d->owner = vm;
    v = &d->st;

    memset(v, 0, sizeof(*v));

    /* ID：Linux 用它做拓扑/自检。我们的 APIC ID 约定等于 vcpu_id
       (MP 表、CPUID.1.EBX[31:24] 与这里必须是同一个值)。
       注意 ID 寄存器只读 —— 写它必须被忽略, 否则 Linux 会认为
       「APIC ID 可写」而走进不该走的分支。
    */
    v->r[VLAPIC_REG_ID] = (vcpu_id & 0xff) << 24;

    /* VERSION：0x14 = 版本 20, bits 16-23 = 最大 LVT 项编号 6。
       Linux 用 bit24 判断「EOI 广播抑制」是否支持 —— 不置位。
    */
    v->r[VLAPIC_REG_VERSION] = 0x14 | (6u << 16);

    /*
       ── 逻辑 destination（flat 模型）─────────────────────────────

       Linux 在核数 <= 8 时用 apic_flat：IPI 走 logical 模式, ICR 高 8 位
       是「CPU 位图」, 目标 CPU 的 LDR 在同一个位上。原来的 0x01000000 把
       所有 vCPU 的逻辑 ID 都设成 0 —— 单核没事, 多核时每一条 logical IPI
       都会投给所有 vCPU(或只投给 vcpu0)，而且目标是靠 guest 自己写 LDR
       纠正的, 早期 IPI 会丢。
       guest 之后会自己写 LDR, 这里给的是与 APIC ID 一致的默认值。
    */
    v->r[VLAPIC_REG_DFR] = 0xf0000000u; /* flat 模型 */
    v->r[VLAPIC_REG_LDR] = (vcpu_id < 8) ? (1u << (24 + vcpu_id)) : 0;

    /* 上电默认值：基址 0xFEE00000 | EN(11) | BSP(8)。EXT D 位恒 0 —— 本
       实现不做 x2APIC, Linux 探测到之后会安心走 MMIO 路径。
       BSP 位只有 0 号 vCPU 置位：AP 的 IA32_APIC_BASE 里这一位是只读的
       拓扑信息, 全置 1 会让 Linux 认为每颗 CPU 都是 boot CPU。
    */
    v->apic_base = VLAPIC_MMIO_BASE | VLAPIC_BASE_ENABLE | (vcpu_id == 0 ? (1ULL << 8) : 0);

    update_ppr(v);
    v->enabled = 1;
}

/*
   vlapic_destroy — 归还本 VM 的全部 LAPIC 槽位
  (由 vmm_arch_vm_destroy → vm_free 调用)

   幂等, 且对"从未 init 过的槽"安全(槽位是全零 static)。
   本设备没有锁, 所以不需要 vpl011_destroy 里那条"永不 memset 锁"的约束。
*/
void vlapic_destroy(vm_t *vm)
{
    uint32_t i;

    if (!vm || (unsigned)vm->slot >= (unsigned)MAX_VMS)
        return;
    for (i = 0; i < VLAPIC_MAX_VCPUS; i++) {
        vlapic_slot_t *d = &g_vlapic[vm->slot][i];

        memset(&d->st, 0, sizeof(d->st));
        d->owner = NULL; /* of() 随即失效 */
    }
}

uint64_t vlapic_apic_base(vm_t *vm, uint32_t vcpu_id)
{
    vlapic_state_t *v = vlapic_of(vm, vcpu_id);

    return v ? v->apic_base : 0;
}

void vlapic_set_apic_base(vm_t *vm, uint32_t vcpu_id, uint64_t val)
{
    vlapic_state_t *v = vlapic_of(vm, vcpu_id);

    if (v)
        v->apic_base = val;
}

/*
   拉高一条中断线(电平触发语义)

   三个来源都走这里进 IRR：LAPIC 定时器, ICR 自发中断, 外部设备(vUART)。
   IRR 是位图不是单个槽 —— 之前只存一个 pending_vec, 定时器和设备中断
   会互相覆盖：谁后到谁赢, 输的那个要等下次事件才补得回来(周期定时器甚至
   会因此整拍丢掉)。

   位在 vlapic_take_pending() 取走时才清, 所以电平触发的设备(UART RX
   FIFO 里还有字节)必须每次入口重新拉一次, 与 vPLIC/vGIC 侧的约定一致
   (见 vmm_console_irq_asserted 的注释)。
*/
void vlapic_raise_irq(vm_t *vm, uint32_t vcpu_id, uint32_t vector)
{
    vlapic_state_t *v = vlapic_of(vm, vcpu_id);

    if (!v)
        return;
    if (vector < 16 || vector > 255)
        return; /* 0-15 是异常, 不能当普通中断投 */
    if (!v->enabled || !(v->r[VLAPIC_REG_SVR] & SVR_ENABLE))
        return; /* APIC 被软件关掉：什么都不投 */

    v->irr[vector / 32] |= (1u << (vector % 32));
}

/*
   待注入向量取走：IRR 里编号最大的, 且不在服务中的那个(取走后清位)

   「不在服务中」这一条不能省：硬件的投递优先级是
       向量优先级(编号) > 正在服务的最高优先级(ISR) > TPR
   同一向量在 ISR 里挂着时, 新的中断只在 IRR 里排队, 要等 guest 写 EOI
   才投得出去。

   电平触发的设备(vUART 的 RX/THRE)会把线一直拉着, 少了这个门控就
   变成「每个 VM-entry 投一次」—— 空闲时 guest 每条 HLT 退出都会吃一个
   串口中断(实测 2.8 万次/秒量级), 串口驱动被中断风暴拖死。有了门控,
   速率自然回落到「每次 EOI 一个」, 与真硬件一致。

   注意： ISR 位是在投递那一刻由 vlapic_accept_interrupt() 置的, 所以这里
   的前提是 guest 一定会 EOI(Linux 的中断处理开头就 EOI)。若某个向量投出去
   而 guest 从不 EOI, 它会被永久挡住 —— 这是有意的：真硬件也是这个行为。
*/
int vlapic_take_pending(vm_t *vm, uint32_t vcpu_id, uint32_t *vec)
{
    vlapic_state_t *v = vlapic_of(vm, vcpu_id);

    if (!v)
        return 0;
    for (int i = 7; i >= 0; i--) {
        uint32_t cand = v->irr[i] & ~v->isr[i];

        if (cand) {
            int top = i * 32 + (31 - __builtin_clz(cand));
            v->irr[i] &= ~(1u << (top % 32));
            *vec = (uint32_t)top;
            return 1;
        }
    }
    return 0;
}

int vlapic_sw_enabled(vm_t *vm, uint32_t vcpu_id)
{
    vlapic_state_t *v = vlapic_of(vm, vcpu_id);

    return v && v->enabled && (v->r[VLAPIC_REG_SVR] & SVR_ENABLE) != 0;
}

/* ── MMIO 访问 ────────────────────────────────────────────── */

static uint32_t reg_read(vlapic_state_t *v, uint32_t idx)
{
    switch (idx) {
    case VLAPIC_REG_ID:
        return v->r[VLAPIC_REG_ID];
    case VLAPIC_REG_VERSION:
    case VLAPIC_REG_APR:
    case VLAPIC_REG_RRR:
        return 0; /* 只读占位, 恒 0 */
    case VLAPIC_REG_PPR:
        update_ppr(v);
        return v->r[VLAPIC_REG_PPR];
    case VLAPIC_REG_ISR + 0:
    case VLAPIC_REG_ISR + 1:
    case VLAPIC_REG_ISR + 2:
    case VLAPIC_REG_ISR + 3:
    case VLAPIC_REG_ISR + 4:
    case VLAPIC_REG_ISR + 5:
    case VLAPIC_REG_ISR + 6:
    case VLAPIC_REG_ISR + 7:
        return v->isr[idx - VLAPIC_REG_ISR];
    case VLAPIC_REG_TMR + 0:
    case VLAPIC_REG_TMR + 1:
    case VLAPIC_REG_TMR + 2:
    case VLAPIC_REG_TMR + 3:
    case VLAPIC_REG_TMR + 4:
    case VLAPIC_REG_TMR + 5:
    case VLAPIC_REG_TMR + 6:
    case VLAPIC_REG_TMR + 7:
        return v->tmr[idx - VLAPIC_REG_TMR];
    case VLAPIC_REG_IRR + 0:
    case VLAPIC_REG_IRR + 1:
    case VLAPIC_REG_IRR + 2:
    case VLAPIC_REG_IRR + 3:
    case VLAPIC_REG_IRR + 4:
    case VLAPIC_REG_IRR + 5:
    case VLAPIC_REG_IRR + 6:
    case VLAPIC_REG_IRR + 7:
        /*
           真正回位图(上游是恒 0)。guest 用 IRR 判断「还有没有中断挂着」,
           恒 0 会让它在中断处理里误判。注意这是只读的 —— 清位只有
           vlapic_take_pending()(投递)和 SVR 关闭两条路。
        */
        return v->irr[idx - VLAPIC_REG_IRR];
    case VLAPIC_REG_TIMER_CUR: {
        /* 当前计数 = 剩余纳秒 >> shift(与写入时的换算互逆) */
        uint64_t left = (v->t_active && v->t_deadline_ns > v->exec_ns) ? (v->t_deadline_ns - v->exec_ns) : 0;
        return (uint32_t)(left >> v->t_shift);
    }
    default:
        /* 未实现/保留偏移：读回 0, 绝不 panic(上游会打挂宿主) */
        if (idx < VLAPIC_REG_COUNT)
            return v->r[idx];
        return 0;
    }
}

/* ── ICR：目标 vCPU 解析 ──────────────────────────────────────

   把一次 ICR 写翻译成「要投给哪些 vCPU」。APIC ID 约定等于 vcpu_id,
   所以物理 destination 直接当 vCPU 编号用。

   覆盖 Linux 实际会用到的三种形态：
     - shorthand：self / all / all-but-self（IPI 广播、self-IPI）
     - physical destination：apic_physflat（>8 核）与 INIT/SIPI 的目标
     - logical flat：apic_flat（<=8 核）的 IPI，ICR[31:24] 是 CPU 位图

   返回目标个数(0 = 没有匹配)。
*/
static int icr_collect_targets(vm_t *vm, uint32_t src, uint32_t lo, uint32_t hi, uint32_t *out, int max)
{
    uint32_t shorthand = lo & ICR_DEST_MASK;
    int n = 0;

    if (!vm)
        return 0;

    if (shorthand == ICR_DEST_SELF) {
        out[n++] = src;
        return n;
    }
    if (shorthand == ICR_DEST_ALL || shorthand == ICR_DEST_ALL_BUT_SELF) {
        for (int i = 0; i < vm->nr_vcpus && n < max; i++) {
            if (shorthand == ICR_DEST_ALL_BUT_SELF && (uint32_t)i == src)
                continue;
            out[n++] = (uint32_t)i;
        }
        return n;
    }
    if (shorthand != 0)
        return 0; /* 保留编码 */

    uint32_t dest = (hi >> 24) & 0xff;

    if (lo & ICR_DEST_LOGICAL) {
        /* flat 模型：LDR[31:24] 的每一位对应一个逻辑 ID */
        for (int i = 0; i < vm->nr_vcpus && n < max; i++) {
            vlapic_state_t *t = vlapic_of(vm, (uint32_t)i);

            if (t && ((t->r[VLAPIC_REG_LDR] >> 24) & dest))
                out[n++] = (uint32_t)i;
        }
    } else if (dest == 0xff) {
        /* 物理广播：xAPIC 里 0xFF 是「所有 CPU」 */
        for (int i = 0; i < vm->nr_vcpus && n < max; i++)
            out[n++] = (uint32_t)i;
    } else if (dest < (uint32_t)vm->nr_vcpus) {
        out[n++] = dest;
    }
    return n;
}

static void reg_write(vm_t *vm, uint32_t vcpu_id, vlapic_state_t *v, uint32_t idx, uint32_t val)
{
    switch (idx) {
    case VLAPIC_REG_ID:
        return; /* 只读 */

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
            /* 软件关掉 APIC：定时器一起停, 挂起的也丢掉(上游 write_svr 同此语义) */
            timer_stop(v);
            memset(v->irr, 0, sizeof(v->irr));
        } else if (v->r[VLAPIC_REG_TIMER_INIT] && !(v->r[VLAPIC_REG_LVT_TIMER] & LVT_MASKED)) {
            timer_start(v); /* 重新使能 → 按原初值重启 */
        }
        return;

    case VLAPIC_REG_ESR:
        v->r[VLAPIC_REG_ESR] = 0; /* 写 = 清挂起位 */
        return;

    case VLAPIC_REG_ICR_HI:
        v->r[VLAPIC_REG_ICR_HI] = val;
        return;

    case VLAPIC_REG_ICR_LO: {
        uint32_t mode = val & ICR_DELIVERY_MASK;
        uint32_t vec = LVT_VECTOR(val);
        uint32_t targets[VLAPIC_MAX_VCPUS];
        int nt;

        v->r[VLAPIC_REG_ICR_LO] = val;

        /*
           ── INIT / SIPI：AP 启动 ──────────────────────────────────

           写 ICR 的永远是 guest 的某颗 vCPU(这里就是 vcpu_id)。目标按
           destination/shorthand 解析, 与 IPI 同一套规则。

           INIT 在本实现里只起「把目标置于 wait-for-SIPI」的语义 —— 而
           AP 任务本来就在 completion 上睡到 SIPI, 所以对 OFFLINE 的 AP
           什么都不用做。对已 RUNNING 的 vCPU 的 INIT(关机/热插拔)不复位,
           真机语义需要重新初始化 VMCS, 当前不需要。
           SIPI 才是真正放行 AP 的那一步：vmm_vcpu_start 填实模式入口并
           完成 completion。第二次 SIPI(目标已 RUNNING)返回 -1, 忽略。
        */
        if (mode == ICR_MODE_INIT || mode == ICR_MODE_SIPI) {
            nt = icr_collect_targets(vm, vcpu_id, val, v->r[VLAPIC_REG_ICR_HI], targets, VLAPIC_MAX_VCPUS);
            if (mode == ICR_MODE_SIPI) {
                for (int k = 0; k < nt; k++) {
                    vcpu_t *target = vmm_vm_vcpu(vm, (int)targets[k]);

                    if (target && vmm_vcpu_start(target, (uint64_t)vec << 12, 0) == 0)
                        KLOG_INFO("[vlapic] vcpu%u: SIPI -> vcpu%u (cs base=0x%x)\n", vcpu_id, targets[k],
                                  vec << 12);
                }
            }
            return;
        }
        if (mode != ICR_MODE_FIXED && mode != ICR_MODE_LOWEST)
            return; /* SMI/NMI/ExtINT 等：未实现(上游会 panic) */

        nt = icr_collect_targets(vm, vcpu_id, val, v->r[VLAPIC_REG_ICR_HI], targets, VLAPIC_MAX_VCPUS);

        /*
           lowest-priority 的语义是「在候选里挑优先级最低的一颗」; 本实现
           不做 PPR 仲裁, 简化为投给第一个候选。Linux 的 flat/physflat
           驱动都走 fixed, 这条只是兜底。
        */
        if (mode == ICR_MODE_LOWEST && nt > 1)
            nt = 1;

        for (int k = 0; k < nt; k++) {
            vcpu_t *target = vmm_vm_vcpu(vm, (int)targets[k]);

            /*
               走 vmm_vcpu_inject_vector 而不是直接 vlapic_raise_irq：
               目标可能在另一颗宿主核上跑 guest, 必须持它的 irq_lock
               写 IRR 再 kick, 否则 IPI 要等它下次自然退出才被看见。
            */
            if (target)
                vmm_vcpu_inject_vector(target, vec);
        }
        return;
    }

    case VLAPIC_REG_LVT_TIMER:
        v->r[VLAPIC_REG_LVT_TIMER] = val;
        if (val & LVT_MASKED)
            timer_stop(v);
        else
            timer_start(v); /* 解除屏蔽 → 按初值重启 */
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
            v->r[idx] = val; /* LVT LINT0/1, error 等：只存值 */
        return;
    }
}

/*
   vlapic_mmio_handle — EPT violation 转发进来的 LAPIC 访问

   返回 1 = 已处理。addr 是 guest 物理地址(0xFEE00000 起)。
*/
int vlapic_mmio_handle(vm_t *vm, uint32_t vcpu_id, uint64_t addr, int is_write, uint8_t size, uint64_t *val)
{
    vlapic_state_t *v = vlapic_of(vm, vcpu_id);
    uint32_t off = (uint32_t)(addr & 0xfff);
    uint32_t idx = off >> 4; /* 16 字节步长 → 寄存器编号 */

    if (!v)
        return 0; /* 未处理 —— 调用方按未映射设备兜底 */
    if (off & 0xf)
        return 0; /* 非对齐访问：不是 LAPIC 语义 */

    if (size != 4)
        return 0; /* xAPIC 只支持 32 位访问 */

    if (idx >= VLAPIC_REG_COUNT)
        return 1; /* 保留区：静默吞掉, 读 0 */

    if (is_write)
        reg_write(vm, vcpu_id, v, idx, (uint32_t)*val);
    else
        *val = reg_read(v, idx);

    /* TEMP-DBG：前 60 次 LAPIC 访问 —— 看内核到底读/写了哪些寄存器,
       我的 vLAPIC 回了什么。APIC_ID(0x20) / SPIV(0xf0) / LVT0(0x35) /
       LVT1(0x36) / TPR(0x08) / SVR(0x0f) 是关键。
    */
    {
        static unsigned n;
        if (n < 60) {
            n++;
            /*
               KLOG_WARN("[LAPIC] #%u off=0x%03x idx=0x%02x %s val=0x%llx\n",
                         n, off, idx, is_write ? "WR" : "RD",
                         (unsigned long long)(is_write ? *val : *val));
            */
        }
    }
    return 1;
}
