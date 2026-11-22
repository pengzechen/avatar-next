/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/cpu.c — Per-CPU 控制块 + 通用 CPU 管理

   跨架构共用部分：
     - g_cpus / g_num_cpus
     - 跨核 resched IPI(注册、发送)
     - BSP 初始化
     - 次级核 idle 任务的构造
     - SMP timer 健康自检的通用流程

   架构差异已经收敛到：
     - kernel/task/<arch>/cpu_startup.c — 次级核拉起、BSP hw_id、自检诊断
     - include/<arch>/cpu_impl.h       — per-CPU 寄存器、idle 等待指令

   三架构的次级核拉起都收敛到 cpu_secondary_bootstrap()：
     - aarch64: PSCI CPU_ON
     - riscv64: SBI HSM hart_start(不支持的平台回退到 release 表 + IPI)
     - x86_64 : INIT-SIPI + AP trampoline
*/

#include "task/cpu.h"
#include "task/task.h"
#include "task/sched.h"
#include "assert.h"
#include "klog.h"
#include "string.h"
#include "arch.h"
#include "barrier.h"
#include "driver/time.h"
#include "driver/irq.h"

/* ── 全局 CPU 池 ────────────────────────────────────────── */
cpu_t g_cpus[AVATAR_MAX_CPUS];
uint32_t g_num_cpus = 1; /* 已在线 CPU 数(BSP=1, 每成功拉起一个 AP +1) */

/* ── 跨核 resched IPI ───────────────────────────────────── */

/*
   resched_ipi_handler — 收到 IPI 只做一件事：置本核 need_resched

   真正的切换发生在中断返回路径(sched_check_and_yield_from_trap)或
   vCPU 主循环顶部(sched_check_and_yield)。在 ISR 里直接切任务是禁止的,
   见 docs/INTERRUPT_CONTEXT_SWITCH.md。

   RISC-V 的 SSIP 是电平型 pending 位、x86 的 LAPIC 需要 EOI，两者都由
   控制器后端的 dispatch 收尾(PLIC 清 SSIP / LAPIC 写 EOI)，这里只置位。
*/
static void resched_ipi_entry(void *frame, void *ctx)
{
    cpu_t *c = cpu_current();

    (void)frame;
    (void)ctx;
    c->need_resched = true;
}

void cpu_ipi_init(void)
{
    /* IPI 号/向量由控制器决定，内核不再关心 SGI 15 / scause 1 / 向量 0xF0 */
    if (irq_register_ipi(resched_ipi_entry, NULL) != 0)
        KLOG_WARN("[cpu] resched IPI registration failed\n");
}

void cpu_send_ipi(uint32_t cpu_id)
{
    if (cpu_id >= g_num_cpus || cpu_id >= AVATAR_MAX_CPUS)
        return;
    if (!g_cpus[cpu_id].online)
        return;

    /* hw_id: aarch64=MPIDR, riscv=hartid, x86=LAPIC ID */
    irq_send_ipi((uint32_t)g_cpus[cpu_id].hw_id);
}

/*
   klog_cpu_id - 给 klog.h 宏使用, 返回当前逻辑 CPU 编号。

   早期 boot(BSP 还没装 per-CPU 寄存器)arch_cpu_self_get 返回 0,
   cpu_current() 退化到 &g_cpus[0], cpu_id=0(BSS 默认值), 正确。
*/
uint32_t klog_cpu_id(void)
{
    return cpu_current()->cpu_id;
}

/*
   cpu_bump_local_ticks - timer ISR 每次调用, 让本核累计 tick 计数器 +1。
   用于 SMP 健康检查：cpu_smp_timer_test 会读所有核的计数器, 确认都在动。
*/
void cpu_bump_local_ticks(void)
{
    cpu_current()->local_ticks++;
}

/* ── BSP 初始化 ─────────────────────────────────────────── */
void cpu_init_bsp(void)
{
    cpu_t *c = &g_cpus[0];
    memset(c, 0, sizeof(*c));

    c->cpu_id = 0;
    /*
       硬件 ID 的读法按架构不同：aarch64=MPIDR, riscv=boot hartid,
       x86=LAPIC ID。差异收在 arch_cpu_bsp_hw_id()。
    */
    c->hw_id = arch_cpu_bsp_hw_id();
    c->current_task = NULL;
    c->idle_task = NULL;
    c->need_resched = false;
    sched_rq_init(c);
    spinlock_irq_init(&c->rq_lock);

    /* 安装 per-CPU 寄存器(TPIDR_EL1 / tp / GS_BASE) */
    arch_cpu_self_set(c);

    g_num_cpus = 1;

    /* 本核的 resched IPI 就绪；必须在 online 置位(任何人都可能开始 kick)之前完成。 */
    cpu_ipi_init();

    wmb();
    c->online = true;

    KLOG_SMP("[cpu] BSP initialized: cpu_id=0 hw_id=0x%llx\n", (unsigned long long)c->hw_id);
}

/* ── 次级核 idle 任务 ───────────────────────────────────── */

/* 每个次级核的 idle 任务控制块(CPU0 用 task.c 里的 g_idle_task)。 */
static task_t g_secondary_idle[AVATAR_MAX_CPUS - 1];

