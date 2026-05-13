/*
 * kernel/task/cpu.c — Per-CPU 初始化和多核启动
 */

#include "task/cpu.h"
#include "task/task.h"
#include "klog.h"
#include "arch.h"
#include "string.h"
#include "list.h"
#include "mm_vm.h"

/* ── 全局 CPU 池 ────────────────────────────────────────– */

uint32_t g_num_cpus = 1;       /* 检测到的 CPU 核心数 */
cpu_t    g_cpus[AVATAR_MAX_CPUS]; /* Per-CPU 结构数组 */

/* ── Per-CPU idle 任务栈池 ─────────────────────────────── */
static uint8_t g_cpu_idle_stacks[AVATAR_MAX_CPUS][4096] __attribute__((aligned(16)));
static task_t  g_cpu_idle_tasks[AVATAR_MAX_CPUS];
static uint32_t g_task_id_counter = 100;  /* Idle 任务的 ID 计数器 */
static volatile uint32_t g_cpu_online_bitmap = 0;

#ifndef CONFIG_SMP_CPUS
#define CONFIG_SMP_CPUS 1U
#endif

static uint32_t
configured_cpu_count(void)
{
    uint32_t n = (uint32_t)CONFIG_SMP_CPUS;
    if (n == 0) {
        n = 1;
    }
    if (n > AVATAR_MAX_CPUS) {
        KLOG_WARN("[cpu] CONFIG_SMP_CPUS=%u > max=%u, clamped\n", n, AVATAR_MAX_CPUS);
        n = AVATAR_MAX_CPUS;
    }
    return n;
}

/* ── CPU 初始化（每个 CPU 核心调用一次） ──────────────────– */

void
cpu_init(void)
{
    /* 在这个 CPU 上调用 */
    cpu_t *cpu = cpu_current();
    uint32_t cpu_id = cpu->cpu_id;

    if (cpu_id == 0) {
        KLOG_DEBUG("[cpu] Initializing CPU %u\n", cpu_id);
    }

    /* 初始化就绪队列 */
    list_init(&cpu->run_queue);
    
    /* 为这个 CPU 创建 idle 任务 */
    if (cpu_id < AVATAR_MAX_CPUS) {
        task_t *idle = &g_cpu_idle_tasks[cpu_id];
        memset(idle, 0, sizeof(task_t));
        
        idle->id = g_task_id_counter++;
        idle->state = TASK_RUNNING;
        idle->priority = 255;  /* 最低优先级 */
        idle->cpu_affinity = cpu_id;
        idle->stack_base = g_cpu_idle_stacks[cpu_id];
        idle->sp = (uintptr_t)(g_cpu_idle_stacks[cpu_id] + 4096);
        idle->entry = NULL;
        idle->arg = NULL;
        idle->fs_base = 0;
        idle->is_user_process = false;
        idle->user_started = false;
        idle->pgd = NULL;
        list_node_init(&idle->run_node);
        list_node_init(&idle->wait_node);
        
        /* 复制名称 */
        idle->name[0] = 'i';
        idle->name[1] = 'd';
        idle->name[2] = 'l';
        idle->name[3] = 'e';
        idle->name[4] = '-';
        idle->name[5] = '0' + cpu_id;
        idle->name[6] = '\0';
        
        /* 设置为当前任务 */
        cpu->current_task = idle;
        cpu->idle_task = idle;
        
        if (cpu_id == 0) {
            KLOG_INFO("[cpu] CPU %u created idle task id=%u\n", cpu_id, idle->id);
        }
    }

    g_cpu_online_bitmap |= (1U << cpu_id);

    if (cpu_id == 0) {
        KLOG_DEBUG("[cpu] CPU %u initialized successfully\n", cpu_id);
    }
}

uint32_t
cpu_online_count(void)
{
    uint32_t v = g_cpu_online_bitmap;
    uint32_t n = 0;
    while (v) {
        n += (v & 1U);
        v >>= 1;
    }
    return n;
}

void
cpu_secondary_bootstrap(uint32_t cpu_id, uint64_t mpidr)
{
    if (cpu_id >= AVATAR_MAX_CPUS) {
        return;
    }

    cpu_t *cpu = &g_cpus[cpu_id];
    memset(cpu, 0, sizeof(*cpu));
    cpu->cpu_id = cpu_id;
    cpu->mpidr = mpidr;
    cpu->need_resched = false;

#if ARCH_AARCH64
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(cpu));
    __asm__ volatile("isb");
