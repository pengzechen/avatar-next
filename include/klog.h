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
#define LOG_MODULE_SYSCALL (1ULL << 11) /* 系统调用（strace 式逐次追踪）*/

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

/* ===== 采样判定（限流用） ===== */

/*
 * klog_sample_hit - 采样判定：前 FIRST 次全打，之后只打 2 的幂，CAP 之后静音。
 *
 * 这是三处**相同**形状的手写采样的提取，以后不要再各写一份：
 *     driver/irq/plic.c         的 plic_irq_log_sample()
 *     boot/riscv64/exception.c  的 rv_irq_log_sample()
 *     kernel/net/netdev.c       的内联 rx_count <= 16 || rx_count % 32 == 0
 *
 * 关键性质：单个调用点的输出**有上界**。FIRST=8 / CAP=4096 时最多
 * 8 + 9 = 17 行，无论事件实际发生多少次。中断风暴下这是唯一能保住串口的性质。
 *
 * CAP 之后彻底静音 —— 想知道"现在到底多少次了"，看调用点自己维护的计数器
 * （参照 driver/uart/uart_pl011.c 的 tx_dropped / pl011_tx_dropped()），
 * 而不是指望这里继续打。
 *
 * 为什么**不**做基于时钟的窗口限流：g_system_ticks 在 timer_init() 之前恒为 0，
 * 而 timer_init() 由 kernel/main.c 调用，**晚于** pmm/fs/vmm 初始化。窗口限流
 * 会认为"所有事件都发生在 tick 0"，打印一次后吞掉整个前 timer 启动阶段 ——
 * 恰好是最需要日志的那一段。所以只做计数采样。
 */
#ifndef KLOG_SAMPLE_FIRST
#define KLOG_SAMPLE_FIRST   8U
#endif
#ifndef KLOG_SAMPLE_CAP
#define KLOG_SAMPLE_CAP     4096U
#endif

static inline int
klog_sample_hit(uint32_t n)
{
    return (n <= KLOG_SAMPLE_FIRST) ||
           (n <= KLOG_SAMPLE_CAP && (n & (n - 1U)) == 0U);
}

/* ===== 日志宏定义 ===== */

/* 核心日志函数（由 klog.c 实现） */
extern void klog_putchar(char c);
extern void klog_flush(void);
extern int  kvprintf(const char *fmt, va_list va);
extern int  kprintf(const char *fmt, ...);

/*
 * klog_panic_begin - 进入崩溃路径，之后的日志不再取全局输出锁
 *
 * 崩溃可能发生在别的 CPU 正持 g_klog_lock 的时候，也可能发生在同一个
 * CPU 已经持锁的 klog 调用内部 —— 此时再取锁就是自旋到死，现场一个字都
 * 打不出来。幂等，单向（没有"退出 panic"）。
 *
 * 调用点：include/assert.h 的 assert/assert_always、platform_panic()。
 * 详见 kernel/debug/backtrace.c 与 docs/basic/BACKTRACE.md。
 */
extern void klog_panic_begin(void);

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
#define KLOG_GIC(fmt, ...)     KLOG_MODULE_DEBUG(LOG_MODULE_GIC, fmt, ##__VA_ARGS__)
#define KLOG_SYSCALL(fmt, ...) KLOG_MODULE_DEBUG(LOG_MODULE_SYSCALL, fmt, ##__VA_ARGS__)

/* ===== 一次性 / 采样日志 =====
 *
 * 用于两类场合，判断依据见 docs/basic/KLOG.md「该用哪个级别」：
 *   _ONCE   —— 同一调用点一辈子只该说一次（缺哪个系统调用、连自己都不认识的条件）
 *   _SAMPLE —— 高频且持续（ISR、每包、每目录项），前几次全打、之后按 2 的幂抽稀
 *
 * 状态是**块作用域 static**，每个宏展开点各自一份，落在 .bss，靠内核启动时的
 * clear_bss（boot/<arch>/boot.S，在任何 C 代码之前）保证初值为 0。
 * 无分配、无锁；SMP 安全靠 __atomic_*（RELAXED 足够 —— 顺序由下游
 * g_klog_lock 的 acquire/release 保证）。不打印时参数**不求值**，与其它宏一致。
 *
 * 两条放置约束：
 *   1. **不要放在被多个 TU include 的头文件的函数里** —— 每个 TU 各拿一份 static，
 *      _ONCE 会变成"每 TU 一次"，_SAMPLE 的预算成倍。
 *   2. riscv64 的 `.bss.boot` 不在 __bss_start/__bss_end 内、**不被清零**
 *      （boot/riscv64/link.ld），所以这些宏不得用于 .text.boot/.bss.boot 里的代码。
 *
 * SAMPLE 宏在格式参数里可以用 `_klog_seq_` 取到当前命中序号，例如
 *     KLOG_WARN_SAMPLE("[plic] unhandled irq=%u hit#%u\n", irq, _klog_seq_);
 * 它**只在宏的参数表内有效**。
 */

