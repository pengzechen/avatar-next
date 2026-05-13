#ifndef KERNEL_TASK_CPU_H
#define KERNEL_TASK_CPU_H

/*
 * kernel/task/cpu.h — Per-CPU 数据结构与多核支持
 *
 * 提供：
 *   - cpu_t: Per-CPU 控制块
 *   - 通过 TPIDR_EL1 快速访问当前 CPU 数据
 *   - Per-CPU 调度队列、idle 任务等
 */

#include "types.h"
#include "list.h"

#define AVATAR_MAX_CPUS 8U

/* 前向声明 */
typedef struct task task_t;

/* ── Per-CPU 控制块 ──────────────────────────────────────── */
typedef struct {
    uint32_t  cpu_id;                /* CPU 编号（0~N-1）               */
    uint64_t  mpidr;                 /* MPIDR_EL1（CPU 识别码）         */
    task_t   *current_task;          /* 当前运行的任务                   */
    task_t   *idle_task;             /* idle 任务（队空时运行）          */
    list_t    run_queue;             /* 就绪任务队列（FIFO）            */
    volatile bool need_resched;      /* 是否需要重新调度                 */
    
    /* idle 任务栈（每个 CPU 专用）*/
    uint8_t   *idle_stack_base;      /* Idle 栈基址                      */
    uint32_t  idle_stack_size;       /* Idle 栈大小                      */
    
    /* 内核启动栈（CPU 启动时使用）*/
    uint8_t   *stack_base;           /* CPU 启动栈基址                   */
    uint8_t   *stack_top;            /* CPU 启动栈顶                     */
    uint32_t  stack_size;            /* CPU 栈大小                       */
} cpu_t;

/* ── CPU 核心数（启动时自动检测） ────────────────────────– */
extern uint32_t g_num_cpus;

/* ── CPU 池（最多 8 核）────────────────────────────────── */
extern cpu_t g_cpus[AVATAR_MAX_CPUS];

/* ── 获取当前 CPU 指针（使用 TPIDR_EL1） ──────────────────── */
static inline cpu_t *
cpu_current(void)
{
#if ARCH_AARCH64
    uint64_t tpidr;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(tpidr));
    return (cpu_t *)tpidr;
#elif ARCH_RISCV64
    uint64_t tp;
    __asm__ volatile("mv %0, tp" : "=r"(tp));
    return (cpu_t *)tp;
#elif ARCH_X86_64
    /* x86_64: 使用 FS base MSR */
    uint64_t fs_base;
    __asm__ volatile("rdmsr" : "=A"(fs_base) : "c"(0xC0000100U));
    return (cpu_t *)fs_base;
#endif
}

/* ── 获取当前 CPU 编号 ────────────────────────────────────── */
static inline uint32_t
get_current_cpu_id(void)
{
    cpu_t *cpu = cpu_current();
    return cpu ? cpu->cpu_id : 0;
}

/* ── CPU 初始化（启动时调用） ────────────────────────────── */
void cpu_init(void);

/* ── CPU 启动（CPU0 调用，启动其他 CPU） ───────────────────– */
void cpu_bring_up_all(void);

/* ── 次级核早期引导（secondary_cpu_start 调用）──────────────── */
void cpu_secondary_bootstrap(uint32_t cpu_id, uint64_t mpidr);

/* ── 在线 CPU 数（用于观测启动状态）────────────────────────── */
uint32_t cpu_online_count(void);

#endif /* KERNEL_TASK_CPU_H */
