#include "types.h"
#include "spinlock.h"
#include "klog.h"

/*
 * 内核日志系统实现
 */

#define BUFSZ 512

/* 全局日志级别：初值来自 Makefile 的 LOG_DEFINE（klog.h 里已 #error 兜住缺失） */
log_level_t g_log_level __attribute__((weak)) = LOG_LEVEL;

/* 全局模块日志掩码（默认启用所有模块） */
uint64_t g_log_module_mask __attribute__((weak)) = 0xFFFFFFFFFFFFFFFFULL;

/* 全局 UART 输出锁：保护整条 klog 消息不被多核交错打散 */
static spinlock_noirq_t g_klog_lock = SPINLOCK_NOIRQ_INIT;

/* 外部依赖：UART 输出函数（需要在 platform 中实现） */
extern void uart_putchar(char c);
extern void uart_putstr(const char *str);

/**
 * klog_putchar - 直接输出一个字符到 UART
 *
 * 不在此处加锁：调用者（kvprintf）按整条消息加锁，
 * 避免每字符锁开销 + 多核穿插。
 */
void
klog_putchar(char c)
{
    uart_putchar(c);
}

/**
 * klog_flush - 空操作（保留用于兼容性）
 */
void
klog_flush(void)
{
    /* 直接输出模式，无需刷新 */
}

/*
 * panic 模式：置位后 kvprintf 不再取 g_klog_lock。
 *
 * 为什么需要：崩溃路径上的日志（assert 的 [ASSERT] 行、panic 的调用栈）
 * 有可能发生在**别的 CPU 正持锁**的时候，甚至发生在同一个 CPU 已经持锁
 * 的 klog 调用内部。那样就是在这把自旋锁上转到天荒地老 —— 最该看见的
 * 现场反而一个字都打不出来。
 *
 * 丢锁的代价（多核下几条消息可能交错）在崩溃面前无所谓：马上就要停机了。
 * 见 kernel/debug/backtrace.c 与 docs/basic/BACKTRACE.md。
 */
static volatile bool g_klog_panic;

void
klog_panic_begin(void)
{
    g_klog_panic = true;
}

/**
 * kvprintf - 格式化输出到内核日志
 *
 * 多核下整条消息持锁输出，防止 `[INFO][C0] foo[C1] bar` 这种交错。
 * 使用 IRQ-safe 锁，避免中断处理也调用 klog 导致同核重入死锁。
 */
int
kvprintf(const char *fmt, va_list va)
{
    extern int my_vsnprintf(char *buf, int size, const char *fmt, va_list va);
    char     buf[BUFSZ];
    int      len;
    int      i;

    len = my_vsnprintf(buf, sizeof(buf), fmt, va);

    /*
     * my_vsnprintf 与 C99 snprintf 同语义：**截断时返回"本该写多长"**，
     * 因此这里 len 可能大于缓冲区。必须 clamp —— 否则下面的循环会越界读
     * 栈上 buf 之后的内容（返回地址、保存的寄存器…）并原样打到串口。
     * 单行超过 511 字节就会触发（长路径、大 %s、参数多的 trace）。
     */
    if (len > (int) sizeof(buf) - 1) {
        len = (int) sizeof(buf) - 1;
    }

    if (g_klog_panic) {
        for (i = 0; i < len; i++) {
            uart_putchar(buf[i]);
        }
        return len;
    }

    uint64_t flags;
    spin_lock_irqsave(&g_klog_lock, &flags);
    for (i = 0; i < len; i++) {
        uart_putchar(buf[i]);
    }
    spin_unlock_irqrestore(&g_klog_lock, flags);

    return len;
}

/* ── 模块掩码：名字 ↔ 位 ──────────────────────────────────────────
 *
 * 与 include/klog.h 里的 LOG_MODULE_* 一一对应，加新模块时两处一起改。
 */
typedef struct {
    const char *name;
    uint64_t    bit;
} klog_module_name_t;

