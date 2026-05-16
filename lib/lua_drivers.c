/*
 * lib/lua_drivers.c — Lua bindings for kernel device drivers
 *
 * Each driver module is exposed as a Lua table (e.g. gicv2.virtual_init()).
 * Compiled with LUA_CFLAGS (FP enabled, compat headers accessible).
 */

#include "lua.h"
#include "lauxlib.h"

/* ── GICv2 bindings ──────────────────────────────────────────────────────── */
#if DRIVER_GIC_V2
#include "irq/gicv2.h"

static int lua_gicv2_virtual_init(lua_State *L)
{
    gic_virtual_init();
    return 0;
}

static int lua_gicv2_init(lua_State *L)
{
    gic_init();
    return 0;
}

static int lua_gicv2_gicc_init(lua_State *L)
{
    gicc_init();
    return 0;
}

static const luaL_Reg lua_drv_gicv2[] = {
    { "virtual_init", lua_gicv2_virtual_init },
    { "init",         lua_gicv2_init         },
    { "gicc_init",    lua_gicv2_gicc_init    },
    { NULL,           NULL                   }
};

int luaopen_gicv2(lua_State *L)
{
    luaL_newlib(L, lua_drv_gicv2);
    return 1;
}
#endif /* DRIVER_GIC_V2 */


/* ── PL011 UART bindings ─────────────────────────────────────────────────── */
#if DRIVER_UART_PL011
#include "uart/uart_pl011.h"

static int lua_pl011_init(lua_State *L)
{
    pl011_init();
    return 0;
}

static const luaL_Reg lua_drv_pl011[] = {
    { "init", lua_pl011_init },
    { NULL,   NULL           }
};

int luaopen_pl011(lua_State *L)
{
    luaL_newlib(L, lua_drv_pl011);
    return 1;
}
#endif /* DRIVER_UART_PL011 */


/* ── DW UART bindings ────────────────────────────────────────────────────── */
#if DRIVER_UART_DW
#include "uart/uart_dw.h"

static int lua_dw_uart_init(lua_State *L)
{
    dw_uart_init();
    return 0;
}

static const luaL_Reg lua_drv_dw_uart[] = {
    { "init", lua_dw_uart_init },
    { NULL,   NULL              }
};

int luaopen_dw_uart(lua_State *L)
{
    luaL_newlib(L, lua_drv_dw_uart);
    return 1;
}
#endif /* DRIVER_UART_DW */


/* ── Timer bindings (always present) ────────────────────────────────────── */
#include "timer/timer.h"

static int lua_timer_init(lua_State *L)
{
    timer_init();
    return 0;
}

static int lua_timer_enable(lua_State *L)
{
    timer_enable();
    return 0;
}

static int lua_timer_disable(lua_State *L)
{
    timer_disable();
    return 0;
}

static const luaL_Reg lua_drv_timer[] = {
    { "init",    lua_timer_init    },
    { "enable",  lua_timer_enable  },
    { "disable", lua_timer_disable },
    { NULL,      NULL              }
};

int luaopen_timer(lua_State *L)
{
    luaL_newlib(L, lua_drv_timer);
    return 1;
}


/* ── Registration helper called from lua_platform.c ─────────────────────── */

void lua_register_all_drivers(lua_State *L)
{
#if DRIVER_GIC_V2
    luaL_requiref(L, "gicv2",    luaopen_gicv2,    1); lua_pop(L, 1);
#endif
#if DRIVER_UART_PL011
    luaL_requiref(L, "pl011",    luaopen_pl011,    1); lua_pop(L, 1);
#endif
#if DRIVER_UART_DW
    luaL_requiref(L, "dw_uart",  luaopen_dw_uart,  1); lua_pop(L, 1);
#endif
    luaL_requiref(L, "timer",    luaopen_timer,    1); lua_pop(L, 1);
}
