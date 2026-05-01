#ifndef AARCH64_BARRIER_IMPL_H
#define AARCH64_BARRIER_IMPL_H

/*
 * AArch64 内存屏障实现
 * 使用 DMB、DSB、ISB 指令
 */

/* ===== 数据内存屏障 ===== */

/**
 * barrier_data - 完整数据内存屏障
 *
 * DMB ISH: Inner Shareable 数据内存屏障
 * 确保所有可见的内存访问完成
 */
static inline void
barrier_data(void)
{
    asm volatile("dmb ish" ::: "memory");
}

/**
 * barrier_data_read - 读数据屏障
 *
 * DMB ISHLD: Inner Shareable 读数据屏障
 */
static inline void
barrier_data_read(void)
{
    asm volatile("dmb ishld" ::: "memory");
}

/**
 * barrier_data_write - 写数据屏障
 *
 * DMB ISHST: Inner Shareable 写数据屏障
 */
static inline void
barrier_data_write(void)
{
    asm volatile("dmb ishst" ::: "memory");
}

/* ===== 指令同步屏障 ===== */

/**
 * barrier_instr_full - 完整指令同步屏障
 *
 * ISB: 指令同步屏障，刷新流水线
 */
static inline void
barrier_instr_full(void)
{
    asm volatile("isb" ::: "memory");
}

/* ===== 获取和释放语义 ===== */

/**
 * barrier_acquire - 获取语义屏障
 *
 * 使用 LDAR 加载指令的隐含屏障
 */
static inline void
barrier_acquire(void)
{
    asm volatile("dmb ishld" ::: "memory");
}

/**
 * barrier_release - 释放语义屏障
 *
 * 使用 STLR 存储指令的隐含屏障
 */
static inline void
barrier_release(void)
{
    asm volatile("dmb ishst" ::: "memory");
}

#endif /* AARCH64_BARRIER_IMPL_H */
