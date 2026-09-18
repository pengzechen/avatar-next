/*
 * include/platform_cfg.h — 运行时平台配置
 *
 * 替代 device_profile.h / mem_layout.h / pmm_reserve.h / driver_cfg.h。
 * 所有值由构建期生成的 platform_static.c 提供，在 PMM 初始化之前即可调用。
 *
 * 使用方式：
 *   #include "platform_cfg.h"   (替代 driver_cfg.h / device_profile.h)
 */

#ifndef PLATFORM_CFG_H
#define PLATFORM_CFG_H

#include "types.h"

/* ── 内存布局 ─────────────────────────────────────────────────────────── */

extern uintptr_t g_mem_ram_base;
extern uintptr_t g_mem_ram_size;
extern uintptr_t g_mem_rootfs_base;
extern uintptr_t g_mem_rootfs_size;

/* MMIO 是否需要加 KERNEL_VMA 偏移（aarch64=1, riscv64/x86_64=0） */
extern int g_mmio_needs_vma;

/* ── PMM 保留区 ──────────────────────────────────────────────────────── */

#define PMM_MAX_RESV 4

typedef struct {
    char     tag[32];
    uint64_t start;
    uint64_t end;
} pmm_resv_t;

typedef struct {
    const char *block;
    const char *key;
    uintptr_t   value;
} platform_kv_t;

typedef struct {
    const char *tag;
    uint64_t    start;
    uint64_t    end;
} platform_reserve_t;

extern const platform_kv_t      g_platform_static_kv[];
extern const unsigned           g_platform_static_kv_count;
extern const platform_reserve_t g_platform_static_reserves[];
extern const unsigned           g_platform_static_reserve_count;

extern pmm_resv_t g_pmm_reserves[PMM_MAX_RESV];
extern int        g_pmm_resv_count;

/* ── 初始化函数 ──────────────────────────────────────────────────────── */

/**
 * platform_conf_scan - 从静态平台配置提取内存布局和 PMM 保留区。
 * 在 MMU 初始化之前即可调用，不需要堆。
 */
void platform_conf_scan(void);
void platform_conf_close(void);

/**
 * platform_get_uintptr / platform_get_uint
 * 通用字段查询：读取构建期生成的 platform.<block>.<key> 值。
 * 需在 platform_conf_scan() 之后调用。
 *
 * 示例：
 *   uintptr_t base = platform_get_uintptr("uart", "base");
 *   unsigned  irq  = platform_get_uint("irq", "gicd");
 */
uintptr_t platform_get_uintptr(const char *block, const char *key);
unsigned  platform_get_uint   (const char *block, const char *key);

/**
 * platform_get_mmio - 返回 MMIO 物理地址（mmio_vma=true 平台自动加 KERNEL_VMA）。
 * 在驱动 init() 中调用一次，将结果保存到模块内变量。
 */
uintptr_t platform_get_mmio(const char *block, const char *key);

/* ── logger_* 兼容宏（与旧 driver_cfg.h 保持兼容）─────────────────────── */

#include "klog.h"
#define logger_error(...)     KLOG_ERROR(__VA_ARGS__)
#define logger_warn(...)      KLOG_WARN(__VA_ARGS__)
#define logger_info(...)      KLOG_INFO(__VA_ARGS__)
#define logger_debug(...)     KLOG_DEBUG(__VA_ARGS__)
#define logger_trace(...)     KLOG_TRACE(__VA_ARGS__)

/*
 * GIC 专用的调试日志：走模块掩码的 GIC 位，而不是普通 KLOG_DEBUG。
 * 这样 LOG=debug 时可以只开它：
 *     make PLATFORM=... run LOG=debug LOG_MODULES=gic
 * 以前它只是 KLOG_DEBUG 的别名，名字里的 "gic" 没有任何过滤作用。
 */
#define logger_gic_debug(...) KLOG_MODULE_DEBUG(LOG_MODULE_GIC, __VA_ARGS__)

#endif /* PLATFORM_CFG_H */
