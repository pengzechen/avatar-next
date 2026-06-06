/*
 * lib/lua54_port/platform.c — Lua runtime platform glue for Avatar kernel
 *
 * Provides:
 *   - Static heap allocator (256 KB BSS) for the Lua VM
 *   - stdio wrappers (printf → kprintf, snprintf → my_vsnprintf)
 *   - malloc/free/realloc wrappers
 *   - lua_platform_open / lua_run_phase / lua_platform_close
 *
 * MUST be compiled with LUA_CFLAGS (FP enabled, compat headers in path).
 */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "klog.h"     /* kprintf, kvprintf  */
#include "types.h"    /* uint8_t, size_t    */
#include "string.h"   /* memset, memcpy     */

/* ── Forward declarations ────────────────────────────────────────────────── */
extern int  my_vsnprintf(char *buf, int size, const char *fmt, va_list va);
extern void lua_register_all_drivers(lua_State *L);
extern void luaL_openlibs_kernel(lua_State *L);

/* ── stdio wrappers ──────────────────────────────────────────────────────── */

/*
 * These provide the symbols that Lua source files reference via our
 * compat stdio.h declarations.  The linker resolves them here.
 */

int printf(const char *fmt, ...)
{
    int ret;
    va_list ap;
    va_start(ap, fmt);
    ret = kvprintf(fmt, ap);
    va_end(ap);
    return ret;
}

int fprintf(void *f, const char *fmt, ...)
{
    int ret;
    va_list ap;
    (void)f;
    va_start(ap, fmt);
    ret = kvprintf(fmt, ap);
    va_end(ap);
    return ret;
}

int vfprintf(void *f, const char *fmt, va_list ap)
{
    (void)f;
    return kvprintf(fmt, ap);
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    int ret;
    va_list ap;
    va_start(ap, fmt);
    ret = my_vsnprintf(buf, (int)n, fmt, ap);
    va_end(ap);
    return ret;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return my_vsnprintf(buf, (int)n, fmt, ap);
}

int fflush(void *f)
{
    (void)f;
    return 0;
}

/* ── Static heap (256 KB) ────────────────────────────────────────────────── */

#define LUA_HEAP_SIZE  (256 * 1024)

static uint8_t  lua_heap_mem[LUA_HEAP_SIZE] __attribute__((aligned(16)));
static int      lua_heap_inited = 0;

/*
 * Simple first-fit block allocator.
 *
 * Block header: [size: size_t][free: size_t]  (two words, 16-byte aligned)
 * 'free' == 1 when block is available; 0 when in use.
 * 'size' is the usable payload size in bytes.
 */
#define HDR_SIZE  (2 * sizeof(size_t))

static void lua_heap_init(void)
{
    size_t *hdr = (size_t *)(void *)lua_heap_mem;
    hdr[0] = LUA_HEAP_SIZE - HDR_SIZE;   /* usable size */
    hdr[1] = 1;                          /* free        */
    lua_heap_inited = 1;
}

void *malloc(size_t size)
{
    uint8_t *p, *end;
    size_t  *hdr;
    size_t   align_size;

    if (!lua_heap_inited) lua_heap_init();
    if (size == 0) return (void *)0;

    /* Align to 16 bytes */
    align_size = (size + 15) & ~(size_t)15;

    p   = lua_heap_mem;
    end = lua_heap_mem + LUA_HEAP_SIZE;

    while (p + HDR_SIZE <= end) {
        hdr = (size_t *)(void *)p;
        if (hdr[1] == 1 && hdr[0] >= align_size) {
            /* Split if leftover is large enough for a new block */
            if (hdr[0] >= align_size + HDR_SIZE + 16) {
                size_t *next = (size_t *)(void *)(p + HDR_SIZE + align_size);
                next[0] = hdr[0] - align_size - HDR_SIZE;
                next[1] = 1;
                hdr[0]  = align_size;
            }
            hdr[1] = 0;   /* mark in-use */
            return (void *)(p + HDR_SIZE);
        }
        p += HDR_SIZE + hdr[0];
    }
    kprintf("lua_malloc: out of heap (requested %u)\n", (unsigned)size);
    return (void *)0;
}

