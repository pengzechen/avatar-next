/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#include "lwip/sys.h"

#include "driver/time.h"

u32_t sys_now(void)
{
    return (u32_t)timer_get_uptime_ms();
}
