#include "types.h"
#include "spinlock.h"
#include "klog.h"

/*
 * 内核日志系统实现
 */

#define BUFSZ 512

/* 全局日志级别（编译时设置，可被 Makefile 的 LOG 覆盖） */
#ifndef LOG_LEVEL
    #define LOG_LEVEL LOG_LEVEL_INFO
#endif

log_level_t g_log_level __attribute__((weak)) = LOG_LEVEL;

/* 全局模块日志掩码（默认启用所有模块） */
uint64_t g_log_module_mask __attribute__((weak)) = 0xFFFFFFFFFFFFFFFFULL;

/* 日志缓冲区 */
static char log_buf[BUFSZ];
static int  log_pos = 0;

/* 日志锁 */
static spinlock_noirq_t log_lock = SPINLOCK_NOIRQ_INIT;

/* 外部依赖：UART 输出函数（需要在 platform 中实现） */
extern void uart_putchar(char c);
extern void uart_putstr(const char *str);

/**
 * klog_putchar - 向日志缓冲区写入一个字符
 */
void
klog_putchar(char c)
{
    spin_lock_irqsave(&log_lock);

    /* 写入缓冲区 */
    if (c == '\n' || log_pos >= BUFSZ - 1) {
        /* 遇到换行或缓冲区满，刷新缓冲区 */
        log_buf[log_pos] = '\0';
        uart_putstr(log_buf);
        log_pos = 0;
    }

    if (c != '\n') {
        log_buf[log_pos++] = c;
    }

    spin_unlock_irqrestore(&log_lock);
}

/**
 * klog_flush - 刷新日志缓冲区
 */
void
klog_flush(void)
{
    spin_lock_irqsave(&log_lock);
    if (log_pos > 0) {
        log_buf[log_pos] = '\0';
        uart_putstr(log_buf);
        log_pos = 0;
    }
    spin_unlock_irqrestore(&log_lock);
}

/**
 * kvprintf - 格式化输出到内核日志
 */
int
kvprintf(const char *fmt, va_list va)
{
    extern int my_vsnprintf(char *buf, int size, const char *fmt, va_list va);
    char buf[BUFSZ];
    int  len;

    len = my_vsnprintf(buf, sizeof(buf), fmt, va);

    /* 逐字符输出 */
    for (int i = 0; i < len; i++) {
        klog_putchar(buf[i]);
    }

    return len;
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
