#ifndef KERNEL_TASK_SWITCH_H
#define KERNEL_TASK_SWITCH_H

/*
 * kernel/task/switch.h — 架构抽象：上下文切换 & 中断控制
 *
 * 提供：
 *   arch_task_switch()      — 在 .S 文件中实现，保存/恢复被调用者寄存器
 *   arch_irq_save()         — 关中断并保存中断状态
 *   arch_irq_restore()      — 恢复中断状态
 *   arch_irq_enable()       — 无条件开中断（新任务首次运行时调用）
 *   arch_init_task_stack()  — 为新任务构造初始切换帧
 *
 * 上下文切换原理：
 *   仅保存"被调用者保存寄存器"（callee-saved registers）。
 *   调用者保存寄存器由 C 调用约定在调用 sched_schedule() 前
 *   自动保存到栈帧，arch_task_switch() 无需额外处理。
 *
 * 新任务初始化：
 *   arch_init_task_stack() 在新任务的栈顶构造一个伪造的切换帧，
 *   使首次 arch_task_switch() "返回"时跳转到 task_trampoline()。
 */

#include "types.h"
#include "arch.h"

/* 前向声明：task_trampoline 在 task.c 中实现 */
void task_trampoline(void);

/* 前向声明：task_trampoline_user 在 switch.S 中实现（汇编） */
void task_trampoline_user(void);

/* ── 架构特定：上下文切换（在 .S 文件中实现）─────────────── */

/**
 * arch_task_switch - 保存当前任务上下文并切换到下一任务
 * @prev_sp: 输出 — 将当前 SP 写入 *prev_sp
 * @next_sp: 输入 — 要加载的下一任务 SP
 * @prev_pgd_ptr: 输出 — 将当前 TTBR0_EL1 写入 *prev_pgd_ptr（可为NULL）
 * @next_pgd: 输入 — 下一任务的页表基址（NULL表示不切换）
 *
 * 执行流程：
 *   1. 将被调用者保存寄存器压栈
 *   2. 将 SP 写入 *prev_sp
 *   3. 将 SP 设为 next_sp
 *   4. 如果 prev_pgd_ptr != NULL，将当前 TTBR0_EL1 保存到 *prev_pgd_ptr
 *   5. 如果 next_pgd != NULL，切换 TTBR0_EL1 到 next_pgd
 *   6. 弹出被调用者保存寄存器
 *   7. ret（跳转到 next_sp 帧中保存的返回地址）
 */
void arch_task_switch(uintptr_t *prev_sp, uintptr_t next_sp,
                      uint64_t **prev_pgd_ptr, uint64_t *next_pgd);

/**
 * arch_switch_to_user - 从内核跳转到用户态
 * @user_entry: 用户态入口点（虚拟地址）
 * @user_sp: 用户栈指针（虚拟地址）
 * @kernel_sp: 内核栈指针（物理地址）
 *
 * 执行流程：
 *   1. 设置异常返回地址到 user_entry
 *   2. 设置用户栈指针到 user_sp
 *   3. 设置 SPSR 到 EL0t（用户态）
 *   4. 保存内核栈指针到 SP_EL0
 *   5. 执行 eret 跳转到用户态
 *
 * 注意：此函数不返回，会跳转到用户态执行。
 */
void arch_switch_to_user(uint64_t user_entry, uint64_t user_sp, uint64_t kernel_sp) __attribute__((noreturn));

/* ── 中断控制 ────────────────────────────────────────────── */

#if ARCH_AARCH64

/* 保存 DAIF，屏蔽 IRQ（置位 DAIF.I），返回旧 DAIF */
static inline uint64_t
arch_irq_save(void)
{
    uint64_t daif;
    __asm__ volatile(
        "mrs %0, daif       \n"
        "msr daifset, #2    \n"
        : "=r"(daif)
        :
        : "memory");
    return daif;
}

/* 恢复 DAIF */
static inline void
arch_irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

/* 无条件开 IRQ（新任务首次运行时使用） */
static inline void
arch_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

#elif ARCH_RISCV64

/* 保存 sstatus，清除 SIE 位（关中断），返回旧 sstatus */
static inline uint64_t
arch_irq_save(void)
{
    uint64_t status;
    /* csrrci: 读取 sstatus 后清除 bit1 (SIE) */
    __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(status) :: "memory");
    return status;
}

/* 恢复 sstatus */
static inline void
arch_irq_restore(uint64_t flags)
{
    __asm__ volatile("csrw sstatus, %0" :: "r"(flags) : "memory");
}

/* 无条件开 IRQ */
static inline void
arch_irq_enable(void)
{
    __asm__ volatile("csrsi sstatus, 2" ::: "memory");
}

#elif ARCH_X86_64

/* 保存 RFLAGS，执行 CLI（关中断），返回旧 RFLAGS */
static inline uint64_t
arch_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile(
        "pushfq         \n"
        "popq %0        \n"
        "cli            \n"
        : "=r"(flags)
        :
        : "memory");
    return flags;
}

/* 恢复 RFLAGS（POPFQ 会恢复 IF 位） */
static inline void
arch_irq_restore(uint64_t flags)
{
    __asm__ volatile(
        "pushq %0       \n"
        "popfq          \n"
        :
        : "r"(flags)
        : "memory", "cc");
}

/* 无条件开 IRQ */
static inline void
arch_irq_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

#endif /* ARCH_* */

