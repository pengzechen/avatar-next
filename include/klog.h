#ifndef KLOG_H
#define KLOG_H

#include "types.h"
#include "barrier.h"
#include "arg.h"

/*
 * 内核日志系统
 * 支持全局日志级别和模块级别的日志控制
 */

/* ===== 日志等级定义 ===== */

typedef enum {
    LOG_LEVEL_NONE = 0,  /* 关闭所有日志 */
    LOG_LEVEL_ERROR,      /* 只显示错误 */
    LOG_LEVEL_WARN,       /* 显示警告和错误 */
    LOG_LEVEL_INFO,       /* 显示信息、警告和错误 */
    LOG_LEVEL_DEBUG,      /* 显示调试信息及以上 */
    LOG_LEVEL_TRACE,      /* 显示所有日志（包括跟踪） */
} log_level_t;

/* ===== 全局日志级别 ===== */

extern log_level_t g_log_level;

static inline void
set_log_level(log_level_t level)
{
    g_log_level = level;
}

static inline log_level_t
get_log_level(void)
{
    return g_log_level;
}

/* ===== 模块日志开关 ===== */

/*
 * 模块位定义
 * 每个模块占用 1 位，最多支持 64 个模块
 */
#define LOG_MODULE_INIT    (1ULL << 0)  /* 初始化模块 */
#define LOG_MODULE_TASK    (1ULL << 1)  /* 任务调度模块 */
#define LOG_MODULE_DRIVER  (1ULL << 2)  /* 驱动模块 */
#define LOG_MODULE_UART    (1ULL << 3)  /* UART 驱动 */
#define LOG_MODULE_TIMER   (1ULL << 4)  /* 定时器模块 */
#define LOG_MODULE_MM      (1ULL << 5)  /* 内存管理模块 */
#define LOG_MODULE_FS      (1ULL << 6)  /* 文件系统模块 */
#define LOG_MODULE_NET     (1ULL << 7)  /* 网络模块 */
#define LOG_MODULE_SMP     (1ULL << 8)  /* 多核模块 */

extern uint64_t g_log_module_mask;

static inline void
log_set_modules(uint64_t mask)
{
    g_log_module_mask = mask;
}

static inline void
log_add_module(uint64_t module)
{
    g_log_module_mask |= module;
}

static inline void
log_remove_module(uint64_t module)
{
    g_log_module_mask &= ~module;
}

static inline int
log_is_module_enabled(uint64_t module)
{
    return (g_log_module_mask & module) != 0;
}

/* ===== 日志宏定义 ===== */

/* 核心日志函数（由 klog.c 实现） */
extern void klog_putchar(char c);
extern void klog_flush(void);
extern int  kvprintf(const char *fmt, va_list va);
extern int  kprintf(const char *fmt, ...);

/**
 * klog_get_cpu_id - 返回当前核编号（static inline，零函数调用开销）
 *
 * AArch64: 读 MPIDR_EL1 Aff0 字段，启动即可用，无需 TPIDR 初始化。
 * RISC-V:  读 tp 寄存器（boot 中设置为 &g_cpus[cpu_id]）首字段 cpu_id。
 * x86_64:  读 APIC ID via CPUID。
 * 注意：必须是 static inline，不能有 BL，否则在切换栈的函数
 *       （如 task_switch_to_idle_stack）里会破坏 LR / x30，导致
 *       ret 跳到 0x0（EC=0x0 ELR=0x0 异常）。
 */
static inline uint32_t
klog_get_cpu_id(void)
{
#if defined(__aarch64__)
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xffU);  /* Aff0 = 逻辑 CPU 编号 */
#elif defined(__riscv)
    uint64_t tp;
    __asm__ volatile("mv %0, tp" : "=r"(tp));
    if (tp == 0) return 0;
    return *(volatile uint32_t *)(uintptr_t)tp;  /* cpu_t::cpu_id at offset 0 */
#elif defined(__x86_64__)
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    return (ebx >> 24) & 0xffU;  /* Initial APIC ID */
#else
    return 0;
#endif
}

/* 彩色日志输出 */
#define KLOG_COLOR_NONE   ""
#define KLOG_COLOR_RED    "\x1b[31m"
#define KLOG_COLOR_GREEN  "\x1b[32m"
#define KLOG_COLOR_YELLOW "\x1b[33m"
#define KLOG_COLOR_BLUE   "\x1b[34m"
#define KLOG_COLOR_RESET  "\x1b[0m"

/* 不同级别的日志宏 */

#define LOG_OUTPUT_NONE ""
#define LOG_PREFIX "[%-5s] "

