/*
 * stdio.h — minimal stub for lwIP in freestanding environment
 *
 * lwIP only uses snprintf() from stdio, and only when MEM_OVERFLOW_CHECK
 * is enabled (disabled by default). We provide just the declaration so
 * the header can be included with -nostdinc.
 */
#ifndef LWIP_STDIO_STUB_H
#define LWIP_STDIO_STUB_H

/* snprintf — declared to satisfy the include; never called when
 * MEM_OVERFLOW_CHECK is 0. */
int snprintf(char *str, unsigned long size, const char *format, ...);

#endif /* LWIP_STDIO_STUB_H */
