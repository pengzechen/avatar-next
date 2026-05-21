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


/* ── GICv3 bindings ─────────────────────────────────────────────────────── */
#if DRIVER_GIC_V3
#include "irq/gicv3.h"

static int lua_gicv3_init(lua_State *L)
{
    gicv3_init();
    return 0;
}

static int lua_gicv3_enable_int(lua_State *L)
{
    int id     = (int)luaL_checkinteger(L, 1);
    bool en    = lua_toboolean(L, 2);
    gicv3_enable_int(id, en);
    return 0;
}

static const luaL_Reg lua_drv_gicv3[] = {
    { "init",       lua_gicv3_init       },
    { "enable_int", lua_gicv3_enable_int },
    { NULL,         NULL                 }
};

int luaopen_gicv3(lua_State *L)
{
    luaL_newlib(L, lua_drv_gicv3);
    return 1;
}
#endif /* DRIVER_GIC_V3 */


/* ── RKNPU bindings ──────────────────────────────────────────────────────── */
#if DRIVER_NPU_RKNPU
#include "npu/rknpu.h"

static int lua_rknpu_init(lua_State *L)
{
    rknpu_init();
    return 0;
}

static int lua_rknpu_validate_version(lua_State *L)
{
    bool ok = rknpu_validate_version();
    lua_pushboolean(L, ok);
    return 1;
}

static const luaL_Reg lua_drv_rknpu[] = {
    { "init",             lua_rknpu_init             },
    { "validate_version", lua_rknpu_validate_version },
    { NULL,               NULL                       }
};

int luaopen_rknpu(lua_State *L)
{
    luaL_newlib(L, lua_drv_rknpu);
    return 1;
}
#endif /* DRIVER_NPU_RKNPU */

#if DRIVER_TPU_CVITPU
#include "tpu/cvi_tpu.h"

static int lua_cvi_tpu_init(lua_State *L)
{
    cvi_tpu_init();
    return 0;
}

static int lua_cvi_tpu_run_dmabuf(lua_State *L)
{
    void    *v = lua_touserdata(L, 1);
    uint64_t p = (uint64_t)luaL_checkinteger(L, 2);
    lua_pushinteger(L, cvi_tpu_run_dmabuf(v, p));
    return 1;
}

static int lua_cvi_tpu_is_ready(lua_State *L)
{
    lua_pushboolean(L, cvi_tpu_is_ready() ? 1 : 0);
    return 1;
}

static const luaL_Reg lua_drv_cvi_tpu[] = {
    { "init",         lua_cvi_tpu_init         },
    { "run_dmabuf",   lua_cvi_tpu_run_dmabuf   },
    { "is_ready",     lua_cvi_tpu_is_ready     },
    { NULL,           NULL                     }
};

int luaopen_cvi_tpu(lua_State *L)
{
    luaL_newlib(L, lua_drv_cvi_tpu);
    return 1;
}
#endif /* DRIVER_TPU_CVITPU */

#if DRIVER_ION
#include "ion/ion.h"

/*
 * ion.alloc(size) → handle, vaddr_lightuserdata, paddr_integer
 * 失败返回 nil
 */
static int lua_ion_alloc(lua_State *L)
{
    size_t size = (size_t)luaL_checkinteger(L, 1);
    void          *va  = NULL;
    uint64_t       pa  = 0;
    ion_handle_t   h   = ION_HANDLE_INVALID;

    if (ion_alloc(size, &va, &pa, &h) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)h);
    lua_pushlightuserdata(L, va);
    lua_pushinteger(L, (lua_Integer)pa);
    return 3;
}

/* ion.free(handle) → bool */
static int lua_ion_free(lua_State *L)
{
    ion_handle_t h = (ion_handle_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, ion_free(h) == 0 ? 1 : 0);
    return 1;
}

/* ion.get(handle) → vaddr_lightuserdata, paddr_integer (or nil on error) */
static int lua_ion_get(lua_State *L)
{
    ion_handle_t h  = (ion_handle_t)luaL_checkinteger(L, 1);
    void        *va = NULL;
    uint64_t     pa = 0;
    if (ion_get_buf(h, &va, &pa) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, va);
    lua_pushinteger(L, (lua_Integer)pa);
    return 2;
}

/* ion.size(handle) → integer */
static int lua_ion_size(lua_State *L)
{
    ion_handle_t h = (ion_handle_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)ion_get_size(h));
    return 1;
}

static const luaL_Reg lua_drv_ion[] = {
    { "alloc", lua_ion_alloc },
    { "free",  lua_ion_free  },
    { "get",   lua_ion_get   },
    { "size",  lua_ion_size  },
    { NULL,    NULL          }
};

