/*
 * kernel/vmm/vdev/vpl011.c — 虚拟 PL011 UART 设备实现
 *
 * 移植自 x-kernel: virt/vdev/vpl011/src/lib.rs，适配 Avatar OS：
 *   - klogger::kprint → klog_putchar（宿主内核日志）
 *   - RxChannel → 本文件内的 RX 环形缓冲 + vpl011_push_rx()；
 *     宿主侧由 vmm_console_pump() 轮询真实 PL011 喂入
 *   - TxChannel：kvmm 里只有控制设备打开了通道才逐字节直通，否则走宿主
 *     日志。avatar 没有独立控制设备 —— 宿主控制台本身就是终端，故**始终**
 *     直通：行缓冲会把 guest 的按键回显（无换行的单字符）一直憋到回车，
 *     交互式会话不可用。见 vpl011_put_char()。
 */

#include "vmm/vmm_vpl011.h"
#include "vmm/vmm.h" /* vm_t：只为 slot 与 console_owned，设备状态已不在里面 */
#include "klog.h"
#include "string.h"
#include "spinlock.h"

/*
 * ── 每 VM 一份的设备槽位 ─────────────────────────────────────────────
 *
 * 从前这三样（状态、锁、设备对象）都嵌在 vm_t 里，本文件通篇写 `vm->vpl011*`
 * —— 于是"设备"反过来依赖 vm_t 的具体布局。现在按 vm->slot 索引成静态池，
 * 与 stage2 / gstage / ept 是同一个模式。
 *
 * 锁**跟着状态一起进槽位**：本设备有两条访问路径（guest 的 MMIO 回调 +
 * 宿主侧的 push_rx/tx_pop），必须加锁。这也是它不能照抄 vGIC 后端的原因 ——
 * 那六个文件 `vm->` 为 0 是因为它们**根本不加锁**（只有 guest 一条路径）。
 *
 * dev->priv 仍指向 &d->st（语义不变），包装器用 container_of 找回。
 */
typedef struct {
    vpl011_state_t st;     /* 纯设备寄存器/FIFO 状态；st.owner 见下 */
    spinlock_noirq_t lock; /* 从前是 vm->vpl011_lock */
    mmio_device_t dev;     /* 从前是 vm->vpl011_dev */
} vpl011_slot_t;

static vpl011_slot_t g_vpl011[MAX_VMS] __attribute__((aligned(64)));

/*
 * 取本 VM 的槽位。
 *
 * ⚠️ 除了范围检查，还要比对 `st.owner`：vm_free() 会把整个 vm_t memset 成 0
 * （含 slot），并发窗口里一个已释放的 vm 指针会索引到 **0 号槽** —— 那是
 * 另一个 VM 的控制台，TX 环会投错 VM。owner 由 init 写入、由 destroy 清成
 * NULL、槽位复用时重写，正好当这个守卫。
 */
static vpl011_slot_t *vpl011_of(const vm_t *vm)
{
    vpl011_slot_t *d;

    if (!vm || (unsigned)vm->slot >= (unsigned)MAX_VMS)
        return NULL;
    d = &g_vpl011[vm->slot];
    if (d->st.owner != vm)
        return NULL;
    return d;
}

/* ── PL011 寄存器偏移 ─────────────────────────────────────── */
#define UARTDR   0x000
#define UARTFR   0x018
#define UARTCR   0x030
#define UARTIMSC 0x038
#define UARTRIS  0x03C
#define UARTMIS  0x040
#define UARTICR  0x044

/* PrimeCell / Peripheral ID（ARM PL011 签名）*/
#define PERIPHID0 0xFE0
#define PERIPHID1 0xFE4
#define PERIPHID2 0xFE8
#define PERIPHID3 0xFEC
#define PCELLID0  0xFF0
#define PCELLID1  0xFF4
#define PCELLID2  0xFF8
#define PCELLID3  0xFFC

/* FR 标志位 */
#define FR_TXFE (1u << 7) /* TX FIFO 空 */
#define FR_RXFF (1u << 6) /* RX FIFO 满 */
#define FR_RXFE (1u << 4) /* RX FIFO 空 */

/* 中断位 */
#define INT_RX (1u << 4) /* RX 中断（RIS/MIS/IMSC bit4）*/

/*
 * RX FIFO 深度。真实 PL011 的硬件 FIFO 只有 16 字节，这里放宽到 256：
 * 宿主一次粘贴多字符时，guest 要等到下一次进中断才来取，16 字节不够用。
 */
#define VPL011_RX_FIFO_SIZE 256