#define KLOG__ONCE_TAKE(flag) \
    (__atomic_exchange_n(&(flag), 1U, __ATOMIC_RELAXED) == 0U)

#define KLOG__SAMPLE_NEXT(n) \
    __atomic_add_fetch(&(n), 1U, __ATOMIC_RELAXED)

/* ERROR：与 KLOG_ERROR 一样没有级别门槛，只是多一次"只说一次/抽稀" */
#define KLOG_ERROR_ONCE(fmt, ...) \
    do { \
        static uint32_t _klog_once; \
        if (KLOG__ONCE_TAKE(_klog_once)) { \
            kprintf(KLOG_COLOR_RED "[ERROR][C%u] " "%s:%d: " fmt \
                    KLOG_COLOR_RESET "", \
                    klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define KLOG_ERROR_SAMPLE(fmt, ...) \
    do { \
        static uint32_t _klog_n; \
        uint32_t _klog_seq_ = KLOG__SAMPLE_NEXT(_klog_n); \
        if (klog_sample_hit(_klog_seq_)) { \
            kprintf(KLOG_COLOR_RED "[ERROR][C%u] " "%s:%d: " fmt \
                    KLOG_COLOR_RESET "", \
                    klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define KLOG_WARN_ONCE(fmt, ...) \
    do { \
        static uint32_t _klog_once; \
        if (g_log_level >= LOG_LEVEL_WARN && KLOG__ONCE_TAKE(_klog_once)) { \
            kprintf(KLOG_COLOR_YELLOW "[WARN][C%u] " "%s:%d: " fmt \
                    KLOG_COLOR_RESET "", \
                    klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define KLOG_WARN_SAMPLE(fmt, ...) \
    do { \
        static uint32_t _klog_n; \
        if (g_log_level >= LOG_LEVEL_WARN) { \
            uint32_t _klog_seq_ = KLOG__SAMPLE_NEXT(_klog_n); \
            if (klog_sample_hit(_klog_seq_)) { \
                kprintf(KLOG_COLOR_YELLOW "[WARN][C%u] " "%s:%d: " fmt \
                        KLOG_COLOR_RESET "", \
                        klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
            } \
        } \
    } while (0)

#define KLOG_INFO_ONCE(fmt, ...) \
    do { \
        static uint32_t _klog_once; \
        if (g_log_level >= LOG_LEVEL_INFO && KLOG__ONCE_TAKE(_klog_once)) { \
            kprintf(KLOG_COLOR_GREEN "[INFO][C%u] " "%s:%d: " fmt \
                    KLOG_COLOR_RESET "", \
                    klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define KLOG_INFO_SAMPLE(fmt, ...) \
    do { \
        static uint32_t _klog_n; \
        if (g_log_level >= LOG_LEVEL_INFO) { \
            uint32_t _klog_seq_ = KLOG__SAMPLE_NEXT(_klog_n); \
            if (klog_sample_hit(_klog_seq_)) { \
                kprintf(KLOG_COLOR_GREEN "[INFO][C%u] " "%s:%d: " fmt \
                        KLOG_COLOR_RESET "", \
                        klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
            } \
        } \
    } while (0)

/* 未打标签的 DEBUG/TRACE 版本**保留 GENERIC 检查**，否则 LOG_MODULES= 的
 * 白名单语义会被这些新宏绕过去（对照上面的 KLOG_DEBUG）。 */
#define KLOG_DEBUG_ONCE(fmt, ...) \
    do { \
        static uint32_t _klog_once; \
        if ((g_log_level >= LOG_LEVEL_DEBUG) && \
            log_is_module_enabled(LOG_MODULE_GENERIC) && \
            KLOG__ONCE_TAKE(_klog_once)) { \
            kprintf(KLOG_COLOR_BLUE "[DEBUG][C%u] " "%s:%d: " fmt \
                    KLOG_COLOR_RESET "", \
                    klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define KLOG_DEBUG_SAMPLE(fmt, ...) \
    do { \
        static uint32_t _klog_n; \
        if ((g_log_level >= LOG_LEVEL_DEBUG) && \
            log_is_module_enabled(LOG_MODULE_GENERIC)) { \
            uint32_t _klog_seq_ = KLOG__SAMPLE_NEXT(_klog_n); \
            if (klog_sample_hit(_klog_seq_)) { \
                kprintf(KLOG_COLOR_BLUE "[DEBUG][C%u] " "%s:%d: " fmt \
                        KLOG_COLOR_RESET "", \
                        klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
            } \
        } \
    } while (0)

/* 模块版：守卫顺序 level → module → 原子操作，被掩码关掉的点连原子操作都不付。
 * 这是"降级到 DEBUG 的嘈杂子系统"该用的宏 —— 也是让 LOG_MODULES=timer,mm,fs
 * 真正能过滤东西的唯一途径。 */
#define KLOG_MODULE_DEBUG_ONCE(module, fmt, ...) \
    do { \
        static uint32_t _klog_once; \
        if ((g_log_level >= LOG_LEVEL_DEBUG) && log_is_module_enabled(module) && \
            KLOG__ONCE_TAKE(_klog_once)) { \
            kprintf(KLOG_COLOR_BLUE "[DEBUG][C%u] [MOD] " "%s:%d: " fmt \
                    KLOG_COLOR_RESET "", \
                    klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define KLOG_MODULE_DEBUG_SAMPLE(module, fmt, ...) \
    do { \
        static uint32_t _klog_n; \
        if ((g_log_level >= LOG_LEVEL_DEBUG) && log_is_module_enabled(module)) { \
            uint32_t _klog_seq_ = KLOG__SAMPLE_NEXT(_klog_n); \
            if (klog_sample_hit(_klog_seq_)) { \
                kprintf(KLOG_COLOR_BLUE "[DEBUG][C%u] [MOD] " "%s:%d: " fmt \
                        KLOG_COLOR_RESET "", \
                        klog_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__); \
            } \
        } \
    } while (0)

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

    /*
     * 下面这一组必须逐个列出：漏掉任何一个，它的 static 计数器和 __atomic_*
     * 就会活到 release 构建里。那种情况**编译通过、链接通过、没有任何测试会失败**，
     * 但上面"LOG=none 零日志开销"的承诺就静默失效了。只能靠对 build 目录下
     * 全部 .o 跑 nm 并筛 _klog_ 这种检查才看得出来。
     * （教训：注释里别写带通配符的 shell 路径 —— "斜杠加星号"会提前结束注释，
     *   "斜杠加星号加..."这种序列 GCC 还会额外报 -Wcomment。）
     */
    #undef KLOG_ERROR_ONCE
    #define KLOG_ERROR_ONCE(fmt, ...) do {} while (0)

    #undef KLOG_ERROR_SAMPLE
    #define KLOG_ERROR_SAMPLE(fmt, ...) do {} while (0)

    #undef KLOG_WARN_ONCE
    #define KLOG_WARN_ONCE(fmt, ...) do {} while (0)

    #undef KLOG_WARN_SAMPLE
    #define KLOG_WARN_SAMPLE(fmt, ...) do {} while (0)

    #undef KLOG_INFO_ONCE
    #define KLOG_INFO_ONCE(fmt, ...) do {} while (0)

    #undef KLOG_INFO_SAMPLE
    #define KLOG_INFO_SAMPLE(fmt, ...) do {} while (0)

    #undef KLOG_DEBUG_ONCE
    #define KLOG_DEBUG_ONCE(fmt, ...) do {} while (0)

    #undef KLOG_DEBUG_SAMPLE
    #define KLOG_DEBUG_SAMPLE(fmt, ...) do {} while (0)

    #undef KLOG_MODULE_DEBUG_ONCE
    #define KLOG_MODULE_DEBUG_ONCE(module, fmt, ...) do {} while (0)

    #undef KLOG_MODULE_DEBUG_SAMPLE
    #define KLOG_MODULE_DEBUG_SAMPLE(module, fmt, ...) do {} while (0)
#endif

#endif /* KLOG_H */