/* ERROR 日志 - 总是显示 */
#define KLOG_ERROR(fmt, ...) \
    do { \
        kprintf(KLOG_COLOR_RED "[ERROR/c%u] " "%s:%d: " fmt \
               KLOG_COLOR_RESET "", \
               klog_get_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)

/* WARN 日志 - 在 WARN 级别及以上显示 */
#define KLOG_WARN(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_WARN) { \
            kprintf(KLOG_COLOR_YELLOW "[WARN/c%u] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "", \
                   klog_get_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/* INFO 日志 - 在 INFO 级别及以上显示 */
#define KLOG_INFO(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_INFO) { \
            kprintf(KLOG_COLOR_GREEN "[INFO/c%u] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "", \
                   klog_get_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/* DEBUG 日志 - 在 DEBUG 级别及以上显示 */
#define KLOG_DEBUG(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_DEBUG) { \
            kprintf(KLOG_COLOR_BLUE "[DEBUG/c%u] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "", \
                   klog_get_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/* TRACE 日志 - 只在 TRACE 级别显示 */
#define KLOG_TRACE(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_TRACE) { \
            kprintf("[TRACE] " "%s:%d: " fmt "\n", \
                   __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/* ===== 模块级调试日志 ===== */

/* 模块日志 - 只在 DEBUG/TRACE 级别且指定模块启用时显示 */
#define KLOG_MODULE_DEBUG(module, fmt, ...) \
    do { \
        if ((g_log_level >= LOG_LEVEL_DEBUG) && log_is_module_enabled(module)) { \
            kprintf(KLOG_COLOR_BLUE "[DEBUG] [MOD] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "\n", \
                   __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define KLOG_MODULE_TRACE(module, fmt, ...) \
    do { \
        if ((g_log_level >= LOG_LEVEL_TRACE) && log_is_module_enabled(module)) { \
            kprintf("[TRACE] [MOD] " "%s:%d: " fmt "\n", \
                   __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/* ===== 简化的模块日志宏 ===== */

#define KLOG_INIT(fmt, ...)    KLOG_MODULE_DEBUG(LOG_MODULE_INIT, fmt, ##__VA_ARGS__)
#define KLOG_TASK(fmt, ...)    KLOG_MODULE_DEBUG(LOG_MODULE_TASK, fmt, ##__VA_ARGS__)
#define KLOG_DRIVER(fmt, ...)  KLOG_MODULE_DEBUG(LOG_MODULE_DRIVER, fmt, ##__VA_ARGS__)
#define KLOG_UART(fmt, ...)    KLOG_MODULE_DEBUG(LOG_MODULE_UART, fmt, ##__VA_ARGS__)
#define KLOG_TIMER(fmt, ...)   KLOG_MODULE_DEBUG(LOG_MODULE_TIMER, fmt, ##__VA_ARGS__)
#define KLOG_MM(fmt, ...)      KLOG_MODULE_DEBUG(LOG_MODULE_MM, fmt, ##__VA_ARGS__)
#define KLOG_FS(fmt, ...)      KLOG_MODULE_DEBUG(LOG_MODULE_FS, fmt, ##__VA_ARGS__)
#define KLOG_NET(fmt, ...)     KLOG_MODULE_DEBUG(LOG_MODULE_NET, fmt, ##__VA_ARGS__)
#define KLOG_SMP(fmt, ...)     KLOG_MODULE_DEBUG(LOG_MODULE_SMP, fmt, ##__VA_ARGS__)

/* ===== 兼容性宏 ===== */

#define pr_err KLOG_ERROR
#define pr_warn KLOG_WARN
#define pr_info KLOG_INFO
#define pr_debug KLOG_DEBUG

#define printk kprintf

/* ===== 条件编译优化 ===== */

/* 调试：检查 LOG_LEVEL 的值 */
#ifdef KLOG_DEBUG_BUILD
    #if LOG_LEVEL == LOG_LEVEL_NONE
        #pragma message "LOG_LEVEL is NONE"
    #elif LOG_LEVEL == LOG_LEVEL_ERROR
        #pragma message "LOG_LEVEL is ERROR"
    #elif LOG_LEVEL == LOG_LEVEL_WARN
        #pragma message "LOG_LEVEL is WARN"
    #elif LOG_LEVEL == LOG_LEVEL_INFO
        #pragma message "LOG_LEVEL is INFO"
    #elif LOG_LEVEL == LOG_LEVEL_DEBUG
        #pragma message "LOG_LEVEL is DEBUG"
    #elif LOG_LEVEL == LOG_LEVEL_TRACE
        #pragma message "LOG_LEVEL is TRACE"
    #else
        #pragma message "LOG_LEVEL is unknown"
    #endif
#endif

/* 如果日志级别为 NONE，完全禁用日志（减少代码体积） */
#if defined(LOG_NONE) || (LOG_LEVEL == LOG_LEVEL_NONE)
    #undef KLOG_ERROR
    #define KLOG_ERROR(fmt, ...) do {} while (0)

    #undef KLOG_WARN
    #define KLOG_WARN(fmt, ...) do {} while (0)

    #undef KLOG_INFO
    #define KLOG_INFO(fmt, ...) do {} while (0)

    #undef KLOG_DEBUG
    #define KLOG_DEBUG(fmt, ...) do {} while (0)

    #undef KLOG_TRACE
    #define KLOG_TRACE(fmt, ...) do {} while (0)

    #undef KLOG_MODULE_DEBUG
    #define KLOG_MODULE_DEBUG(module, fmt, ...) do {} while (0)

    #undef KLOG_MODULE_TRACE
    #define KLOG_MODULE_TRACE(module, fmt, ...) do {} while (0)
#endif

#endif /* KLOG_H */