/* TX 缓冲深度：要能扛住 guest 启动那一大串内核日志的突发 */
#define VPL011_TX_FIFO_SIZE 8192

/* ── 设备私有状态 ─────────────────────────────────────────── */

/*
 * 设备私有状态（设备实例由调用方持有，见 kernel/vmm/aarch64/vm_init.c）。
 *
 * 并发：两个环都是**跨任务**的 ——
 *   - guest MMIO 读写发生在 vCPU 任务里（VMM 退出路径分发），
 *     此时宿主中断是开的（vmm_run_vcpu 在调用 exit_handler 前已 restore）；
 *   - 宿主侧 push_rx / tx_pop 在 /dev/vmm 的 read/write 里，属于 helper 任务。
 * 所以两把环都要用 IRQ-safe 的锁保护，不能像同线程时期那样裸操作。
 */

/* ── RX 中断线状态 ────────────────────────────────────────── */
/* 调用者需持有 slot->lock */
static int rx_irq_asserted_locked(const vpl011_state_t *s)
{
    return s->rx_count > 0 && (s->imsc & INT_RX) != 0;
}

int vpl011_rx_irq_asserted(vm_t *vm)
{
    vpl011_slot_t *d = vpl011_of(vm);
    uint64_t flags;
    int asserted;

    if (!d)
        return 0;
    spin_lock_irqsave(&d->lock, &flags);
    asserted = rx_irq_asserted_locked(&d->st);
    spin_unlock_irqrestore(&d->lock, flags);
    return asserted;
}

/* ── 宿主侧入口：压入一个控制台字节 ───────────────────────── */
void vpl011_push_rx(vm_t *vm, uint8_t c)
{
    vpl011_slot_t *d = vpl011_of(vm);
    uint64_t flags;

    if (!d)
        return;
    spin_lock_irqsave(&d->lock, &flags);
    if (d->st.rx_count >= VPL011_RX_FIFO_SIZE) {
        d->st.rx_dropped++;
        spin_unlock_irqrestore(&d->lock, flags);
        /*
         * 满则丢弃。这里可以打日志：调用点是任务上下文（/dev/vmm 的
         * write 或 vmm_run_vcpu 的循环），不在 klog 锁内。只报第一次。
         */
        if (d->st.rx_dropped == 1)
            KLOG_WARN("[vpl011] RX FIFO full, dropping console input\n");
        return;
    }

    d->st.rx_fifo[d->st.rx_head] = c;
    d->st.rx_head = (d->st.rx_head + 1) % VPL011_RX_FIFO_SIZE;
    d->st.rx_count++;
    spin_unlock_irqrestore(&d->lock, flags);
}

void vpl011_rx_flush(vm_t *vm)
{
    vpl011_slot_t *d = vpl011_of(vm);
    uint64_t flags;

    if (!d)
        return;
    spin_lock_irqsave(&d->lock, &flags);
    d->st.rx_head = d->st.rx_tail = d->st.rx_count = 0;
    spin_unlock_irqrestore(&d->lock, flags);
}

/* ── TX 通道（guest 输出 → 用户态 helper）─────────────────── */
void vpl011_tx_set_enabled(vm_t *vm, int enabled)
{
    vpl011_slot_t *d = vpl011_of(vm);
    uint64_t flags;

    if (!d) {
        /* 设备还没 init（vmm_dev.c 在 vm_create 之前就会调这里）——
         * 归属标志在 vm_t 里，直接写即可，不能因此漏掉。*/
        if (vm)
            vm->console_owned = enabled ? 1 : 0;
        return;
    }
    spin_lock_irqsave(&d->lock, &flags);
    vm->console_owned = enabled ? 1 : 0;
    if (!enabled) {
        /* 关通道时清空残留，免得下次开通道吐出上一次会话的尾巴 */
        d->st.tx_head = d->st.tx_tail = d->st.tx_count = 0;
    }
    spin_unlock_irqrestore(&d->lock, flags);
}

int vpl011_tx_channel_enabled(vm_t *vm)
{
    vpl011_slot_t *d = vpl011_of(vm);
    uint64_t flags;
    int enabled;

    /*
     * 归属标志在 vm_t 里，设备未 init 时也要能读（vmm_dev.c 的早期调用）。
     *
     * ⚠️ 有设备时必须**在锁里读**，不要图快写成裸 `return vm->console_owned;`
     *    （这里从前就是那么写的）。那个标志过去只在 boot 时写一次 —— 那时
     *    vCPU 任务还没被创建，天然有序，裸读没暴露过问题。但 VMM_IOC_ATTACH
     *    现在会在**运行期**改它（切前台），而读它的 vmm_console_pump() 常常
     *    跑在**另一颗核**上：裸读在弱序架构上没有 acquire 语义，那一核可能
     *    好几轮之后才看见新值，表现为"刚 attach 的那几下按键有概率被泵抢走"
     *    —— 字节会被推进一台用户看不见的 guest 的 RX FIFO。
     *    uart16550 那份一直是在锁里读的，这里跟上它。
     */
    if (!d)
        return vm ? vm->console_owned : 0;
    spin_lock_irqsave(&d->lock, &flags);
    enabled = vm->console_owned;
    spin_unlock_irqrestore(&d->lock, flags);
    return enabled;
}

