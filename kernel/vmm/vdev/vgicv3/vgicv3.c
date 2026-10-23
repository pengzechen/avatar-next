/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/vmm/vdev/vgicv3/vgicv3.c — VM 级虚拟 GICv3 核心

   职责划分(对应 GICv2 版的 vgic.c + vgicc.c 的 LR 部分)：
     - 维护 guest 中断的 pending / enabled / active 软件状态
     - 把这些状态通过 ICH_LR<n>_EL2 推给硬件(guest 侧由 ICV_* 直接消费)
     - 退出 guest 后从 LR.State / ICH_ELRSR_EL2 回推软件状态

   GICv2 版是「VMM 替代 guest 应答」(GICC_IAR 被陷入后由软件返回 IRQ 号)；
   GICv3 的 CPU interface 是系统寄存器, VMM 无法陷入, 只能做上面这套
   「写入 LR + 回读状态」的硬件辅助模型。
*/

#include "vmm/vmm.h" /* vcpu_t：LR 归属切换要拿上一个 vCPU 的 irq_lock */
#include "vmm/vmm_vgicv3.h"
#include "irq/gicv3.h"
#include "klog.h"
#include "string.h"
#include "barrier.h" /* barrier_sync：清 LR 后要等它落地 */
#include "aarch64/sysreg.h"
#include "task/cpu.h" /* get_current_cpu_id：诊断「En 设在哪一核」 */

#define SGI_MASK 0xffffu

static int valid_vcpu(const vgic3_t *vgic, uint32_t vcpu_id)
{
    return vgic && vcpu_id < vgic->nr_vcpus;
}

static int valid_irq(uint32_t irq)
{
    return irq < VGIC3_MAX_IRQS;
}

static uint32_t irq_bit(uint32_t irq)
{
    return 1u << (irq & 31u);
}

/* ── LR 访问 ─────────────────────────────────────────────────── */

static uint32_t lr_vintid(uint64_t lr)
{
    return (uint32_t)(lr & ICH_LR_VINTID_MASK);
}

/* 返回 State 字段的原始取值(0..3), 不是掩码 */
static uint32_t lr_state(uint64_t lr)
{
    return (uint32_t)((lr >> ICH_LR_STATE_SHIFT) & 3u);
}

/*
   vgic3_lr_value — 组一个「纯软件注入」的 Group1 中断 LR

   State=Pending, HW=0(没有对应的物理中断需要硬件联动),
   优先级取 guest 自己给该 INTID 配的值 —— guest 的 ICH_VMCR_EL2.VPMR
   也是它自己写的, 两者同源才能保证「配置合法就该投递」。
*/
static uint64_t vgic3_lr_value(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    uint8_t prio = VGIC3_DEFAULT_PRIO;

    if (irq < 32)
        prio = vgic->vcpu[vcpu_id].prio0[irq];
    else if (irq < VGIC3D_REG_SIZE - 0x400)
        prio = vgic->dist_regs[0x400 + irq];

    return ((uint64_t)ICH_LR_ST_PENDING << ICH_LR_STATE_SHIFT) | ICH_LR_GROUP1 | ((uint64_t)prio << ICH_LR_PRIO_SHIFT) |
           ((uint64_t)irq & ICH_LR_VINTID_MASK);
}

static int lr_has_irq(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    const vgic3_vcpu_t *vcpu = &vgic->vcpu[vcpu_id];

    for (uint32_t i = 0; i < _gicv3.nr_lrs && i < VGIC3_MAX_LRS; i++)
        if (vcpu->lr[i] && lr_vintid(vcpu->lr[i]) == irq)
            return 1;
    return 0;
}

static int lr_empty_slot(const vgic3_t *vgic, uint32_t vcpu_id)
{
    const vgic3_vcpu_t *vcpu = &vgic->vcpu[vcpu_id];
    uint32_t n = _gicv3.nr_lrs;

    if (n > VGIC3_MAX_LRS)
        n = VGIC3_MAX_LRS;

    for (uint32_t i = 0; i < n; i++)
        if (vcpu->lr[i] == 0)
            return (int)i;
    return -1;
}

