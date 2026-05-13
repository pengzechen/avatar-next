#include "types.h"
#include "spinlock.h"
#include "klog.h"

/*
 * 内核日志系统实现
 */

#define BUFSZ 512

/* 全局日志级别（由 Makefile 的 LOG_DEFINE 设置） */
#ifndef LOG_LEVEL
    #define LOG_LEVEL LOG_LEVEL_INFO
#endif

log_level_t g_log_level __attribute__((weak)) = LOG_LEVEL;

/* 全局模块日志掩码（默认启用所有模块） */
uint64_t g_log_module_mask __attribute__((weak)) = 0xFFFFFFFFFFFFFFFFULL;

/* SMP 日志序列化锁：保证多核并发输出不交叉 */
static spinlock_noirq_t g_klog_lock = SPINLOCK_NOIRQ_INIT;

/* 外部依赖：UART 输出函数（需要在 platform 中实现） */
extern void uart_putchar(char c);
extern void uart_putstr(const char *str);

/**
 * klog_putchar - 直接输出一个字符到 UART
 */
void
klog_putchar(char c)
{
    /* 暂时禁用锁进行调试 */
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

/**
 * kvprintf - 格式化输出到内核日志
 */
int
kvprintf(const char *fmt, va_list va)
{
    extern int my_vsnprintf(char *buf, int size, const char *fmt, va_list va);
    char buf[BUFSZ];
    int  len;
    int  i;

    len = my_vsnprintf(buf, sizeof(buf), fmt, va);

    /* 逐字符输出 */
    for (i = 0; i < len; i++) {
        klog_putchar(buf[i]);
    }

    return len;
}

/**
 * kprintf - 格式化输出到内核日志（SMP 安全：持锁期间屏蔽本核中断）
 */
int
kprintf(const char *fmt, ...)
{
    va_list va;
    int     r;

    spin_lock_irqsave(&g_klog_lock);
    va_start(va, fmt);
    r = kvprintf(fmt, va);
    va_end(va);
    spin_unlock_irqrestore(&g_klog_lock);

    return r;
}
