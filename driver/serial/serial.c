/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/serial/serial.c — 串口 class 核心

   g_serial_ops 在串口 probe 之前指向 null 实现，保证 klog/panic 在最早
   阶段调用 serial_putc 不会空指针。后端 probe 时通过 serial_bind() 换掉。
*/

#include "driver/serial.h"

static void serial_null_putc(char c)
{
    (void)c;
}

static void serial_null_puts(const char *s)
{
    (void)s;
}

static char serial_null_getc(void)
{
    return 0;
}

static bool serial_null_getc_nb(char *c)
{
    (void)c;
    return false;
}

static bool serial_null_rx_ready(void)
{
    return false;
}

static const serial_ops_t serial_null_ops = {
    .putc = serial_null_putc,
    .puts = serial_null_puts,
    .getc = serial_null_getc,
    .getc_nb = serial_null_getc_nb,
    .rx_ready = serial_null_rx_ready,
};

const serial_ops_t *g_serial_ops = &serial_null_ops;

void serial_bind(const serial_ops_t *ops)
{
    if (ops)
        g_serial_ops = ops;
}