/* ── 初始化 ─────────────────────────────────────────────────── */

int vmm_vgic3_init(vgic3_t *vgic, uint32_t nr_vcpus)
{
    if (!vgic)
        return -1;
    if (nr_vcpus < 1)
        nr_vcpus = 1;
    if (nr_vcpus > VGIC3_MAX_VCPUS)
        nr_vcpus = VGIC3_MAX_VCPUS;

    memset(vgic, 0, sizeof(*vgic));
    vgic->nr_vcpus = nr_vcpus;
    for (uint32_t i = 0; i < VGIC3_MAX_VCPUS; i++) {
        /* SGIs 永远使能(GICv3 下 SGI 的使能位同样是 banked 的, 但 Linux
           从不单独使能 SGI, GICD_TYPER 也不报告它们)；PPI 初始为禁止。
        */
        vgic->vcpu[i].enabled0 = SGI_MASK;
        memset(vgic->vcpu[i].prio0, VGIC3_DEFAULT_PRIO, 32);
        vgic->vcpu[i].sgi_igroupr0 = 0xFFFFFFFFu;
        vgic->vcpu[i].rd_waker = GICR_WAKER_PROCESSOR_SLEEP;

        /*
           ICH_VMCR_EL2 的初值(每个 vCPU 一份)。

           注意： 不能全 0：VPMR(bit[31:24], guest 的 ICC_PMR_EL1)在 ARM 里
           是优先级屏蔽, 值 0 表示"屏蔽一切优先级"—— 全 0 的初值会让
           guest 在写下自己的 PMR 之前一个中断都收不到, 于是 vm1 直接卡在
           启动早期(实测：改成全 0 后连单 VM 都起不来)。

           全放行 = VPMR=0xFF(最低优先级), VENG1=1(Group1 使能)。
        */
        vgic->vcpu[i].ich_vmcr = (0xFFULL << 24) | (1ULL << 1);
        vgic->vcpu[i].ich_ap1r0 = 0;
    }

    KLOG_INFO("[vgicv3] VM vGIC ready (%u vCPU, %u IRQs, %u LR)\n", nr_vcpus, (unsigned)VGIC3_MAX_IRQS,
              (unsigned)_gicv3.nr_lrs);
    return 0;
}

