/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef KERNEL_TASK_CPU_H
#define KERNEL_TASK_CPU_H

/*
   kernel/task/cpu.h — Per-CPU 控制块(SMP 抽象层)

   Phase 0：仅提供数据结构 + BSP 初始化骨架, 行为等价于单核。
     - g_num_cpus 始终为 1, cpu_bring_up_all() 为 stub
     - cpu_current() 返回 &g_cpus[0](BSP 安装后从 per-CPU 寄存器读取)

   后续阶段：
     Phase 1: 调度器迁移到 cpu_t::run_queue / current_task / need_resched
     Phase 2: aarch64 PSCI 起多核 → cpu_secondary_bootstrap()
     Phase 4: IPI / 跨核唤醒
*/

#include "types.h"
#include "list.h"
#include "spinlock.h"
#include "arch.h"
#include "assert.h" /* static_assert：校验下方 .S 可见的字段偏移 */

#if ARCH_AARCH64
#include "aarch64/cpu_impl.h"
#elif ARCH_RISCV64
#include "riscv64/cpu_impl.h"
#elif ARCH_X86_64
#include "x86_64/cpu_impl.h"
#else
#error "Unsupported architecture for cpu_impl.h"
#endif

#ifndef AVATAR_MAX_CPUS
#define AVATAR_MAX_CPUS 8U
#endif

#ifndef CONFIG_SMP_CPUS
#define CONFIG_SMP_CPUS 1U
#endif

/* 前向声明, 避免循环 include(task.h 也会 include cpu.h) */
struct task;

/* ── Per-CPU 控制块 ──────────────────────────────────────────
   Phase 0：仅占位字段；调度器迁移在 Phase 1 完成。

   注意：当 Phase 1 接管 run_queue / current_task / need_resched 后,
   任何跨 CPU 访问这些字段都必须持有 rq_lock。
*/
typedef struct cpu {
    uint32_t cpu_id;      /* 逻辑 CPU 编号(0..g_num_cpus-1) */
    uint64_t hw_id;       /* MPIDR / hartid / APIC ID */
    volatile bool online; /* 是否已通过 bootstrap(BSP 自旋等待, 必须 volatile) */

    /* ── Phase 1 将迁入的调度器状态(现在仅占位)────────── */
    struct task *current_task;       /* 本核当前任务 */
    struct task *idle_task;          /* 本核 idle 任务 */
    list_t run_queue;                /* 本核就绪队列 */
    volatile bool need_resched;      /* 时钟中断置位 */
    spinlock_noirq_t rq_lock;        /* 保护 run_queue + current_task */
    uint32_t irq_depth;              /* 硬中断嵌套深度(抢占边界判断) */
    uint32_t preempt_schedule_depth; /* 防止 preempt_enable 递归调度 */

    /* ── 诊断 / 测试 ─────────────────────────────────────── */
    volatile uint64_t local_ticks; /* 本核 timer ISR 累计次数(SMP 验证用) */

    /* x86_64 SYSCALL 路径暂存用户 RSP(per-CPU, 通过 gs:offset 访问) */
    uint64_t scratch_rsp;

    /* x86_64 SYSCALL 入口切换到本核内核栈用的栈顶, 即本核 TSS.RSP0 的镜像。
       SYSCALL 不像中断那样自动换栈, 入口必须显式从本核取值装入 RSP；
       以前读的是全局 g_x86_tss_rsp0(只由 CPU0 更新), SMP 下会把 AP 的
       syscall 帧建到别的 CPU 正在用的任务栈上, 导致栈互相破坏。
    */
    uint64_t kernel_rsp0;

    /*
       本核在 idle 任务上累计的 tick 数。/proc/uptime 的第二个字段要它。

       注意： 必须加在 scratch_rsp / kernel_rsp0 之后：那两个字段的偏移被
       boot/x86_64/syscall_wrapper.S 手抄成常量(CPU_SCRATCH_RSP=96 /
       CPU_KERNEL_RSP0=104), .S 里写不了 offsetof, 只能靠下面那组
       static_assert 把关。往它们前面插字段会当场把断言打炸(这是好事),
       但更省事的做法就是加在最后。
    */
    uint64_t idle_ticks;
} cpu_t;

