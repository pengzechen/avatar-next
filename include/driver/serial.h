/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/serial.h — 串口公共接口

   内核只依赖本文件的 serial_* API；具体后端（pl011/dw/x86）在 probe 时
   通过 serial_bind() 交出 ops。未绑定前 g_serial_ops 指向 null 实现，
   早期 boot 不会因为空指针而崩。
*/

#ifndef DRIVER_SERIAL_H
#define DRIVER_SERIAL_H

#include "types.h"

typedef struct {
    uintptr_t base;         /* 原始物理地址；驱动用 device_mmio() 换算 */
    uint32_t reg_shift;     /* 0 = 字节寻址；2 = 4 字节步长 */
    uint32_t irq;           /* 0 = 暂不接中断 */
    bool irq_mode;          /* true：在 DRV_PHASE_IRQ 的 start 里切中断模式 */
    const char *compatible; /* 可选 sanity check，如 "arm,pl011" */
} serial_cfg_t;

typedef struct {
    void (*putc)(char c);
    void (*puts)(const char *s);
    char (*getc)(void);
    bool (*getc_nb)(char *c);
    bool (*rx_ready)(void);
} serial_ops_t;

/* 由 driver/serial/serial.c 实现；后端 probe 时调用 */
void serial_bind(const serial_ops_t *ops);
extern const serial_ops_t *g_serial_ops;

static inline void serial_putc(char c)
{
    g_serial_ops->putc(c);
}

static inline void serial_puts(const char *s)
{
    g_serial_ops->puts(s);
}

static inline char serial_getc(void)
{
    return g_serial_ops->getc();
}

static inline bool serial_getc_nb(char *c)
{
    return g_serial_ops->getc_nb(c);
}

static inline bool serial_rx_ready(void)
{
    return g_serial_ops->rx_ready();
}

#endif /* DRIVER_SERIAL_H */