int vpl011_tx_has_data(vm_t *vm)
{
    vpl011_slot_t *d = vpl011_of(vm);
    uint64_t flags;
    int has;

    if (!d)
        return 0;
    spin_lock_irqsave(&d->lock, &flags);
    has = d->st.tx_count > 0;
    spin_unlock_irqrestore(&d->lock, flags);
    return has;
}

int vpl011_tx_pop(vm_t *vm, uint8_t *c)
{
    vpl011_slot_t *d = vpl011_of(vm);
    uint64_t flags;
    vpl011_state_t *s;

    if (!c || !d)
        return 0;
    s = &d->st;

    spin_lock_irqsave(&d->lock, &flags);
    if (s->tx_count == 0) {
        spin_unlock_irqrestore(&d->lock, flags);
        return 0;
    }
    *c = s->tx_fifo[s->tx_tail];
    s->tx_tail = (s->tx_tail + 1) % VPL011_TX_FIFO_SIZE;
    s->tx_count--;
    spin_unlock_irqrestore(&d->lock, flags);
    return 1;
}

/* ── 输出：通道模式走缓冲，否则直通宿主控制台 ─────────────── */
/* 调用者需持有 slot->lock */
static void put_char_locked(vpl011_state_t *s, uint8_t c)
{
    if (!s->owner->console_owned) {
        /*
         * 直启模式：立即输出。klog_putchar 直通 uart_putchar（未开中断时
         * 是直接 MMIO 写），不经行缓冲，所以 guest 按键回显是实时的。
         *
         * 注意这里**在持锁状态下**调 klog_putchar：klog 自己也有锁，且
         * uart_putchar 在缓冲满时会 timer_spin 等待。这把锁的中断已关，
         * 不至于死锁，但属于「锁内做 I/O」—— 换来的是直启模式下不引入
         * 新的锁层级。通道模式（helper）下不会走到这里。
         */
        /* 逐字符走唯一入口（与 vuart16550.c 的 uart_put_char 同理）*/
        klog_write((const char *)&c, 1);
        return;
    }

    if (s->tx_count >= VPL011_TX_FIFO_SIZE) {
        s->tx_dropped++;
        return; /* 丢弃最新字节，与 kvmm 一致 */
    }
    s->tx_fifo[s->tx_head] = c;
    s->tx_head = (s->tx_head + 1) % VPL011_TX_FIFO_SIZE;
    s->tx_count++;
}

/* ── MMIO 读写回调 ────────────────────────────────────────── */
static uint64_t vpl011_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    vpl011_state_t *s = (vpl011_state_t *)dev->priv;
    /* priv 仍指向 st（语义不变），锁在包装器里 —— 用 container_of 找回 */
    vpl011_slot_t *d = container_of(s, vpl011_slot_t, st);
    uint64_t flags;
    uint64_t ret = 0;
    (void)size;

    spin_lock_irqsave(&d->lock, &flags);

    switch (off) {
    case UARTDR: {
        uint8_t c = 0;
        if (s->rx_count > 0) {
            c = s->rx_fifo[s->rx_tail];
            s->rx_tail = (s->rx_tail + 1) % VPL011_RX_FIFO_SIZE;
            s->rx_count--;
        }
        ret = (uint64_t)c;
        break;
    }
    case UARTFR: {
        uint64_t fr = FR_TXFE; /* 输出永远不阻塞 */
        if (s->rx_count == 0)
            fr |= FR_RXFE;
        if (s->rx_count >= VPL011_RX_FIFO_SIZE)
            fr |= FR_RXFF;
        ret = fr;
        break;
    }
    case UARTCR:
        ret = s->cr;
        break;
    case UARTIMSC:
        ret = s->imsc;
        break;
    case UARTRIS:
        /* 电平触发：数据还在 FIFO 里就一直为高 */
        ret = rx_irq_asserted_locked(s) ? INT_RX : 0;
        break;
    case UARTMIS:
        ret = rx_irq_asserted_locked(s) ? INT_RX : 0;
        break;
    case PERIPHID0:
        ret = 0x11;
        break;
    case PERIPHID1:
        ret = 0x10;
        break;
    case PERIPHID2:
        ret = 0x14;
        break;
    case PERIPHID3:
        ret = 0x00;
        break;
    case PCELLID0:
        ret = 0x0D;
        break;
    case PCELLID1:
        ret = 0xF0;
        break;
    case PCELLID2:
        ret = 0x05;
        break;
    case PCELLID3:
        ret = 0xB1;
        break;
    default:
        ret = 0;
        break;
    }

    spin_unlock_irqrestore(&d->lock, flags);
    return ret;
}

