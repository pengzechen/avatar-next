/*
 * lib/lua54_port/lua_kernel_init.c — Open only base + table Lua libraries
 *
 * Replaces linit.c (do NOT compile linit.c).
 * Compiled with LUA_CFLAGS so Lua headers are accessible.
 */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/**
 * luaL_openlibs_kernel() — Open the minimal library set needed by platform.lua
 */
void luaL_openlibs_kernel(lua_State *L)
{
    static const luaL_Reg kernel_libs[] = {
        /* base: print, error, pcall, xpcall, pairs, ipairs,
         *       type, tostring, tonumber, select, rawget/rawset, etc. */
        { "_G",              luaopen_base   },
        /* table: table.insert, table.remove, table.concat, ... */
        { LUA_TABLIBNAME,    luaopen_table  },
        /* string: string.upper, string.lower, string.format, string.find, ... */
        { LUA_STRLIBNAME,    luaopen_string },
        /* math: lmathlib depends on libc math functions (log10, sin, etc.).
         * Not available in freestanding — omitted. Basic Lua arithmetic
         * (+, -, *, /) works without lmathlib. */
        { NULL,              NULL           }
    };
    int i;
    for (i = 0; kernel_libs[i].func != NULL; i++) {
        luaL_requiref(L, kernel_libs[i].name, kernel_libs[i].func, 1);
        lua_pop(L, 1);
    }
}

/**
 * luaL_openlibs() — Compatibility stub (calls the kernel variant).
 * Satisfies the linker if anything references the standard name.
 */
void luaL_openlibs(lua_State *L)
{
    luaL_openlibs_kernel(L);
}
