/*
 * stdlib.h — minimal stub for lwIP in freestanding environment
 *
 * lwIP uses atoi() from stdlib (netif.c). malloc/free includes in mem.c
 * are guarded by MEM_LIBC_MALLOC=0 and never processed.
 */
#ifndef LWIP_STDLIB_STUB_H
#define LWIP_STDLIB_STUB_H

int atoi(const char *s);

#endif /* LWIP_STDLIB_STUB_H */