static void vpl011_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                         uint64_t value)
{
    vpl011_state_t *s = (vpl011_state_t *)dev->priv;
    vpl011_slot_t *d = container_of(s, vpl011_slot_t, st);
    uint64_t flags;
    (void)size;

    spin_lock_irqsave(&d->lock, &flags);

    switch (off) {
    case UARTDR:
        put_char_locked(s, (uint8_t)value);
        break;
    case UARTCR:
        s->cr = (uint32_t)value;
        break;
    case UARTIMSC:
        /* RX 中断的使能位只影响 RIS/MIS 的呈现，不改变 FIFO 内容；
         * 置 pending 由调用方在进入 guest 前按 vpl011_rx_irq_asserted() 做。*/
        s->imsc = (uint32_t)value;
        break;
    case UARTICR:
        /* RIS 完全由 FIFO 状态导出（电平触发），没有需要清的粘滞位 */
        break;
    default:
        break;
    }

    spin_unlock_irqrestore(&d->lock, flags);
}

static const mmio_dev_ops_t g_vpl011_ops = {
    .name = "vpl011",
    .base = VPL011_BASE,
    .size = VPL011_SIZE,
    .read = vpl011_read,
    .write = vpl011_write,
};

int vpl011_init(vm_t *vm, mmio_bus_t *bus)
{
    vpl011_slot_t *d;

    /* 设备对象在池里，不再由调用方传进来 */
    if (!vm || !bus || (unsigned)vm->slot >= (unsigned)MAX_VMS)
        return -1;
    d = &g_vpl011[vm->slot];

    /*
     * 只清状态与设备对象，**不碰锁**（见 vpl011_destroy 的说明）。
     *
     * 从前这里要"先存后恢复 tx_channel" —— 因为归属标志当时是 state 的一个
     * 字段，而 memset 会把它清回 0，导致通道模式失效、vmm_console_pump 复活
     * 去跟用户态 helper 抢同一个 UART（GUEST_CONSOLE.md §2 记着这个陷阱）。
     * 归属标志仍在 vm_t.console_owned，不在这个结构里，所以陷阱不适用于
     * 状态清零本身 —— 它现在只对"锁"这一类跨 init 存活的对象有意义。
     */
    memset(&d->st, 0, sizeof(d->st));
    d->st.cr = 0x301; /* UARTEN | TXE | RXE */
    d->st.owner = vm; /* of() 靠它认出"这个槽位属于谁" */

    d->dev.ops = &g_vpl011_ops;
    d->dev.priv = &d->st;

    return mmio_bus_register(bus, &d->dev);
}

/*
 * vpl011_destroy — 归还本 VM 的槽位（由 vmm_arch_vm_destroy → vm_free 调用）
 *
 * 三条硬要求：
 *   ① **幂等**、且对"从未 init 过的槽"安全 —— vmm_dev.c 的回滚路径会在
 *      vm_create 失败时调 vm_free，此时设备一次都没 init（状态全零、
 *      锁是静态零值 = 解锁态，memset 是空操作）。
 *   ② **永不 memset 锁对象本身** —— 清零一个正被持有的锁等于凭空放锁，
 *      两个任务会同时进临界区。只清 st 与 dev。
 *   ③ 必须在 g_vm_pool_lock **外面**调用：它靠设备自己的锁串行化，
 *      而池锁的临界区里不应该做设备操作。
 */
void vpl011_destroy(vm_t *vm)
{
    vpl011_slot_t *d;
    uint64_t flags;

    if (!vm || (unsigned)vm->slot >= (unsigned)MAX_VMS)
        return;
    d = &g_vpl011[vm->slot];

    spin_lock_irqsave(&d->lock, &flags);
    memset(&d->st, 0, sizeof(d->st)); /* 含 st.owner —— of() 随即失效 */
    d->dev.ops = NULL;
    d->dev.priv = NULL;
    spin_unlock_irqrestore(&d->lock, flags);
}