/*
   vmm_vgic3_hw_init — 打开本 CPU 的 ICH_HCR_EL2.En

   不开这个位, 硬件在任何 ICC_IAR1_EL1 读上都只会返回 spurious, guest 永远
   收不到虚拟中断。

   注意： ICH_HCR_EL2 是每 CPU 的系统寄存器, 必须在真正跑 vCPU 的那个核上
   设置 —— 它不是「VM 创建时设一次」的东西。两者并不总是同一个核：
     - vcpu_task_create() 把 vCPU 任务钉在 CPU0(`task_set_cpu_affinity(t, 0)`)；
     - 而 vm_create() 跑在调用者的核上。从 /dev/vmm 启动时, 那是用户态 helper
       的 write 系统调用所在的核, SMP>1 时完全可能是 CPU1。

   实测(GIC=v3 + SMP=2, QEMU)：En=1 落在 CPU1, guest 跑在 CPU0 → guest 永远
   收不到虚拟中断, 卡在启动中途。症状很有迷惑性：guest 照常启动到设备探测完,
   然后停住；宿主侧看到的是 vtimer 注入计数一路涨, 而 guest 的 CNTV_CVAL 再也
   不变(它压根没收到中断, 自然没重编程)。SMP=1 时只有一个核, 所以从没暴露。

   因此本函数在每次进入 guest 前调用(见 el2_run.c 的
   vmm_arch_restore_guest_ctx)。对同一个核重复写是幂等的, 代价只有几条指令。
*/
void vmm_vgic3_hw_init(void)
{
    uint64_t hcr = gicv3_read_hcr();

    /* En=1；维护中断相关位保持 0 —— 我们在每次 VM exit 主动轮询
       ICH_ELRSR_EL2/ICH_EISR_EL2, 不需要 guest EOI 时打断自己。

       TDIR=1：guest 的 ICC_DIR_EL1/ICV_DIR_EL1(split EOI 模式的
       deactivate)必须陷入。active 中断在 vCPU 切换时会被移出 LR, 硬件
       DIR 找不到 LR 就会把这次 deactivation 丢掉, guest 无法恢复。
       由 vmm_vgic3_deactivate() 在软件里收口。
    */
    hcr |= ICH_HCR_EN | ICH_HCR_TDIR;
    hcr &= ~(uint64_t)ICH_HCR_UIEN;
    gicv3_write_hcr(hcr);

    /*
       只打第一次。带上 cpu 号：本函数的调用核就是 vCPU 的运行核, 上面那个
       SMP>1 的 bug 里, 「En 设在哪一核」是唯一的线索。
    */
    static int logged;
    if (!logged) {
        logged = 1;
        KLOG_INFO("[vgicv3] ICH_HCR_EL2=0x%llx (En=1, %u LRs), set on cpu=%u\n", (unsigned long long)gicv3_read_hcr(),
                  (unsigned)_gicv3.nr_lrs, (unsigned)get_current_cpu_id());
    }
}

/* ── 状态写入接口 ───────────────────────────────────────────── */

void vmm_vgic3_set_dist_enabled(vgic3_t *vgic, int enabled)
{
    if (vgic)
        vgic->dist_enabled = enabled ? 1 : 0;
}

int vmm_vgic3_dist_enabled(const vgic3_t *vgic)
{
    return vgic ? vgic->dist_enabled : 0;
}

void vmm_vgic3_set_sgi_pending(vgic3_t *vgic, uint32_t vcpu_id, uint32_t source_vcpu, uint32_t irq)
{
    if (!valid_vcpu(vgic, vcpu_id) || irq >= 16)
        return;
    if (source_vcpu >= VGIC3_MAX_VCPUS)
        source_vcpu = 0;

    vgic->vcpu[vcpu_id].sgi_sources[irq] |= (uint16_t)(1u << source_vcpu);
    vgic->vcpu[vcpu_id].pending0 |= irq_bit(irq);
}

static void clear_active(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq);

void vmm_vgic3_deactivate(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (!vgic || !valid_irq(irq) || !valid_vcpu(vgic, vcpu_id))
        return;

    vgic3_vcpu_t *vcpu = &vgic->vcpu[vcpu_id];
    uint32_t n = _gicv3.nr_lrs > VGIC3_MAX_LRS ? VGIC3_MAX_LRS : _gicv3.nr_lrs;

    /*
       DIR 的语义：Active -> Invalid；Active+Pending -> Pending。
       LR 里没有对应 INTID 时(active 状态此前被移到软件位), 硬件什么
       也不做(只增加 EOICount), 所以下面无论如何都要清软件 active。
    */
    for (uint32_t i = 0; i < n; i++) {
        uint64_t lr = vcpu->lr[i];

        if (!lr || lr_vintid(lr) != irq)
            continue;

        uint32_t st = lr_state(lr);
        if (!(st & ICH_LR_ST_ACTIVE))
            continue; /* Pending 上的 DIR 是 UNPREDICTABLE, 忽略 */

        if (st & ICH_LR_ST_PENDING) {
            lr = (lr & ~ICH_LR_STATE_MASK) | ((uint64_t)ICH_LR_ST_PENDING << ICH_LR_STATE_SHIFT);
            vcpu->lr[i] = lr;
            gicv3_write_lr(i, lr);
            /* 软硬件都记着：DIR 后它仍然 pending。 */
            if (irq < 32)
                vcpu->pending0 |= irq_bit(irq);
            else
                vgic->spi_pending[vcpu_id][irq / 32] |= irq_bit(irq);
        } else {
            vcpu->lr[i] = 0;
            gicv3_write_lr(i, 0);
        }
        clear_active(vgic, vcpu_id, irq);
        return;
    }

    /*
       LR 里没有：这正是 split EOI 模式下 vCPU 切换造成的情况 ——
       active 中断已经被移到 active0/spi_active。清掉软件 active 位。
       如果同一个 INTID 在 active 期间又来了新边沿, pending 位保持不动,
       下一次 sync_entry 会重新注入。
    */
    clear_active(vgic, vcpu_id, irq);
}