static const klog_module_name_t g_klog_module_names[] = {
    { "init",    LOG_MODULE_INIT    },
    { "task",    LOG_MODULE_TASK    },
    { "driver",  LOG_MODULE_DRIVER  },
    { "uart",    LOG_MODULE_UART    },
    { "timer",   LOG_MODULE_TIMER   },
    { "mm",      LOG_MODULE_MM      },
    { "fs",      LOG_MODULE_FS      },
    { "net",     LOG_MODULE_NET     },
    { "smp",     LOG_MODULE_SMP     },
    { "gic",     LOG_MODULE_GIC     },
    { "generic", LOG_MODULE_GENERIC },
    { "syscall", LOG_MODULE_SYSCALL },
};

#define KLOG_MODULE_NAME_COUNT \
    (sizeof(g_klog_module_names) / sizeof(g_klog_module_names[0]))

/* tok[0..len) 是否等于 name（tok 不是 NUL 结尾的，来自逗号切分） */
static int
klog_name_eq(const char *tok, int len, const char *name)
{
    int i = 0;

    while (i < len && name[i] != '\0' && tok[i] == name[i])
        i++;

    return (i == len) && (name[i] == '\0');
}

int
log_set_modules_by_name(const char *names)
{
    uint64_t    mask = 0;
    int         bad  = 0;
    const char *p    = names;

    if (p == NULL)
        return 0;

    while (*p) {
        const char *start = p;
        int         len;
        int         found = 0;

        while (*p && *p != ',')
            p++;
        len = (int) (p - start);

        /* 去掉两端空白，允许 "uart, gic" */
        while (len > 0 && (*start == ' ' || *start == '\t')) {
            start++;
            len--;
        }
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
            len--;

        if (len == 0) {
            /* 空项（如 "uart,,gic" 或结尾逗号）：忽略 */
        } else if (klog_name_eq(start, len, "all")) {
            mask  = ~0ULL;
            found = 1;
        } else if (klog_name_eq(start, len, "none")) {
            mask  = 0;
            found = 1;
        } else {
            for (unsigned i = 0; i < KLOG_MODULE_NAME_COUNT; i++) {
                if (klog_name_eq(start, len, g_klog_module_names[i].name)) {
                    mask |= g_klog_module_names[i].bit;
                    found = 1;
                    break;
                }
            }
        }

        if (!found)
            bad++;

        if (*p == ',')
            p++;
    }

    g_log_module_mask = mask;
    return bad;
}

static void
klog_append(char *out, int len, int *n, const char *s)
{
    while (*s && *n < len - 1)
        out[(*n)++] = *s++;
    out[*n] = '\0';
}

int
log_modules_to_string(char *out, int len)
{
    int n = 0;

    if (out == NULL || len <= 0)
        return 0;

    out[0] = '\0';

    if (g_log_module_mask == 0) {
        klog_append(out, len, &n, "none");
        return n;
    }

    for (unsigned i = 0; i < KLOG_MODULE_NAME_COUNT; i++) {
        if (g_log_module_mask & g_klog_module_names[i].bit) {
            if (n > 0)
                klog_append(out, len, &n, ",");
            klog_append(out, len, &n, g_klog_module_names[i].name);
        }
    }

    if (n == 0)   /* 掩码里只有表外的位 */
        klog_append(out, len, &n, "unknown");

    return n;
}

void
klog_init(void)
{
    char names[128];

#ifdef LOG_MODULES_DEFAULT
    int bad = log_set_modules_by_name(LOG_MODULES_DEFAULT);
    if (bad > 0) {
        KLOG_ERROR("[klog] LOG_MODULES 有 %d 个无法识别的模块名：\"%s\"\n",
                   bad, LOG_MODULES_DEFAULT);
    }
#endif

    log_modules_to_string(names, sizeof(names));
    KLOG_INFO("[klog] level=%u modules=%s\n", (unsigned) g_log_level, names);
}

/**
 * kprintf - 格式化输出到内核日志
 */
int
kprintf(const char *fmt, ...)
{
    va_list va;
    int     r;

    va_start(va, fmt);
    r = kvprintf(fmt, va);
    va_end(va);

    return r;
}
