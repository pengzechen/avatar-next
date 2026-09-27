/*
 * include/vmm/vmm_virq.h — guest 中断「线」的架构无关命名
 *
 * 只统一到「设备拉哪条线」这一层。线的**号码空间**由架构决定，线的**投递
 * 语义**由各控制器决定，两者都不在这里统一。
 *
 * 从前是「一个 bool 查询 + 另一个头里的 #define 中断号 + 调用方负责配对」，
 * 三个架构各写一遍同一个模式：
 *
 *     device:    xxx_irq_asserted(vm)          ← 裸 bool
 *     caller:    if (asserted) yyy_set_pending(vm, 某个宏)
 *
 * 现在收成一句 `vmm_arch_irq_raise(vcpu, virq_line(VIRQ_XXX))`。
 *
 * ── 为什么没有 ops 表 / 没有控制器 vtable ────────────────────────────
 * ① 能真正统一的只有一个动作（「置 pending」）。其余全是异构的：
 *    vplic_next_deliverable 是 VMM 主动轮询推进；vlapic_take_pending +
 *    accept_interrupt 是两阶段、中间还夹一次**可能失败的注入**；
 *    vmm_vgic_ack/eoi 由 guest 写 MMIO 驱动。套表只会得到一张大部分 NULL
 *    的胖接口。
 * ② 同一个构建里**只有一个控制器**（aarch64 由 DRIVER_GIC_V3 编译期二选一，
 *    riscv/x86 各只有一个），运行期多态零收益。
 * ③ **sync_entry/sync_exit 是 vGIC 独有的**（PLIC/LAPIC 没有 LR 那种硬件影子）。
 *    把它们放进表里，等于对另外两个后端宣称「你们也有 entry/exit 对称生命周期」，
 *    将来有人在这张表上写调用点会得到**静默无操作**而不是编译错误 —— 这正是
 *    vgicv3.c 那条「已 ack 未 EOI 的中断必须重装回 LR」的不变量当初的成因。
 *
 * ── 所以「统一」体现在哪 ────────────────────────────────────────────
 * 只在分层与命名：设备 → 线 的 `vmm_arch_irq_raise()` 是新的；线 → 投递的
 * 三个控制器后端原样保留（vmm_vgic{,3}_set_pending / vplic_set_pending /
 * vlapic_raise_irq）。那四个函数是同一个语义动作的四个后端实现。
 *
 * ── 明确**不**放进 virq_t 的东西（别加） ────────────────────────────
 *   - vcpu_id：它是**入口上下文**的属性，不是线的属性。PLIC 根本没有这个
 *     维度（它的 pending 是源级全局的，`vplic.c` 把同一位置进所有 context），
 *     GIC 要的是「当前正在进入的这个 vcpu」—— 由 vmm_arch_irq_raise 的
 *     `vcpu_t *` 参数给出，不是线的字段。
 *   - vector：x86 上向量由 guest 自己写进 IO-APIC 重定向表，VMM 无权决定。
 *   - 触发语义（level/edge）：三个控制台都是 level，「是否每次入口重拉」由
 *     架构入口钩子无条件执行，不是设备声明的属性。加进来就是个骗人的旋钮。
 */
#ifndef VMM_VIRQ_H
#define VMM_VIRQ_H

#include "types.h"
#include "arch.h"

/*
 * 一条设备中断线。
 *
 * 号码的**含义由架构决定**：aarch64 是 GIC INTID、riscv 是 PLIC source、
 * x86 是 IO-APIC GSI（**不是** vLAPIC 的 vector —— 两者在 x86 上是不同的数，
 * 见下面 VIRQ_CONSOLE 的注释）。
 *
 * 用 struct 而不是 `typedef uint32_t virq_t`：x86 上「GSI」和「向量」极易写混，
 * 而现网就埋着一个实例 —— vmx.c 的控制台路径一直硬编码 GSI 4，而
 * `VMM_CONSOLE_IRQ` 在 x86 上展开成 UART16550_IRQ(10)。distinct struct 能让
 * 「把向量当线传进去」在编译期就失败。
 */
typedef struct virq {
    uint32_t line;
} virq_t;

static inline virq_t virq_line(uint32_t line)
{
    virq_t v;
    v.line = line;      /* 用函数而不是复合字面量：freestanding 下更稳 */
    return v;
}

/*
 * guest 可见的中断号。
 *
 * ⚠️ **同一个名字在三个架构里属于不同的号空间** —— 别把这几行合并成一个值。
 *    aarch64 33 是 GIC INTID（PL011 = SPI 1），riscv 10 是 PLIC source，
 *    x86 4 是 IO-APIC GSI（COM1）。x86 的**向量**由 guest 写进 IO-APIC 重定向
 *    表，VMM 不决定。
 */
#if ARCH_AARCH64
#define VIRQ_CONSOLE   33u   /* vPL011 → SPI 1 → INTID 33 */
#define VIRQ_VTIMER    27u   /* guest 虚拟定时器 → PPI 27  */
#elif ARCH_RISCV64
#define VIRQ_CONSOLE   10u   /* vuart16550 → PLIC source 10 */
#elif ARCH_X86_64
#define VIRQ_CONSOLE    4u   /* COM1 → IO-APIC GSI 4 */
#endif

#endif /* VMM_VIRQ_H */
