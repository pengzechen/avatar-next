#ifndef BARRIER_H
#define BARRIER_H

#include "types.h"
#include "arch.h"

/*
 * 跨架构内存屏障抽象层
 * 提供编译器屏障和 CPU 内存屏障
 */

/* ===== 编译器屏障 ===== */

/**
 * barrier_compiler - 编译器屏障
 *
 * 防止编译器重排指令，但不生成 CPU 指令
 */
#define barrier_compiler() asm volatile("" ::: "memory")

/* ===== 数据内存屏障 ===== */

/**
 * barrier_data - 数据内存屏障
 *
 * 确保之前的所有内存访问完成，才执行后续的内存访问
 * 类似于 Linux kernel 的 mb()
 */
static inline void barrier_data(void);

/**
 * barrier_data_read - 读数据屏障
 *
 * 确保之前的所有读操作完成，才执行后续的内存访问
 * 类似于 Linux kernel 的 rmb()
 */
static inline void barrier_data_read(void);

/**
 * barrier_data_write - 写数据屏障
 *
 * 确保之前的所有写操作完成，才执行后续的内存访问
 * 类似于 Linux kernel 的 wmb()
 */
static inline void barrier_data_write(void);

/* ===== 指令同步屏障 ===== */

/**
 * barrier_instr_full - 完整指令同步屏障
 *
 * 确保之前的所有指令都执行完成，才执行后续指令
 * 类似于 Linux kernel 的 isb()
 */
static inline void barrier_instr_full(void);

/* ===== 获取和释放语义 ===== */

/**
 * barrier_acquire - 获取语义屏障
 *
 * 确保读取操作之后的指令不会被重排到读取之前
 * 用于锁释放后的读操作
 */
static inline void barrier_acquire(void);

/**
 * barrier_release - 释放语义屏障
 *
 * 确保写入操作之前的指令不会被重排到写入之后
 * 用于锁获取前的写操作
 */
static inline void barrier_release(void);

/* ===== 辅助宏（兼容 Linux 内核风格）===== */

#define mb()         barrier_data()
#define rmb()        barrier_data_read()
#define wmb()        barrier_data_write()
#define isb()        barrier_instr_full()
#define smp_mb()     barrier_data()
#define smp_rmb()    barrier_data_read()
#define smp_wmb()    barrier_data_write()
#define smp_acquire() barrier_acquire()
#define smp_release() barrier_release()

/* ===== 架构特定实现 ===== */

#if defined(ARCH_X86_64)
    #include "x86_64/barrier_impl.h"
#elif defined(ARCH_AARCH64)
    #include "aarch64/barrier_impl.h"
#elif defined(ARCH_RISCV64)
    #include "riscv64/barrier_impl.h"
#else
    #error "Unsupported architecture"
#endif

#endif /* BARRIER_H */