void free(void *ptr)
{
    uint8_t *p;
    size_t  *hdr;

    if (!ptr) return;
    p   = (uint8_t *)ptr - HDR_SIZE;
    hdr = (size_t *)(void *)p;
    hdr[1] = 1;   /* mark free */

    /* Coalesce with next block if it is also free */
    {
        uint8_t *next_p   = p + HDR_SIZE + hdr[0];
        size_t  *next_hdr = (size_t *)(void *)next_p;
        uint8_t *end      = lua_heap_mem + LUA_HEAP_SIZE;
        if (next_p + HDR_SIZE <= end && next_hdr[1] == 1) {
            hdr[0] += HDR_SIZE + next_hdr[0];
        }
    }
}

void *realloc(void *ptr, size_t new_size)
{
    size_t *hdr;
    void   *new_ptr;

    if (!ptr)      return malloc(new_size);
    if (!new_size) { free(ptr); return (void *)0; }

    hdr = (size_t *)((uint8_t *)ptr - HDR_SIZE);
    if (hdr[0] >= new_size) return ptr;   /* fits in existing block */

    new_ptr = malloc(new_size);
    if (!new_ptr) return (void *)0;
    memcpy(new_ptr, ptr, hdr[0] < new_size ? hdr[0] : new_size);
    free(ptr);
    return new_ptr;
}

/* ── Lua allocator callback ──────────────────────────────────────────────── */

static void *lua_pmm_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud; (void)osize;
    if (nsize == 0) {
        free(ptr);
        return (void *)0;
    }
    if (!ptr) return malloc(nsize);
    return realloc(ptr, nsize);
}

/* ── register_device Lua function ────────────────────────────────────────── */

/*
 * Lua: register_device(name, device_table)
 *
 * Appends the table to the global _devices array after setting
 * device_table.name = name.
 */
static int lua_fn_register_device(lua_State *L)
{
    const char *name;
    int         top;

    /* Expect: register_device(name, table) */
    luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    name = lua_tostring(L, 1);

    /* device_table.name = name */
    lua_pushstring(L, name);
    lua_setfield(L, 2, "name");

    /* _devices[#_devices+1] = device_table */
    lua_getglobal(L, "_devices");   /* stack: name, tbl, _devices */
    top = (int)lua_rawlen(L, -1);
    lua_pushvalue(L, 2);            /* push device_table */
    lua_rawseti(L, -2, top + 1);    /* _devices[top+1] = device_table */
    lua_pop(L, 1);                  /* pop _devices */

    return 0;
}

/* ── lua_run_phase ───────────────────────────────────────────────────────── */

void lua_run_phase(lua_State *L, const char *phase)
{
    int      i, n;
    int      top_save;

    if (!L || !phase) return;
    top_save = lua_gettop(L);

    lua_getglobal(L, "_devices");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    n = (int)lua_rawlen(L, -1);
    for (i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);          /* push _devices[i]          */
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_getfield(L, -1, phase);     /* push _devices[i][phase]   */
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);              /* pop nil + device table    */
            continue;
        }
        /* Call: device[phase](device)  — pass device table as self */
        lua_pushvalue(L, -2);           /* push device table as arg  */
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            kprintf("lua_run_phase[%s] error: %s\n",
                    phase, lua_tostring(L, -1));
            lua_pop(L, 1);             /* pop error message          */
        }
        lua_pop(L, 1);                 /* pop device table           */
    }
    lua_pop(L, 1);                     /* pop _devices               */
    lua_settop(L, top_save);
}

/* ── lua_platform_open ───────────────────────────────────────────────────── */

/*
 * Weak fallback blob symbols — overridden by the real platform_lua_blob.o
 * when a new-style platform.conf platform is used. For legacy PLATFORM=qemu
 * (no blob), len = 0 causes lua_platform_open() to return NULL gracefully.
 */
__attribute__((weak)) const char        g_platform_lua_src[1]  = { '\0' };
__attribute__((weak)) const unsigned int g_platform_lua_src_len = 0;

