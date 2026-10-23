/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef SHIM_CACHE_H
#define SHIM_CACHE_H
#include <stddef.h>
static inline void clean_dcache_range(const void *addr, size_t size)
{ (void)addr; (void)size; }
#endif
