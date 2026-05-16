/*
 * lib/platform_cfg.c — 平台运行时配置（Lua 5.4 引擎驱动）
 *
 * platform_conf_scan()：
 *   1. 调用 lua_platform_open() 启动 Lua VM，执行内嵌 platform.lua
 *   2. 通过 Lua API 访问 platform.memory.* / platform.mmio_vma，填充 C 全局变量
 *   3. Lua State 持续存活，供 platform_get_uintptr/uint/mmio() 后续查询
 *
 * platform_get_uintptr/uint/mmio()：
 *   直接查询 platform.<block>.<key>，支持完整 Lua 语法。
 *
 * 编译要求：LUA_CFLAGS（compat 头文件前置，FP 开启）。
 */

#include "platform_cfg.h"
#include "mm_vm.h"   /* KERNEL_VMA */

#include "lua.h"
#include "lauxlib.h"

/* ── 全局变量定义 ─────────────────────────────────────────────────────── */

uintptr_t g_mem_ram_base    = 0;
uintptr_t g_mem_ram_size    = 0;
uintptr_t g_mem_rootfs_base = 0;
uintptr_t g_mem_rootfs_size = 0;

int g_mmio_needs_vma = 0;

pmm_resv_t g_pmm_reserves[PMM_MAX_RESV];
int        g_pmm_resv_count = 0;

/* 内嵌 Lua 源码（由 gen_platform.py 生成的 platform_lua_blob.c 提供） */
extern const char         g_platform_lua_src[];
extern const unsigned int g_platform_lua_src_len;

/* lua_platform_open() 在 lua_platform.c（LUA_CFLAGS）中实现 */
extern lua_State *lua_platform_open(const char *src, unsigned int len);

/* 持久 Lua State（platform_conf_scan 后保持存活，直到主循环结束） */
static lua_State *g_L = NULL;

/* ── 内部助手 ─────────────────────────────────────────────────────────── */

/*
 * lua_tag_copy - 将 Lua 字符串复制到固定宽度的 tag 缓冲区。
 * src 可为 NULL（当 Lua 值不是字符串时）。
 */
static void lua_tag_copy(char *dst, const char *src, unsigned n)
{
    unsigned i;
    for (i = 0; i < n - 1 && src && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ── platform_conf_scan ──────────────────────────────────────────────── */

void platform_conf_scan(void)
{
    lua_State *L;
    int i, n;

    L = lua_platform_open(g_platform_lua_src, g_platform_lua_src_len);
    if (!L) return;
    g_L = L;

    /* ── platform.memory ──────────────────────────────────────────── */
    lua_getglobal(L, "platform");       /* [platform]                 */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    lua_getfield(L, -1, "memory");      /* [platform, memory]         */
    if (lua_istable(L, -1)) {

        /* memory.ram */
        lua_getfield(L, -1, "ram");     /* [platform, memory, ram]    */
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "base");
            g_mem_ram_base = (uintptr_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "size");
            g_mem_ram_size = (uintptr_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);                  /* pop ram                    */

        /* memory.rootfs */
        lua_getfield(L, -1, "rootfs");  /* [platform, memory, rootfs] */
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "base");
            g_mem_rootfs_base = (uintptr_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "size");
            g_mem_rootfs_size = (uintptr_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);                  /* pop rootfs                 */

        /* memory.reserves[] */
        lua_getfield(L, -1, "reserves"); /* [platform, memory, rsv]   */
        if (lua_istable(L, -1)) {
            n = (int)lua_rawlen(L, -1);
            if (n > PMM_MAX_RESV) n = PMM_MAX_RESV;
            g_pmm_resv_count = n;
            for (i = 1; i <= n; i++) {
                pmm_resv_t *r = &g_pmm_reserves[i - 1];
                lua_rawgeti(L, -1, i);      /* [.., rsv, rsv[i]]      */
                if (lua_istable(L, -1)) {
                    lua_getfield(L, -1, "name");
                    lua_tag_copy(r->tag, lua_tostring(L, -1), sizeof(r->tag));
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "start");
                    r->start = (uint64_t)lua_tointeger(L, -1);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "stop");
                    r->end = (uint64_t)lua_tointeger(L, -1);
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);              /* pop rsv[i]             */
            }
        }
        lua_pop(L, 1);                  /* pop reserves               */
    }
    lua_pop(L, 1);                      /* pop memory                 */

    /* ── platform.mmio_vma ────────────────────────────────────────── */
    lua_getfield(L, -1, "mmio_vma");    /* [platform, mmio_vma]       */
    if (lua_isboolean(L, -1))
        g_mmio_needs_vma = lua_toboolean(L, -1);
    lua_pop(L, 1);                      /* pop mmio_vma               */

    lua_pop(L, 1);                      /* pop platform               */
}

/* ── platform_lua_state ──────────────────────────────────────────────── */

lua_State *platform_lua_state(void)
{
    return g_L;
}

/* ── 通用字段查询 ────────────────────────────────────────────────────── */

uintptr_t platform_get_uintptr(const char *block, const char *key)
{
    uintptr_t val = 0;
    if (!g_L) return 0;
    lua_getglobal(g_L, "platform");       /* [platform]         */
    if (!lua_istable(g_L, -1)) { lua_pop(g_L, 1); return 0; }
    lua_getfield(g_L, -1, block);         /* [platform, block]  */
    if (!lua_istable(g_L, -1)) { lua_pop(g_L, 2); return 0; }
    lua_getfield(g_L, -1, key);           /* [platform, block, val] */
    val = (uintptr_t)lua_tointeger(g_L, -1);
    lua_pop(g_L, 3);
    return val;
}

unsigned platform_get_uint(const char *block, const char *key)
{
    return (unsigned)platform_get_uintptr(block, key);
}

/* ── platform_get_mmio ───────────────────────────────────────────────── */

uintptr_t platform_get_mmio(const char *block, const char *key)
{
    uintptr_t addr = platform_get_uintptr(block, key);
    if (addr == 0) return 0;
    return g_mmio_needs_vma ? addr + KERNEL_VMA : addr;
}