#endif

    (void)mpidr; /* 次级核早期阶段避免日志输出（UART 可能尚未在该核可用） */

    cpu_init();
}

/* ── 启动所有其他 CPU 核心（仅 CPU0 调用） ──────────────────– */

#if ARCH_AARCH64

/*
 * AArch64: 通过 PSCI CPU_ON 启动其他核心
 * 需要知道：
 *   1. CPU 总数（通过 device tree 或硬编码）
 *   2. 每个 CPU 的入口地址
 */

static int64_t
psci_cpu_on(uint64_t cpuid, uint64_t entry, uint64_t context)
{
    /*
     * PSCI CPU_ON 调用
     * x0 = 函数编号 0xC4000003
     * x1 = 目标 CPU MPIDR
     * x2 = 入口地址
     * x3 = 上下文值（传递给目标 CPU）
     */
    register uint64_t x0 asm("x0") = 0xC4000003;  /* PSCI 2.0 CPU_ON 64-bit */
    register uint64_t x1 asm("x1") = cpuid;
    register uint64_t x2 asm("x2") = entry;
    register uint64_t x3 asm("x3") = context;

    /* 内核处于el2 使用smc */
    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "cc", "memory");

    return (int64_t)x0;
}

extern void secondary_cpu_start(void);  /* 定义在 boot/aarch64/boot.S */

void
cpu_bring_up_all(void)
{
    /*
     * 这个函数由 CPU0 在主内核启动时调用。
     * 它应该启动其他所有 CPU 核心。
     */

    /* CPU 数由编译期 CONFIG_SMP_CPUS 指定（来自 Makefile 的 SMP） */
    g_num_cpus = configured_cpu_count();

    KLOG_INFO("[cpu] Bringing up %u CPUs (CPU0 already running)\n", g_num_cpus);

    /* CPU0 已在线 */
    g_cpu_online_bitmap = 1U;

    /* 启动 CPU1..N-1 */
    for (uint32_t i = 1; i < g_num_cpus; i++) {
        /* 构造目标 CPU 的 MPIDR
         * 在 QEMU virt 中，MPIDR 格式：
         *   - Bits [7:0]:   Affinity level 0 (CPU ID)
         *   - Bits [15:8]:  Affinity level 1
         *   - Bits [23:16]: Affinity level 2
         *   - Bits [31:24]: Affinity level 3
         * 单核系统中就是简单的 CPU ID
         */
        uint64_t target_mpidr = (uint64_t)i;

        KLOG_DEBUG("[cpu] Starting CPU %u (MPIDR=0x%llx)\n", i, target_mpidr);

        /* 调用 PSCI CPU_ON 启动目标 CPU
         * PSCI 入口地址要求物理地址，不能传高半区虚拟地址。
         */
        uint64_t entry_pa = virt_to_phys((uint64_t)secondary_cpu_start);
        int64_t rc = psci_cpu_on(target_mpidr, entry_pa, (uint64_t)i);
        if (rc != 0) {
            KLOG_WARN("[cpu] PSCI_CPU_ON cpu=%u failed rc=%lld\n", i, rc);
        } else {
            KLOG_INFO("[cpu] PSCI_CPU_ON cpu=%u accepted (entry_pa=0x%llx)\n", i, entry_pa);
        }

        /* 给 CPU 一些时间启动 */
        for (volatile int j = 0; j < 1000000; j++);
    }

    KLOG_INFO("[cpu] CPU online bitmap=0x%x online=%u/%u\n",
              g_cpu_online_bitmap, cpu_online_count(), g_num_cpus);
    KLOG_INFO("[cpu] All CPUs brought up\n");
}

#elif ARCH_RISCV64

void
cpu_bring_up_all(void)
{
    /* RISC-V: 需要实现 PLIC + Hart IPI 或其他启动机制 */
    KLOG_WARN("[cpu] cpu_bring_up_all not yet implemented for RISC-V\n");
    g_num_cpus = 1;  /* 暂时只支持单核 */
}

#elif ARCH_X86_64

void
cpu_bring_up_all(void)
{
    /* x86_64: 需要实现 APIC + IPI 或其他启动机制 */
    KLOG_WARN("[cpu] cpu_bring_up_all not yet implemented for x86_64\n");
    g_num_cpus = 1;  /* 暂时只支持单核 */
}

#endif  /* ARCH_* */