void vmm_vgic3_set_pending(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (!vgic || !valid_irq(irq) || !valid_vcpu(vgic, vcpu_id))
        return;

    if (irq < 16) {
        vmm_vgic3_set_sgi_pending(vgic, vcpu_id, 0, irq);
    } else if (irq < 32) {
        vgic->vcpu[vcpu_id].pending0 |= irq_bit(irq);
    } else {
        vgic->spi_pending[vcpu_id][irq / 32] |= irq_bit(irq);
    }
}

void vmm_vgic3_set_enabled(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq, int enabled)
{
    if (!vgic || !valid_irq(irq) || irq < 16)
        return; /* SGI 使能位不参与投递判定, 忽略 */

    if (irq < 32) {
        if (!valid_vcpu(vgic, vcpu_id))
            return;
        if (enabled)
            vgic->vcpu[vcpu_id].enabled0 |= irq_bit(irq);
        else
            vgic->vcpu[vcpu_id].enabled0 &= ~irq_bit(irq);
    } else if (enabled) {
        vgic->enabled[irq / 32] |= irq_bit(irq);
    } else {
        vgic->enabled[irq / 32] &= ~irq_bit(irq);
    }
}

uint32_t vmm_vgic3_enabled_word(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t word)
{
    if (!vgic || word >= VGIC3_MAX_WORDS)
        return 0;
    if (word == 0)
        return valid_vcpu(vgic, vcpu_id) ? (vgic->vcpu[vcpu_id].enabled0 | SGI_MASK) : SGI_MASK;
    return vgic->enabled[word];
}

uint32_t vmm_vgic3_pending_word(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t word)
{
    if (!vgic || word >= VGIC3_MAX_WORDS || !valid_vcpu(vgic, vcpu_id))
        return 0;
    if (word == 0)
        return vgic->vcpu[vcpu_id].pending0;
    return vgic->spi_pending[vcpu_id][word];
}

uint32_t vmm_vgic3_active_word(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t word)
{
    if (!vgic || word >= VGIC3_MAX_WORDS || !valid_vcpu(vgic, vcpu_id))
        return 0;
    if (word == 0)
        return vgic->vcpu[vcpu_id].active0;
    return vgic->spi_active[vcpu_id][word];
}

void vmm_vgic3_clear_pending_word(vgic3_t *vgic, uint32_t vcpu_id, uint32_t word, uint32_t bits)
{
    if (!vgic || word >= VGIC3_MAX_WORDS || !valid_vcpu(vgic, vcpu_id))
        return;
    if (word == 0)
        vgic->vcpu[vcpu_id].pending0 &= ~bits;
    else
        vgic->spi_pending[vcpu_id][word] &= ~bits;
}

void vmm_vgic3_clear_active_word(vgic3_t *vgic, uint32_t vcpu_id, uint32_t word, uint32_t bits)
{
    if (!vgic || word >= VGIC3_MAX_WORDS || !valid_vcpu(vgic, vcpu_id))
        return;
    if (word == 0)
        vgic->vcpu[vcpu_id].active0 &= ~bits;
    else
        vgic->spi_active[vcpu_id][word] &= ~bits;
}

