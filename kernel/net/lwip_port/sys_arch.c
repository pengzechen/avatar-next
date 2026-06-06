#include "lwip/sys.h"

#include "timer/timer.h"

u32_t sys_now(void)
{
    return (u32_t)timer_get_uptime_ms();
}