lua_State *lua_platform_open(const char *src, unsigned int len)
{
    lua_State *L;

    /* No platform.lua available (legacy PLATFORM=qemu — no platform.conf) */
    if (!src || len == 0)
        return (lua_State *)0;

    if (!lua_heap_inited) lua_heap_init();

    L = lua_newstate(lua_pmm_alloc, (void *)0);
    if (!L) {
        kprintf("lua_platform_open: lua_newstate failed\n");
        return (lua_State *)0;
    }

    /* Open base + table libraries */
    luaL_openlibs_kernel(L);

    /* Create _devices = {} */
    lua_newtable(L);
    lua_setglobal(L, "_devices");

    /* Register register_device() as a global C function */
    lua_pushcfunction(L, lua_fn_register_device);
    lua_setglobal(L, "register_device");

    /* Register all driver modules */
    lua_register_all_drivers(L);

    /* Load and execute platform.lua source */
    if (src && len > 0) {
        int rc = luaL_loadbuffer(L, src, len, "platform.lua");
        if (rc != LUA_OK) {
            kprintf("lua_platform_open: load error: %s\n",
                    lua_tostring(L, -1));
            lua_close(L);
            return (lua_State *)0;
        }
        rc = lua_pcall(L, 0, 0, 0);
        if (rc != LUA_OK) {
            kprintf("lua_platform_open: exec error: %s\n",
                    lua_tostring(L, -1));
            lua_close(L);
            return (lua_State *)0;
        }
    }

    return L;
}

/* ── lua_platform_close ──────────────────────────────────────────────────── */

void lua_platform_close(lua_State *L)
{
    if (L) lua_close(L);
}

/* ── lua_selftest ────────────────────────────────────────────────────────── */

int lua_selftest(lua_State *L)
{
    static const char demo[] =
        "print('[lua] =========================================')\n"
        "print('[lua] Lua 5.4 VM self-test')\n"
        "print('[lua] =========================================')\n"
        "\n"
        "-- 1. 基本算术\n"
        "local a, b = 6, 7\n"
        "assert(a * b == 42, 'arithmetic failed')\n"
        "print('[lua] 1. arithmetic: 6 * 7 = ' .. tostring(a * b) .. '  OK')\n"
        "\n"
        "-- 2. 字符串操作\n"
        "local s = 'Avatar' .. ' ' .. 'OS'\n"
        "assert(#s == 9, 'string concat len failed')\n"
        "assert(string.upper(s) == 'AVATAR OS', 'string.upper failed')\n"
        "print('[lua] 2. string: \"' .. s .. '\"  upper=\"' .. string.upper(s) .. '\"  OK')\n"
        "\n"
        "-- 3. 表 + ipairs\n"
        "local fruits = {'apple', 'banana', 'cherry'}\n"
        "local out = ''\n"
        "for i, v in ipairs(fruits) do\n"
        "    out = out .. i .. ':' .. v .. ' '\n"
        "end\n"
        "print('[lua] 3. table ipairs: ' .. out .. ' OK')\n"
        "\n"
        "-- 4. 闭包 / 递归\n"
        "local function fib(n)\n"
        "    if n <= 1 then return n end\n"
        "    return fib(n-1) + fib(n-2)\n"
        "end\n"
        "assert(fib(10) == 55, 'fibonacci failed')\n"
        "print('[lua] 4. fibonacci(10) = ' .. tostring(fib(10)) .. '  OK')\n"
        "\n"
        "-- 5. type() / tostring()\n"
        "assert(type(42)      == 'number',  'type number')\n"
        "assert(type('hi')    == 'string',  'type string')\n"
        "assert(type({})      == 'table',   'type table')\n"
        "assert(type(print)   == 'function','type function')\n"
        "print('[lua] 5. type() checks  OK')\n"
        "\n"
        "-- 6. pcall 错误捕获\n"
        "local ok, err = pcall(function() error('test error') end)\n"
        "assert(not ok, 'pcall should catch error')\n"
        "print('[lua] 6. pcall/error:  caught=\"' .. tostring(err) .. '\"  OK')\n"
        "\n"
        "print('[lua] =========================================')\n"
        "print('[lua] ALL TESTS PASSED')\n"
        "print('[lua] =========================================')\n";

    int rc;

    if (!L) return -1;

    rc = luaL_loadbuffer(L, demo, sizeof(demo) - 1, "selftest");
    if (rc != LUA_OK) {
        kprintf("[lua] selftest load error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    rc = lua_pcall(L, 0, 0, 0);
    if (rc != LUA_OK) {
        kprintf("[lua] selftest runtime error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}
