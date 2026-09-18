#ifndef KLOG_H
#define KLOG_H

#include "types.h"
#include "barrier.h"
#include "arg.h"

/*
 * 内核日志系统
 * 支持全局日志级别和模块级别的日志控制
 *
 * 两种控制手段，配合使用：
 *   1. 级别（编译期 Makefile 的 LOG=，运行期 set_log_level()）
 *      —— 决定 DEBUG/TRACE 这些宏体是否存在、初始门槛多高；
 *   2. 模块掩码（编译期 Makefile 的 LOG_MODULES=，运行期 log_set_modules_by_name()）
 *      —— 在 DEBUG/TRACE 级别上再筛一层，只放行你关心的模块。
 *
 * 典型用法：定位 GIC 相关问题时
 *     make PLATFORM=qemu-virt-aarch64 run LOG=debug LOG_MODULES=gic
 * 这样只有打了 GIC 模块标签的日志会输出。
 */

/*
 * LOG_LEVEL 必须由构建系统给出（Makefile 的 LOG=<level>）。
 * 少了它，下面 "关闭日志" 的 #if 会因为 LOG_LEVEL 与枚举值都按 0 参与
 * 预处理判断而**静默**把整个翻译单元的日志（包括 KLOG_ERROR）全部抹掉 ——
 * 曾经就是这样：手工编译某个文件时日志集体失踪，且没有任何提示。
 */
#ifndef LOG_LEVEL
#  error "LOG_LEVEL 未定义：请用 Makefile 的 LOG=<none|error|warn|info|debug|trace> 构建（见 make help）"
#endif

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
#define LOG_MODULE_GIC     (1ULL << 9)  /* 中断控制器（gicv2/gicv3）*/
#define LOG_MODULE_GENERIC (1ULL << 10) /* 未打模块标签的 KLOG_DEBUG/KLOG_TRACE */

extern uint64_t g_log_module_mask;

/*
 * 模块名 → 位 的映射表在 lib/klog.c（加新模块时两处一起改）。
 * 名字用逗号分隔，接受 all / none：
 *     log_set_modules_by_name("uart,gic");
 * 返回无法识别的名字个数（0 = 全部识别）。
 */
extern int log_set_modules_by_name(const char *names);

/* 把当前启用的模块名写进 out（逗号分隔），返回写入长度 */
extern int log_modules_to_string(char *out, int len);

/*
 * 应用编译期模块白名单（Makefile 的 LOG_MODULES=，经 -DLOG_MODULES_DEFAULT）
 * 并打印一行生效配置。未定义该宏时不改掩码（保持全开）。
 * 在 kernel_main 里、UART 可用之后尽早调用。
 */
extern void klog_init(void);

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

/*
 * 当前逻辑 CPU 号；由 kernel/task/cpu.c 实现（weak 默认返回 0，便于
 * 单元测试 / 早期 boot 在 BSP 未注册 TPIDR_EL1 前也能调用 klog）。
 */
extern uint32_t klog_cpu_id(void);

/* 彩色日志输出 */
#define KLOG_COLOR_NONE   ""
#define KLOG_COLOR_RED    "\x1b[31m"
#define KLOG_COLOR_GREEN  "\x1b[32m"
#define KLOG_COLOR_YELLOW "\x1b[33m"
#define KLOG_COLOR_BLUE   "\x1b[34m"
#define KLOG_COLOR_GRAY   "\x1b[90m"
#define KLOG_COLOR_RESET  "\x1b[0m"

/* 不同级别的日志宏 */

/* ERROR 日志 - 总是显示（LOG=none 构建下整个宏被替换为空，见文件末尾） */
#define KLOG_ERROR(fmt, ...) \
    do { \
        kprintf(KLOG_COLOR_RED "[ERROR][C%u] " "%s:%d: " fmt \
               KLOG_COLOR_RESET "", \
               klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)

/* WARN 日志 - 在 WARN 级别及以上显示 */
#define KLOG_WARN(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_WARN) { \
            kprintf(KLOG_COLOR_YELLOW "[WARN][C%u] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "", \
                   klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/* INFO 日志 - 在 INFO 级别及以上显示 */
#define KLOG_INFO(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_INFO) { \
            kprintf(KLOG_COLOR_GREEN "[INFO][C%u] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "", \
                   klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/* DEBUG 日志 - 在 DEBUG 级别及以上、且放行 generic 模块时显示
 *
 * 为什么要看模块位：Makefile 里一旦给了 LOG_MODULES=，掩码就是**白名单**，
 * 未打模块标签的 DEBUG/TRACE 归入 LOG_MODULE_GENERIC。默认（不传
 * LOG_MODULES）掩码是全 1，所以旧行为不变。 */
#define KLOG_DEBUG(fmt, ...) \
    do { \
        if ((g_log_level >= LOG_LEVEL_DEBUG) && \
            log_is_module_enabled(LOG_MODULE_GENERIC)) { \
            kprintf(KLOG_COLOR_BLUE "[DEBUG][C%u] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "", \
                   klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/* TRACE 日志 - 只在 TRACE 级别、且放行 generic 模块时显示 */
#define KLOG_TRACE(fmt, ...) \
    do { \
        if ((g_log_level >= LOG_LEVEL_TRACE) && \
            log_is_module_enabled(LOG_MODULE_GENERIC)) { \
            kprintf(KLOG_COLOR_GRAY "[TRACE][C%u] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "", \
                   klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/* ===== 模块级调试日志 ===== */

/*
 * 模块日志 - 只在 DEBUG/TRACE 级别且指定模块启用时显示。
 *
 * 注意：所有日志宏都**不**自动补换行，调用方自己写 "\n"（与
 * KLOG_ERROR/WARN/INFO 一致）。早先这几个宏偷偷补 "\n"，而调用方也写了
 * "\n"，于是每行多一个空行 —— 已统一。
 */
#define KLOG_MODULE_DEBUG(module, fmt, ...) \
    do { \
        if ((g_log_level >= LOG_LEVEL_DEBUG) && log_is_module_enabled(module)) { \
            kprintf(KLOG_COLOR_BLUE "[DEBUG][C%u] [MOD] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "", \
                   klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define KLOG_MODULE_TRACE(module, fmt, ...) \
    do { \
        if ((g_log_level >= LOG_LEVEL_TRACE) && log_is_module_enabled(module)) { \
            kprintf(KLOG_COLOR_GRAY "[TRACE][C%u] [MOD] " "%s:%d: " fmt \
                   KLOG_COLOR_RESET "", \
                   klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
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

/*
 * 只有 LOG=none 是**编译期**抹除（宏体变 do{}while(0)，参数不求值、
 * 格式串不进 .rodata）。其余级别（error/warn/info/debug/trace）都只是
 * 把 g_log_level 的**初值**设成该级别，调用点仍然带着
 * "读 g_log_level + 比较 + 分支"，运行期 set_log_level() 还能打开更细的日志。
 * —— 这一点文档以前写反了，见 docs/basic/KLOG.md。
 *
 * LOG_LEVEL_NONE 是枚举常量不是宏，在 #if 里会被预处理成 0；上面的
 * #ifndef LOG_LEVEL 已经挡住了 "未定义 → 0 == 0 → 全静音" 那个坑。
 */
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
