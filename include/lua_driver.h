/*
 * include/lua_driver.h — Lua platform initialization API
 *
 * Called from kernel_main to run platform.lua phases.
 */
#ifndef LUA_DRIVER_H
#define LUA_DRIVER_H

/* Forward-declare lua_State to avoid pulling in Lua headers everywhere */
struct lua_State;
typedef struct lua_State lua_State;

/**
 * lua_platform_open() — Create and initialize a Lua VM loaded with platform.lua
 *
 * @src    Null-terminated platform.lua source (embedded as byte array)
 * @len    Length of src in bytes (not counting null terminator)
 * @return Initialized lua_State, or NULL on failure (will kprintf the error)
 */
lua_State *lua_platform_open(const char *src, unsigned int len);

/**
 * lua_run_phase() — Execute a named initialization phase
 *
 * Iterates registered devices and calls device[phase](device) for each device
 * that has a function for this phase.
 *
 * @L      Lua state returned by lua_platform_open()
 * @phase  Phase name: "earlycon", "irqcore", "drivers", "fs", "late"
 */
void lua_run_phase(lua_State *L, const char *phase);

/**
 * lua_platform_close() — Destroy the Lua VM
 *
 * @L  Lua state to close
 */
void lua_platform_close(lua_State *L);

/**
 * lua_selftest() — Run a small Lua self-test and print results via kprintf.
 *
 * Verifies arithmetic, strings, tables and loops work correctly.
 * Returns 0 on success, -1 if any Lua error occurred.
 *
 * @L  Lua state returned by lua_platform_open()
 */
int lua_selftest(lua_State *L);

#endif /* LUA_DRIVER_H */