/*
   cpu_setup_secondary_idle_task - 初始化次级核 idle 任务控制块

   复用 secondary_boot_stacks[cpu_id] 作为 idle 的内核栈(次级核此刻
   的 SP 就指向那块 SECONDARY_STACK_SIZE 大小(16KB)的栈；切换到自己时
   只是恢复同一区域)。

   必须在第一次允许中断 / 第一次调用 sched_schedule 之前完成,
   否则 sched_check_and_yield 会读到 NULL current_task 而崩溃。
*/
void cpu_setup_secondary_idle_task(cpu_t *c)
{
    task_t *idle = &g_secondary_idle[c->cpu_id - 1];
    memset(idle, 0, sizeof(*idle));

    idle->state = TASK_RUNNING;
    idle->on_cpu = true;                /* 次级核此刻就在 idle 上执行 */
    idle->id = 0xFFFF0000U | c->cpu_id; /* 用高位段避免与 g_task_id_cnt 冲突 */
    idle->priority = 255;
    idle->stack_base = &secondary_boot_stacks[c->cpu_id * SECONDARY_STACK_SIZE];
    idle->entry = NULL;
    idle->arg = NULL;
    idle->cpu_affinity = c->cpu_id;
    idle->fs_base = 0;
    /*
       idle 也需要 files/fs/mm。files_alloc() 会把 fd 表显式填成 -1 ——
       不能靠 memset 归零：0 是合法的 fd 池槽位号，fd_table_inherit()
       会把它当成「256 个都指向槽位 0 的 fd」，一次继承就把
       FD_POOL_SIZE(128) 个槽全部吃光（实测过：之后宿主里所有 open()
       都失败）。CPU0 的 idle 走 task.c 的 g_idle_task，同样在
       task_init() 里分配。
    */
    assert_always(task_objects_alloc(idle));
    list_node_init(&idle->run_node);
    list_node_init(&idle->wait_node);

    /* 名字："idle/N" */
    idle->name[0] = 'i';
    idle->name[1] = 'd';
    idle->name[2] = 'l';
    idle->name[3] = 'e';
    idle->name[4] = '/';
    idle->name[5] = '0' + (char)(c->cpu_id % 10);
    idle->name[6] = '\0';

    c->idle_task = idle;
    c->current_task = idle;
}

/* ── SMP 自检：验证所有在线核的 timer ISR 都在动 ──────────

   流程：BSP 用 timer_delay_ms 忙等若干轮(不依赖调度器), 每轮快照
   g_cpus[i].local_ticks, 对比上一轮 → 全核都增长则 PASS, 否则 FAIL。

   只依赖次级核的本地 timer 已经启用(timer_init_secondary 完成并 online),
   不依赖 sched_tick 回调；主流程在 timer_set_tick_cb 之后调用。

   全核必须共享一致的时间基准；aarch64 是 CNTPCT_EL0, per-core 但同源
   振荡器, timer_delay_ms 在任何核上读数一致。

   架构专有的"开中断时机"和诊断打印由 arch_cpu_smp_test_*() 提供。
*/
extern volatile uint64_t g_system_ticks; /* driver/time/timer.c */

void cpu_smp_timer_test(uint32_t rounds, uint32_t ms_per_round)
{
    /* 即使单核也跑诊断：先确认 BSP 自己的 timer 是否在 tick。 */
    arch_cpu_smp_test_begin();
    arch_cpu_smp_test_diag_begin();

    KLOG_SMP("[smp-test] timer health: %u cpu(s), %u rounds x %u ms\n", (unsigned)g_num_cpus, (unsigned)rounds,
             (unsigned)ms_per_round);

    uint64_t prev[AVATAR_MAX_CPUS];
    for (uint32_t i = 0; i < g_num_cpus; i++) {
        prev[i] = g_cpus[i].local_ticks;
    }
    uint64_t prev_sys = g_system_ticks;

    bool all_ok = true;

    for (uint32_t r = 0; r < rounds; r++) {
        timer_delay_ms(ms_per_round);
        arch_cpu_smp_test_diag_round(r);

        uint64_t sys_now = g_system_ticks;
        KLOG_SMP("[smp-test] round=%u system_ticks=%llu (+%llu)\n", (unsigned)r, (unsigned long long)sys_now,
                 (unsigned long long)(sys_now - prev_sys));
        prev_sys = sys_now;

        for (uint32_t i = 0; i < g_num_cpus; i++) {
            uint64_t now = g_cpus[i].local_ticks;
            uint64_t diff = now - prev[i];
            KLOG_SMP("[smp-test] round=%u cpu%u local_ticks=%llu (+%llu)\n", (unsigned)r, (unsigned)i,
                     (unsigned long long)now, (unsigned long long)diff);
            if (diff == 0ULL) {
                all_ok = false;
            }
            prev[i] = now;
        }
    }

    if (all_ok) {
        KLOG_SMP("[smp-test] PASS: all %u cpu(s) timer ticking\n", (unsigned)g_num_cpus);
    } else {
        KLOG_ERROR("[smp-test] FAIL: some cpu(s) timer stuck\n");
    }
}