/* ── 软件 active / pending 置位(内部)──────────────────────── */

static void set_active(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (irq < 32)
        vgic->vcpu[vcpu_id].active0 |= irq_bit(irq);
    else
        vgic->spi_active[vcpu_id][irq / 32] |= irq_bit(irq);
}

static void clear_active(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (irq < 32)
        vgic->vcpu[vcpu_id].active0 &= ~irq_bit(irq);
    else
        vgic->spi_active[vcpu_id][irq / 32] &= ~irq_bit(irq);
}

static void clear_pending(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (irq < 16) {
        vgic->vcpu[vcpu_id].pending0 &= ~irq_bit(irq);
        vgic->vcpu[vcpu_id].sgi_sources[irq] = 0;
    } else if (irq < 32) {
        vgic->vcpu[vcpu_id].pending0 &= ~irq_bit(irq);
    } else {
        vgic->spi_pending[vcpu_id][irq / 32] &= ~irq_bit(irq);
    }
}

/* ── per-CPU 的 ICH_*_EL2 访问(见 vgic3_vcpu_t 里 ich_vmcr 的注释)─────

   VMCR 和 AP1R0 都是 per-pCPU 硬件, 但内容属于当前在上面跑的 vCPU,
   所以按 vCPU 保存/恢复。实测只按 VM 存一份时, 同 VM 的 vCPU 轮转会把
   对方的 PMR/IGRPEN/active priority 覆盖掉：vtimer 的 LR 卡在 Active,
   guest idle 后再也醒不过来 —— vCPU 越多(2 分 8)越容易命中。

   注意： 只碰 AP1R0。ICH_AP1R1/2/3_EL2 在 QEMU 上实测会触发 "Unknown reason"
   (EC=0x0), 连单 VM 都起不来；当前优先级位数下 AP1R0 已覆盖。
*/

static uint64_t ich_read_vmcr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, " ICH_VMCR_EL2 : "=r"(v));
    return v;
}

static void ich_write_vmcr(uint64_t v)
{
    __asm__ volatile("msr " ICH_VMCR_EL2 ", %0" ::"r"(v) : "memory");
}

static uint64_t ich_read_ap1r0(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, " ICH_AP1R0_EL2 : "=r"(v));
    return v;
}

static void ich_write_ap1r0(uint64_t v)
{
    __asm__ volatile("msr " ICH_AP1R0_EL2 ", %0" ::"r"(v) : "memory");
}

/* ── 进入 guest 前：把可投递中断写进空 LR ───────────────────── */

/*
   本 pCPU 的 ICH_LR<n>_EL2 当前"属于"哪个 VM 的哪个 vCPU。

   注意： LR 是 per-pCPU 的硬件, 而多个 VM 的 vCPU 任务可以在同一颗核上
   分时跑(都钉 CPU0 时必然如此)。没有这张表的话：VM2 的 sync_entry() 会
   看到 VM1 还留在 LR 里的 active 项 —— 要么把 VM1 的中断当成自己的
   (guest 收到不属于它的 IRQ), 要么因为槽位被占满而排不进自己的中断
   (guest 卡死等不到 tick)。两种症状都极其难查。

   这里用 per-pCPU 数组记录「哪颗核的 LR 当前归哪个 vgic/vCPU」；切换
   归属时必须先回收上一个的 LR 状态, 再清空, 再装当前 vCPU 的。表很小
   (最多 8 核), 查找是常数时间。
*/
#define VGIC3_MAX_LR_CPUS 8
static vcpu_t *g_lr_owner_vcpu[VGIC3_MAX_LR_CPUS];

