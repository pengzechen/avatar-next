#ifndef MMIO_H
#define MMIO_H

#include "types.h"
#include "barrier.h"

/*
 * MMIO (Memory-Mapped I/O) 操作
 * 提供带内存屏障和不带屏障的 MMIO 访问函数
 */

/* ===== 不带屏障的 MMIO 操作（Relaxed） ===== */

/**
 * read8_relaxed - 读取 8 位 MMIO 寄存器（无屏障）
 * @addr: MMIO 地址
 *
 * 返回值: 读取的值
 *
 * 注意：不保证内存排序，仅在明确不需要屏障时使用
 */
static inline uint8_t
read8_relaxed(const volatile void *addr)
{
    return *(const volatile uint8_t *)addr;
}

/**
 * write8_relaxed - 写入 8 位 MMIO 寄存器（无屏障）
 * @value: 要写入的值
 * @addr: MMIO 地址
 *
 * 注意：不保证内存排序和写入可见性
 */
static inline void
write8_relaxed(uint8_t value, volatile void *addr)
{
    *(volatile uint8_t *)addr = value;
}

static inline uint16_t
read16_relaxed(const volatile void *addr)
{
    return *(const volatile uint16_t *)addr;
}

static inline void
write16_relaxed(uint16_t value, volatile void *addr)
{
    *(volatile uint16_t *)addr = value;
}

static inline uint32_t
read32_relaxed(const volatile void *addr)
{
    return *(const volatile uint32_t *)addr;
}

static inline void
write32_relaxed(uint32_t value, volatile void *addr)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint64_t
read64_relaxed(const volatile void *addr)
{
    return *(const volatile uint64_t *)addr;
}

static inline void
write64_relaxed(uint64_t value, volatile void *addr)
{
    *(volatile uint64_t *)addr = value;
}

/* ===== 带完整屏障的 MMIO 操作 ===== */

/**
 * read8 - 读取 8 位 MMIO 寄存器（带屏障）
 * @addr: MMIO 地址
 *
 * 返回值: 读取的值
 *
 * 确保读取操作完成，且后续操作不会被重排到读取之前
 */
static inline uint8_t
read8(const volatile void *addr)
{
    uint8_t value;
    barrier_data();
    value = *(const volatile uint8_t *)addr;
    barrier_data();
    return value;
}

/**
 * write8 - 写入 8 位 MMIO 寄存器（带屏障）
 * @value: 要写入的值
 * @addr: MMIO 地址
 *
 * 确保之前的操作完成，且写入立即对设备可见
 */
static inline void
write8(uint8_t value, volatile void *addr)
{
    barrier_data();
    *(volatile uint8_t *)addr = value;
    barrier_data();
}

static inline uint16_t
read16(const volatile void *addr)
{
    uint16_t value;
    barrier_data();
    value = *(const volatile uint16_t *)addr;
    barrier_data();
    return value;
}

static inline void
write16(uint16_t value, volatile void *addr)
{
    barrier_data();
    *(volatile uint16_t *)addr = value;
    barrier_data();
}

static inline uint32_t
read32(const volatile void *addr)
{
    uint32_t value;
    barrier_data();
    value = *(const volatile uint32_t *)addr;
    barrier_data();
    return value;
}

static inline void
write32(uint32_t value, volatile void *addr)
{
    barrier_data();
    *(volatile uint32_t *)addr = value;
    barrier_data();
}

static inline uint64_t
read64(const volatile void *addr)
{
    uint64_t value;
    barrier_data();
    value = *(const volatile uint64_t *)addr;
    barrier_data();
    return value;
}

static inline void
write64(uint64_t value, volatile void *addr)
{
    barrier_data();
    *(volatile uint64_t *)addr = value;
    barrier_data();
}

/* ===== 优化的 MMIO 操作（仅写屏障） ===== */

/**
 * write8_post - 写入 8 位 MMIO 寄存器（仅后屏障）
 * @value: 要写入的值
 * @addr: MMIO 地址
 *
 * 只在写入后添加屏障，确保写入对设备可见
 * 适用于：写入后立即需要设备响应的场景
 */
static inline void
write8_post(uint8_t value, volatile void *addr)
{
    *(volatile uint8_t *)addr = value;
    barrier_data_write();
}

static inline void
write16_post(uint16_t value, volatile void *addr)
{
    *(volatile uint16_t *)addr = value;
    barrier_data_write();
}

static inline void
write32_post(uint32_t value, volatile void *addr)
{
    *(volatile uint32_t *)addr = value;
    barrier_data_write();
}

static inline void
write64_post(uint64_t value, volatile void *addr)
{
    *(volatile uint64_t *)addr = value;
    barrier_data_write();
}

/* ===== 设备特定的访问宏 ===== */

/**
 * mmio_readb - 读取 8 位寄存器（带屏障）
 * mmio_readw - 读取 16 位寄存器（带屏障）
 * mmio_readl - 读取 32 位寄存器（带屏障）
 * mmio_writeb - 写入 8 位寄存器（带屏障）
 * mmio_writew - 写入 16 位寄存器（带屏障）
 * mmio_writel - 写入 32 位寄存器（带屏障）
 */
#define mmio_readb(addr)    read8(addr)
#define mmio_readw(addr)    read16(addr)
#define mmio_readl(addr)    read32(addr)
#define mmio_writeb(val, addr)  write8(val, addr)
#define mmio_writew(val, addr)  write16(val, addr)
#define mmio_writel(val, addr)  write32(val, addr)

/* ===== Relaxed 版本（无屏障）===== */
#define mmio_readb_relaxed(addr)  read8_relaxed(addr)
#define mmio_readw_relaxed(addr)  read16_relaxed(addr)
#define mmio_readl_relaxed(addr)  read32_relaxed(addr)
#define mmio_writeb_relaxed(val, addr) write8_relaxed(val, addr)
#define mmio_writew_relaxed(val, addr) write16_relaxed(val, addr)
#define mmio_writel_relaxed(val, addr) write32_relaxed(val, addr)

#endif /* MMIO_H */