int luaopen_ion(lua_State *L) { luaL_newlib(L, lua_drv_ion); return 1; }
#endif /* DRIVER_ION */


/* ── sdblk bindings ─────────────────────────────────────────────────────── */
/* ── DWC3 USB bindings ──────────────────────────────────────────────────── */
#if DRIVER_USB_DWC3
#include "usb/dwc3.h"

static int lua_dwc3_probe(lua_State *L)
{
    (void)L;
    dwc3_probe();
    return 0;
}

static int lua_dwc3_host_init(lua_State *L)
{
    (void)L;
    dwc3_host_init();
    return 0;
}

static int lua_dwc3_xhci_start(lua_State *L)
{
    (void)L;
    dwc3_xhci_start();
    return 0;
}

static int lua_dwc3_hid_enumerate(lua_State *L)
{
    (void)L;
    dwc3_hid_enumerate();
    return 0;
}

static const luaL_Reg lua_drv_dwc3[] = {
    { "probe",         lua_dwc3_probe         },
    { "host_init",     lua_dwc3_host_init     },
    { "xhci_start",    lua_dwc3_xhci_start    },
    { "hid_enumerate", lua_dwc3_hid_enumerate },
    { NULL,            NULL                   }
};

int luaopen_dwc3(lua_State *L)
{
    luaL_newlib(L, lua_drv_dwc3);
    return 1;
}
#endif /* DRIVER_USB_DWC3 */


#if DRIVER_SDBLK_SG2002
#include "blk/sdblk.h"

/* sdblk.init() → int (0=ok, -2=nocard, -1=err) */
static int lua_sdblk_init(lua_State *L)
{
    lua_pushinteger(L, sdblk_init());
    return 1;
}

/* sdblk.read_blocks(block_id, buf_userdata, count) → int */
static int lua_sdblk_read_blocks(lua_State *L)
{
    uint32_t block_id = (uint32_t)luaL_checkinteger(L, 1);
    void    *buf      = lua_touserdata(L, 2);
    size_t   count    = (size_t)luaL_checkinteger(L, 3);
    lua_pushinteger(L, sdblk_read_blocks(block_id, buf, count));
    return 1;
}

/* sdblk.write_block(block_id, buf_userdata) → int */
static int lua_sdblk_write_block(lua_State *L)
{
    uint32_t    block_id = (uint32_t)luaL_checkinteger(L, 1);
    const void *buf      = lua_touserdata(L, 2);
    lua_pushinteger(L, sdblk_write_block(block_id, buf));
    return 1;
}

/* sdblk.capacity_bytes() → integer */
static int lua_sdblk_capacity_bytes(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)sdblk_capacity_bytes());
    return 1;
}

/* sdblk.capacity_blocks() → integer */
static int lua_sdblk_capacity_blocks(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)sdblk_capacity_blocks());
    return 1;
}

static const luaL_Reg lua_drv_sdblk[] = {
    { "init",            lua_sdblk_init            },
    { "read_blocks",     lua_sdblk_read_blocks     },
    { "write_block",     lua_sdblk_write_block     },
    { "capacity_bytes",  lua_sdblk_capacity_bytes  },
    { "capacity_blocks", lua_sdblk_capacity_blocks },
    { NULL,              NULL                      }
};

int luaopen_sdblk(lua_State *L)
{
    luaL_newlib(L, lua_drv_sdblk);
    return 1;
}
#endif /* DRIVER_SDBLK_SG2002 */


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
#if DRIVER_GIC_V3
    luaL_requiref(L, "gicv3",    luaopen_gicv3,    1); lua_pop(L, 1);
#endif
#if DRIVER_UART_PL011
    luaL_requiref(L, "pl011",    luaopen_pl011,    1); lua_pop(L, 1);
#endif
#if DRIVER_UART_DW
    luaL_requiref(L, "dw_uart",  luaopen_dw_uart,  1); lua_pop(L, 1);
#endif
    luaL_requiref(L, "timer",    luaopen_timer,    1); lua_pop(L, 1);
#if DRIVER_NPU_RKNPU
    luaL_requiref(L, "rknpu",    luaopen_rknpu,    1); lua_pop(L, 1);
#endif
#if DRIVER_USB_DWC3
    luaL_requiref(L, "dwc3",     luaopen_dwc3,     1); lua_pop(L, 1);
#endif
#if DRIVER_TPU_CVITPU
    luaL_requiref(L, "cvi_tpu", luaopen_cvi_tpu, 1); lua_pop(L, 1);
#endif
#if DRIVER_ION
    luaL_requiref(L, "ion",     luaopen_ion,     1); lua_pop(L, 1);
#endif
#if DRIVER_SDBLK_SG2002
    luaL_requiref(L, "sdblk",  luaopen_sdblk,  1); lua_pop(L, 1);
#endif
}