/*
   vmm_vgic3_lr_switch_in — 进入 guest 前调用：确保本核的 LR 属于给定的
   vCPU, 然后把可投递的中断排进去。

   归属变化时的顺序不能反：
     ① 先 sync_exit(上一个) —— 把硬件里仍 active/pending 的项回收进上一个
        VM 自己的软件镜像(它的 guest 可能已经 ack 但还没 EOI, 状态不能丢)；
     ② 再清空全部 LR —— 让新 VM 从一个干净的硬件状态开始；
     ③ 最后 sync_entry(当前) 排入当前 VM 的中断。

   反过来(先清再 sync_exit)会把上一个 VM 尚未 EOI 的中断状态直接抹掉。
*/
void vmm_vgic3_lr_switch_in(vgic3_t *vgic, vcpu_t *vcpu)
{
    uint32_t cpu = get_current_cpu_id();
    uint32_t vcpu_id = (uint32_t)vcpu->vcpu_id;

    if (cpu < VGIC3_MAX_LR_CPUS && g_lr_owner_vcpu[cpu] != vcpu) {
        vcpu_t *prev = g_lr_owner_vcpu[cpu];

        if (prev) {
            vgic3_t *prev_vgic = &prev->vm->vgic3;
            vgic3_vcpu_t *prev_state = &prev_vgic->vcpu[(uint32_t)prev->vcpu_id];
            uint32_t n = _gicv3.nr_lrs > VGIC3_MAX_LRS ? VGIC3_MAX_LRS : _gicv3.nr_lrs;

            /*
               持上一个 vCPU 的 irq_lock。

               它的 exit 路径已经 sync 过一次, 但这里还要再同步：远端 CPU
               可能正对着它调用 vmm_vcpu_inject_irq(SGI/控制台/NPU), 而
               sync_exit 会对 active 的 LR 执行 clear_pending。不加锁的话,
               刚置上的 pending 会被这次"补同步"抹掉 —— 表现是 guest 丢了
               一次 IPI, SMP 启动或 RCU 握手随机卡死。
            */
            spin_lock(&prev->irq_lock);
            vmm_vgic3_sync_exit(prev_vgic, (uint32_t)prev->vcpu_id);

            /*
               把本 CPU 的 ICH_*_EL2 硬件状态存回上一个 vCPU 自己,
               稍后它被调度回来时再装回去(见下面的 restore)。
               顺序：先 sync_exit(回收 LR 语义)→ 再存 ICH → 再清 LR。
            */
            prev_state->ich_vmcr = ich_read_vmcr();
            prev_state->ich_ap1r0 = ich_read_ap1r0();

            /*
               接下来要清空本核硬件 LR。对仍在 LR 里, 还没被 guest ack
               的边沿 SGI, 镜像里只有它, 软件 pending 已经在注入时消费掉了；
               把 pending 位放回软件状态, 切回来才能重新投递。

               必须放在这里(确定要切走时), 不能放在 sync_exit：同一个
               vCPU 正常退出再进入时不会清 LR, 那时重放 pending 会让同一个
               SGI 在 EOI 后被重复投一次。
            */
            for (uint32_t i = 0; i < n; i++) {
                uint64_t lr = prev_state->lr[i];

                if (!lr)
                    continue;
                uint32_t irq = lr_vintid(lr);
                if (irq < 16 && (lr_state(lr) & ICH_LR_ST_PENDING) && !(lr_state(lr) & ICH_LR_ST_ACTIVE))
                    prev_state->pending0 |= irq_bit(irq);
            }

            for (uint32_t i = 0; i < n; i++)
                gicv3_write_lr(i, 0);
            barrier_sync();
            spin_unlock(&prev->irq_lock);
        }

        /* 装入当前 vCPU 的 ICH 状态(首次进入时是 init 给的默认值) */
        ich_write_vmcr(vgic->vcpu[vcpu_id].ich_vmcr);
        ich_write_ap1r0(vgic->vcpu[vcpu_id].ich_ap1r0);
        barrier_sync();

        g_lr_owner_vcpu[cpu] = vcpu;
    }

    vmm_vgic3_sync_entry(vgic, vcpu_id);
}

