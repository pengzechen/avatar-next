#ifndef KERNEL_TASK_CPU_H
#define KERNEL_TASK_CPU_H

/*
 * kernel/task/cpu.h — Per-CPU 控制块（SMP 抽象层）
 *
 * Phase 0：仅提供数据结构 + BSP 初始化骨架，行为等价于单核。
 *   - g_num_cpus 始终为 1，cpu_bring_up_all() 为 stub
 *   - cpu_current() 返回 &g_cpus[0]（BSP 安装后从 per-CPU 寄存器读取）
 *
 * 后续阶段：
 *   Phase 1: 调度器迁移到 cpu_t::run_queue / current_task / need_resched
 *   Phase 2: aarch64 PSCI 起多核 → cpu_secondary_bootstrap()
 *   Phase 4: IPI / 跨核唤醒
 */

#include "types.h"
#include "list.h"
#include "spinlock.h"
#include "arch.h"

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

/* 前向声明，避免循环 include（task.h 也会 include cpu.h） */
struct task;

/* ── Per-CPU 控制块 ──────────────────────────────────────────
 * Phase 0：仅占位字段；调度器迁移在 Phase 1 完成。
 *
 * 注意：当 Phase 1 接管 run_queue / current_task / need_resched 后，
 * 任何跨 CPU 访问这些字段都必须持有 rq_lock。
 */
typedef struct cpu {
    uint32_t            cpu_id;        /* 逻辑 CPU 编号（0..g_num_cpus-1） */
    uint64_t            hw_id;         /* MPIDR / hartid / APIC ID         */
    volatile bool       online;        /* 是否已通过 bootstrap（BSP 自旋等待，必须 volatile） */

    /* ── Phase 1 将迁入的调度器状态（现在仅占位）────────── */
    struct task        *current_task;  /* 本核当前任务                     */
    struct task        *idle_task;     /* 本核 idle 任务                   */
    list_t              run_queue;     /* 本核就绪队列                     */
    volatile bool       need_resched;  /* 时钟中断置位                     */
    spinlock_noirq_t    rq_lock;       /* 保护 run_queue + current_task    */
    uint32_t            irq_depth;     /* 硬中断嵌套深度（RISC-V 阶段 2） */
    uint32_t            preempt_schedule_depth; /* 防止 preempt_enable 递归调度 */

    /* ── 诊断 / 测试 ─────────────────────────────────────── */
    volatile uint64_t   local_ticks;   /* 本核 timer ISR 累计次数（SMP 验证用） */

    /* x86_64 SYSCALL 路径暂存用户 RSP（per-CPU，通过 gs:offset 访问） */
    uint64_t            scratch_rsp;
} cpu_t;

/* ── 全局 CPU 池 ────────────────────────────────────────── */
extern cpu_t   g_cpus[AVATAR_MAX_CPUS];
extern uint32_t g_num_cpus;               /* 当前在线 CPU 数（Phase 0 = 1） */

/* ── 获取当前 CPU ────────────────────────────────────────── */
static inline cpu_t *
cpu_current(void)
{
    cpu_t *c = arch_cpu_self_get();
    /* 若 per-CPU 寄存器尚未安装（早期 boot 或测试代码），回退 CPU0。 */
    return c ? c : &g_cpus[0];
}

static inline uint32_t
get_current_cpu_id(void)
{
    return cpu_current()->cpu_id;
}

/* ── 初始化 ─────────────────────────────────────────────── */

/*
 * cpu_init_bsp - BSP 调用一次，初始化 g_cpus[0] 并安装 per-CPU 寄存器。
 *
 * 必须在 task_init() 之前调用。
 */
void cpu_init_bsp(void);

/*
 * cpu_bring_up_all - BSP 调用一次，启动其他 CPU。
 *
 * Phase 0：stub（直接返回）。
 * Phase 2 起：aarch64 通过 PSCI CPU_ON 拉起 CPU1..N-1。
 */
void cpu_bring_up_all(void);

/*
 * cpu_secondary_bootstrap - 次级核 C 入口（aarch64 Phase 2）。
 *
 * 由 boot/aarch64/boot.S:_secondary_start 在 MMU + VBAR 就绪后调用，
 * 入参 cpu_id 即 BSP 在 psci_cpu_on 中传入的 context_id。
 * riscv64 / x86_64 后续 Phase 复用同名 C 入口。
 */
void cpu_secondary_bootstrap(uint32_t cpu_id);

/*
 * cpu_smp_timer_test - SMP 健康检查：验证所有在线 CPU 的 timer ISR 都在动。
 *
 * 必须在 timer_set_tick_cb 之后调用。BSP 忙等 rounds*ms_per_round 毫秒，
 * 每轮打印各核 local_ticks 增量。
 */
void cpu_smp_timer_test(uint32_t rounds, uint32_t ms_per_round);

#endif /* KERNEL_TASK_CPU_H */