/*
   boot/x86_64/syscall_wrapper.S 用 gs:offset 直接访问下面两个字段,
   .S 里无法写 offsetof, 只能手抄常量。这里加编译期断言, 字段布局一变
   立刻构建失败, 避免再次出现“偏移写错→写坏 rq_lock→SMP 下栈错乱”。
*/
#if ARCH_X86_64
static_assert(offsetof(cpu_t, scratch_rsp) == 96, "CPU_SCRATCH_RSP in boot/x86_64/syscall_wrapper.S is stale");
static_assert(offsetof(cpu_t, kernel_rsp0) == 104, "CPU_KERNEL_RSP0 in boot/x86_64/syscall_wrapper.S is stale");
#endif

/* ── 全局 CPU 池 ────────────────────────────────────────── */
extern cpu_t g_cpus[AVATAR_MAX_CPUS];
extern uint32_t g_num_cpus; /* 当前在线 CPU 数(Phase 0 = 1) */

/* ── 跨核 IPI ─────────────────────────────────────────────

   目前只有一种用途：resched(让目标核立刻重新调度)。三架构的「号空间」
   不同, 所以这里只定义各自的中断号, 发送/接收由 cpu.c 统一封装：

     AArch64: 物理 GIC SGI 15(GICv2/GICv3 都由 irq.h 的 irq_send_ipi 发)
     RISC-V : S 模式软件中断, scause = 1(SBI IPI 置 SSIP)
     x86_64 : LAPIC fixed-delivery IPI, 向量 0xF0(IDT 里已装门)

   注意： AArch64 的 GICv3 发送函数目前按 Aff0 寻址(QEMU virt 上 cpu_id ==
      Aff0)。多 cluster 平台(如 RK3588)要按 MPIDR 的 Aff1/Aff2/Aff3
      扩展 ICC_SGI1R_EL1；这条限制在 cpu_send_ipi 处有注释。
*/
#if ARCH_AARCH64
#define CPU_RESCHED_IPI 15u /* SGI 15：避开常用的 SGI 0-7 */
#elif ARCH_RISCV64
#define CPU_RESCHED_IPI_CAUSE 1u /* supervisor software interrupt */
#elif ARCH_X86_64
#define CPU_RESCHED_IPI_VECTOR 0xF0u
#endif

/*
   cpu_ipi_init - 在当前 CPU 上安装并打开 resched IPI

   每颗核在 IRQ 控制器就绪后各调一次(BSP 在 cpu_init_bsp, AP 在
   cpu_secondary_bootstrap)。必须在 cpu->online 置位之前完成 —— 发送方
   只对 online 的核发 IPI, 否则会往一个还没装好向量的核上打。
*/
void cpu_ipi_init(void);

/* cpu_send_ipi - 向指定逻辑 CPU 发一个 resched IPI */
void cpu_send_ipi(uint32_t cpu_id);

/* ── 获取当前 CPU ────────────────────────────────────────── */
static inline cpu_t *cpu_current(void)
{
    cpu_t *c = arch_cpu_self_get();
    /* 若 per-CPU 寄存器尚未安装(早期 boot 或测试代码), 回退 CPU0。 */
    return c ? c : &g_cpus[0];
}

static inline uint32_t get_current_cpu_id(void)
{
    return cpu_current()->cpu_id;
}

/* ── 初始化 ─────────────────────────────────────────────── */

/*
   cpu_init_bsp - BSP 调用一次, 初始化 g_cpus[0] 并安装 per-CPU 寄存器。

   必须在 task_init() 之前调用。
*/
void cpu_init_bsp(void);

/*
   cpu_bring_up_all - BSP 调用一次, 启动其他 CPU。

   Phase 0：stub(直接返回)。
   Phase 2 起：aarch64 通过 PSCI CPU_ON 拉起 CPU1..N-1。
*/
void cpu_bring_up_all(void);

/*
   cpu_secondary_bootstrap - 次级核 C 入口(aarch64 Phase 2)。

   由 boot/aarch64/boot.S:_secondary_start 在 MMU + VBAR 就绪后调用,
   入参 cpu_id 即 BSP 在 psci_cpu_on 中传入的 context_id。
   riscv64 / x86_64 后续 Phase 复用同名 C 入口。
*/
void cpu_secondary_bootstrap(uint32_t cpu_id);

/*
   cpu_smp_timer_test - SMP 健康检查：验证所有在线 CPU 的 timer ISR 都在动。

   必须在 timer_set_tick_cb 之后调用。BSP 忙等 rounds*ms_per_round 毫秒,
   每轮打印各核 local_ticks 增量。
*/
void cpu_smp_timer_test(uint32_t rounds, uint32_t ms_per_round);

#endif /* KERNEL_TASK_CPU_H */