void vmm_vgic3_sync_entry(vgic3_t *vgic, uint32_t vcpu_id)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return;

    vgic3_vcpu_t *vcpu = &vgic->vcpu[vcpu_id];

    /*
       1) 先按 ICH_ELRSR_EL2 回收槽位。
          ELRSR[i]=1 表示硬件认为 LR i 已空(guest 已 EOI, 或从未被占用),
          此时清掉镜像, 后续才可能把新的中断排进去。
    */
    uint64_t elrsr = gicv3_read_elrsr();
    uint32_t n = _gicv3.nr_lrs > VGIC3_MAX_LRS ? VGIC3_MAX_LRS : _gicv3.nr_lrs;

    for (uint32_t i = 0; i < n; i++)
        if ((elrsr >> i) & 1)
            vcpu->lr[i] = 0;

    /*
       1b) 把软件记着 active, 但硬件 LR 里已经没有的中断重新装回 LR,
           状态只置 Active(不带 Pending)。

       为什么必须补这一步(实测第二个 VM 卡在 init 的根因)：
         多 VM 分时跑时, 切换 VM 必须清空硬件 LR 让给下一个 VM, 于是
         "guest 已 ack, 但还没 EOI"的中断只能记在软件 active 位里。等它被
         调度回来时, 若不把那个中断装回 LR, guest 随后写的
         ICC_EOIR1_EL1 落在一个空槽上 —— 硬件无事发生, ELRSR 不会置位,
         我们也收不到通知 ⇒ 软件 active 位永远没人清。
         而 sync_entry 的排队条件是 `pending & enabled & ~active`,
         于是这个 INTID 被永久堵死：宿主侧看 vtimer 注入计数一路涨,
         guest 的 CNTV_CVAL 却再也不变。

       装回去之后闭环就自洽了：guest EOI → 硬件清 LR, ELRSR 置位 →
       下次 sync_exit 的 "槽位已空" 分支 → clear_active。

       注意： 这一段正是「中断接口只统一到线, 不统一控制器」的理由(见
       include/vmm/vmm_virq.h §③)：sync_entry/sync_exit 是 vGIC 独有的
       硬件协助阶段, PLIC/vLAPIC 没有对应物。若把它们塞进一张通用 ops 表,
       缺的只是空实现, 将来有人在那张表上写调用点会得到静默无操作
       而不是编译错误 —— 这条不变量的成因就是「某个动作没做而没人报错」。
    */
    for (uint32_t word = 0; word < VGIC3_MAX_WORDS; word++) {
        uint32_t act = vmm_vgic3_active_word(vgic, vcpu_id, word);

        while (act) {
            uint32_t irq = word * 32 + (uint32_t)__builtin_ctz(act);
            int slot;
            uint64_t lr;

            act &= act - 1;

            if (lr_has_irq(vgic, vcpu_id, irq))
                continue; /* 已经在 LR 里了 */

            slot = lr_empty_slot(vgic, vcpu_id);
            if (slot < 0)
                break; /* 槽位不够, 留给下一轮 */

            lr = vgic3_lr_value(vgic, vcpu_id, irq);
            lr = (lr & ~ICH_LR_STATE_MASK) | ((uint64_t)ICH_LR_ST_ACTIVE << ICH_LR_STATE_SHIFT);
            vcpu->lr[slot] = lr;
            gicv3_write_lr((uint32_t)slot, lr);
        }
    }

    /*
       2) 扫全部 INTID, 把 pending & enabled & !active 且尚未排入 LR 的
          写进空槽位。
    */
    for (uint32_t word = 0; word < VGIC3_MAX_WORDS; word++) {
        uint32_t ready = vmm_vgic3_pending_word(vgic, vcpu_id, word) & vmm_vgic3_enabled_word(vgic, vcpu_id, word) &
                         ~vmm_vgic3_active_word(vgic, vcpu_id, word);

        while (ready) {
            uint32_t bit = ready & (~ready + 1u);
            uint32_t off = 0;
            uint32_t tmp = bit;

            while (tmp > 1u) {
                tmp >>= 1;
                off++;
            }
            ready &= ~bit;

            uint32_t irq = word * 32u + off;
            if (irq >= VGIC3_MAX_IRQS)
                break;
            if (lr_has_irq(vgic, vcpu_id, irq))
                continue;

            int slot = lr_empty_slot(vgic, vcpu_id);
            if (slot < 0)
                return; /* LR 用满 */

            uint64_t value = vgic3_lr_value(vgic, vcpu_id, irq);
            vcpu->lr[slot] = value;
            gicv3_write_lr(slot, value);

            /*
               SGI 是边沿触发：这个 pending 位在写进 LR 的这一刻就被
               「消费」了。必须现在清掉, 之后 guest 再发同一 INTID 的 SGI
               才能重新把 pending 置起来(否则两次 IPI 会被合并成一次)。

               PPI/SPI 不走这条：它们是电平语义, pending 要保留到 guest
               ack(sync_exit 里清), 设备侧每轮 entry 会重新拉线。
            */
            if (irq < 16) {
                vcpu->pending0 &= ~irq_bit(irq);
                vcpu->sgi_sources[irq] = 0;
            }
        }
    }
}