/* ── 新任务栈初始化 ───────────────────────────────────────── */

/**
 * arch_init_task_stack - 在新任务的栈上构造伪切换帧
 * @stack_base: 栈底（低地址）
 * @stack_size: 栈大小（字节）
 *
 * 返回：应写入 task->sp 的初始值。
 *
 * 构造的帧与 arch_task_switch() 保存的帧格式完全一致，
 * 使得首次调度到该任务时 arch_task_switch 的 "ret"
 * 跳转到 task_trampoline()。
 */
static inline uintptr_t
arch_init_task_stack(uint8_t *stack_base, uint32_t stack_size)
{
    uint64_t *sp = (uint64_t *)((uintptr_t)(stack_base + stack_size));

#if ARCH_AARCH64
    /*
     * arch_task_switch 保存顺序（stp x19,x20 [sp,#-96]! ... stp x29,x30 [sp,#80]）：
     * saved_sp + 0  = x19,  saved_sp + 8  = x20
     * saved_sp + 16 = x21,  saved_sp + 24 = x22
     * saved_sp + 32 = x23,  saved_sp + 40 = x24
     * saved_sp + 48 = x25,  saved_sp + 56 = x26
     * saved_sp + 64 = x27,  saved_sp + 72 = x28
     * saved_sp + 80 = x29,  saved_sp + 88 = x30 (LR)  ← ret 目标
     * 共 12 × 8 = 96 字节
     */
    sp -= 12;
    for (int i = 0; i < 11; i++)
        sp[i] = 0;
    sp[11] = (uint64_t)task_trampoline; /* x30 (LR) */

#elif ARCH_RISCV64
    /*
     * arch_task_switch 保存顺序（sd ra 0(sp) ... sd s11 96(sp)）：
     * saved_sp + 0  = ra    ← ret 目标
     * saved_sp + 8  = s0,  saved_sp + 16 = s1
     * ...
     * saved_sp + 96 = s11
     * 共 13 × 8 = 104 字节
     */
    sp -= 13;
    sp[0] = (uint64_t)task_trampoline; /* ra */
    for (int i = 1; i < 13; i++)
        sp[i] = 0;

#elif ARCH_X86_64
    /*
     * arch_task_switch 保存顺序（pushq rbx, rbp, r12, r13, r14, r15）：
     * 恢复顺序（popq r15, r14, r13, r12, rbp, rbx, ret）：
     * saved_sp + 0  = r15
     * saved_sp + 8  = r14
     * saved_sp + 16 = r13
     * saved_sp + 24 = r12
     * saved_sp + 32 = rbp
     * saved_sp + 40 = rbx
     * saved_sp + 48 = 返回地址  ← ret 目标
     * 共 7 × 8 = 56 字节
     */
    sp -= 7;
    for (int i = 0; i < 6; i++)
        sp[i] = 0;
    sp[6] = (uint64_t)task_trampoline; /* return address */

#endif /* ARCH_* */

    return (uintptr_t)sp;
}

/* ── 用户进程栈初始化 ───────────────────────────────────────── */

/**
 * arch_init_user_stack - 为用户进程初始化内核栈（首次跳转用）
 * @stack_base: 内核栈底（低地址）
 * @stack_size: 内核栈大小（字节）
 * @user_entry: 用户态入口点（虚拟地址）
 * @user_sp: 用户栈指针（虚拟地址）
 *
 * 返回：应写入 task->sp 的初始内核栈指针。
 *
 * 构造的帧使得首次调度时跳转到 task_trampoline_user，
 * 然后调用 arch_switch_to_user 跳转到用户态。
 */
static inline uintptr_t
arch_init_user_stack(uint8_t *stack_base, uint32_t stack_size,
                     uint64_t user_entry, uint64_t user_sp)
{
    uint64_t *sp = (uint64_t *)((uintptr_t)(stack_base + stack_size));

#if ARCH_AARCH64
    /*
     * 保存12个被调用者寄存器（96 字节）
     * 布局：
     *   [0]   x19=user_entry  [8]   x20=user_sp
     *   [16]  x21=unused       [24]  x22
     *   [32]  x23              [40]  x24
     *   [48]  x25              [56]  x26
     *   [64]  x27              [72]  x28
     *   [80]  x29              [88]  x30 (LR) → task_trampoline_user_asm
     */
    sp -= 12;
    sp[0] = user_entry;                  /* x19 = 用户入口 */
    sp[1] = user_sp;                     /* x20 = 用户栈 */
    for (int i = 2; i < 11; i++)
        sp[i] = 0;
    sp[11] = (uint64_t)task_trampoline_user; /* x30 (LR) */

#elif ARCH_RISCV64
    /* TODO: RISC-V 支持 */
    (void)user_entry;
    (void)user_sp;
    sp -= 16;
    sp[0] = (uint64_t)task_trampoline_user;
    for (int i = 1; i < 16; i++)
        sp[i] = 0;

#elif ARCH_X86_64
    /* TODO: x86_64 支持 */
    (void)user_entry;
    (void)user_sp;
    sp -= 10;
    sp[6] = (uint64_t)task_trampoline_user;
    for (int i = 0; i < 6; i++)
        sp[i] = 0;
#endif

    return (uintptr_t)sp;
}

/* ── 前向声明：用户进程trampoline（汇编实现）──────────────────── */
void task_trampoline_user(void);

#endif /* KERNEL_TASK_SWITCH_H */
