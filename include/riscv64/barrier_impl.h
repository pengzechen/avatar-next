#ifndef RISCV64_BARRIER_IMPL_H
#define RISCV64_BARRIER_IMPL_H

/*
 * RISC-V 64位内存屏障实现
 * 使用 FENCE 指令
 */

/* ===== 数据内存屏障 ===== */

/**
 * barrier_data - 完整数据内存屏障
 *
 * FENCE RW,RW: 读+写屏障
 */
static inline void
barrier_data(void)
{
    asm volatile("fence rw, rw" ::: "memory");
}

/**
 * barrier_data_read - 读数据屏障
 *
 * FENCE R,R: 读屏障
 */
static inline void
barrier_data_read(void)
{
    asm volatile("fence r, r" ::: "memory");
}

/**
 * barrier_data_write - 写数据屏障
 *
 * FENCE W,W: 写屏障
 */
static inline void
barrier_data_write(void)
{
    asm volatile("fence w, w" ::: "memory");
}

/* ===== 指令同步屏障 ===== */

/**
 * barrier_instr_full - 完整指令同步屏障
 *
 * FENCE IORW,IORW: IO + 读 + 写 屏障
 * 用于同步指令和数据流
 */
static inline void
barrier_instr_full(void)
{
    asm volatile("fence iorw, iorw" ::: "memory");
}

/* ===== 获取和释放语义 ===== */

/**
 * barrier_acquire - 获取语义屏障
 *
 * FENCE R,R: 读屏障确保加载操作完成
 */
static inline void
barrier_acquire(void)
{
    asm volatile("fence r, r" ::: "memory");
}

/**
 * barrier_release - 释放语义屏障
 *
 * FENCE WW,WW: 写屏障确保存储操作对其他核心可见
 */
static inline void
barrier_release(void)
{
    asm volatile("fence w, w" ::: "memory");
}

#endif /* RISCV64_BARRIER_IMPL_H */