/* ── 退出 guest 后：从硬件回推软件状态 ─────────────────────── */

/*
   LR.State 由硬件维护：
     Pending          —— 已排入, guest 还没应答
     Active&Pending   —— guest 已读 IAR(ack), 还没写 EOIR
     (槽位消失)        —— guest 已写 EOIR(硬件把 LR 置 Invalid,
                         ELRSR 对应位置 1)

   所以：
     槽位仍在 && State 含 Active  → 这次 guest 应答了 → pending 转 active
     槽位消失 && 之前有镜像        → guest 完成了 EOI    → 清 active,
                                     等设备侧下次置 pending 再投
*/
void vmm_vgic3_sync_exit(vgic3_t *vgic, uint32_t vcpu_id)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return;

    vgic3_vcpu_t *vcpu = &vgic->vcpu[vcpu_id];
    uint64_t elrsr = gicv3_read_elrsr();
    uint32_t n = _gicv3.nr_lrs > VGIC3_MAX_LRS ? VGIC3_MAX_LRS : _gicv3.nr_lrs;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t prev = vcpu->lr[i];

        if (!prev)
            continue;

        if ((elrsr >> i) & 1) {
            /* 槽位已空：guest 走完了 ack+EOI(或硬件丢弃) */
            uint32_t irq = lr_vintid(prev);
            clear_active(vgic, vcpu_id, irq);
            vcpu->lr[i] = 0;
            continue;
        }

        /* 槽位仍被占用：回读硬件状态 */
        uint64_t hw = gicv3_read_lr(i);
        uint32_t irq = lr_vintid(prev);

        if (lr_state(hw) & ICH_LR_ST_ACTIVE) {
            /*
               guest 已应答。

               PPI/SPI(电平)：挂起位转 active 记帐, 避免重复投递。
               SGI(边沿)：不能清 pending —— 注入时已经清过一次,
               这里再清会把「active 期间新到的同号 IPI」抹掉, guest 就会
               丢一次跨核唤醒(SMP 启动/RCU 握手随机卡死的经典来源)。
            */
            if (irq >= 16)
                clear_pending(vgic, vcpu_id, irq);
            set_active(vgic, vcpu_id, irq);
        }
        vcpu->lr[i] = hw;
    }
}

/* ── 定时器注入(宿主侧设备 -> guest)──────────────────────── */

void vmm_vgic3_inject_timer(vgic3_t *vgic, uint32_t vcpu_id)
{
    vmm_vgic3_set_pending(vgic, vcpu_id, VIRQ_VTIMER);
}
