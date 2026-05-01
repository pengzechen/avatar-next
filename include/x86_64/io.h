#ifndef X86_64_IO_H
#define X86_64_IO_H

#include "types.h"

/*
 * x86_64 Port I/O
 *
 * x86 拥有独立于内存地址空间的 I/O 端口空间（64K 个端口），
 * 通过专用指令 in/out 访问，与 MMIO 的 volatile 指针访问无关。
 *
 * x86 的 in/out 指令自带序列化语义，无需额外内存屏障。
 */

/* ===== 8-bit ===== */

static inline uint8_t
inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void
outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* ===== 16-bit ===== */

static inline uint16_t
inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void
outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

/* ===== 32-bit ===== */

static inline uint32_t
inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void
outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

#endif /* X86_64_IO_H */
