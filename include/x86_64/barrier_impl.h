#ifndef X86_64_BARRIER_IMPL_H
#define X86_64_BARRIER_IMPL_H

/*
 * x86_64 内存屏障实现
 * 使用 MFENCE、SFENCE、LFENCE 指令
 */

/* ===== 数据内存屏障 ===== */

/**
 * barrier_data - 完整数据内存屏障
 *
 * MFENCE: 内存屏障（读+写）
 */
static inline void
barrier_data(void)
{
    asm volatile("mfence" ::: "memory");
}

/**
 * barrier_data_read - 读数据屏障
 *
 * LFENCE: 加载屏障
 */
static inline void
barrier_data_read(void)
{
    asm volatile("lfence" ::: "memory");
}

/**
 * barrier_data_write - 写数据屏障
 *
 * SFENCE: 存储屏障
 */
static inline void
barrier_data_write(void)
{
    asm volatile("sfence" ::: "memory");
}

/* ===== 指令同步屏障 ===== */

/**
 * barrier_instr_full - 完整指令同步屏障
 *
 * x86_64 上 MFENCE 已经提供了完整的同步
 * CPUID 也可以用作指令序列化器
 */
static inline void
barrier_instr_full(void)
{
    asm volatile("mfence" ::: "memory");
}

/* ===== 获取和释放语义 ===== */

/**
 * barrier_acquire - 获取语义屏障
 *
 * x86_64 的 TSO (Total Store Order) 模型已经提供了较强的保证
 * 使用编译器屏障即可
 */
static inline void
barrier_acquire(void)
{
    asm volatile("" ::: "memory");
}

/**
 * barrier_release - 释放语义屏障
 *
 * x86_64 的 TSO 模型已经提供了较强的保证
 * 使用编译器屏障即可
 */
static inline void
barrier_release(void)
{
    asm volatile("" ::: "memory");
}

#endif /* X86_64_BARRIER_IMPL_H */
