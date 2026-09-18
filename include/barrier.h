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

/* ===== 全系统同步屏障（dsb 强度）===== */

/**
 * barrier_sync - 全系统同步屏障
 *
 * 与 barrier_data() 的区别是**强度**：barrier_data 是数据屏障（aarch64 上是
 * `dmb ish`，只保证内部共享域的访存顺序），而本函数是同步屏障
 * （aarch64 `dsb sy`），会一直等到之前的访存真正完成、对全系统可见。
 *
 * 什么时候需要它：对设备/页表这类"写完必须确保已经到达"的场景 ——
 * GIC 寄存器写之后的 dsb、TLB/EPT 失效（tlbi/hfence）之后的 dsb、
 * 页表项写完再切 TTBR/satp 之前。这些地方**不能**用 barrier_data() 代替：
 * dmb 比 dsb 弱，替换会把原本正确的同步削弱。
 *
 * 映射：aarch64 `dsb sy` / riscv64 `fence iorw,iorw` / x86_64 `mfence`
 */
static inline void barrier_sync(void);

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
