/*
 * kernel/syscall/syscall.h - 系统调用接口定义
 */

#ifndef KERNEL_SYSCALL_SYSCALL_H
#define KERNEL_SYSCALL_SYSCALL_H

#include "types.h"

/* ── 系统调用号定义 ──────────────────────────────────────────── */

typedef enum {
    SYS_WRITE = 0,    /* 写入字符串到 UART */
    SYS_EXIT  = 1,    /* 退出当前进程 */
    SYS_YIELD = 2,    /* 让出 CPU */
    SYS_MAX          /* 系统调用数量 */
} syscall_num_t;

/* ── 系统调用处理函数 ──────────────────────────────────────────── */

/**
 * syscall_handler - 系统调用分发器
 * @regs: 用户寄存器数组（x0-x7 为参数）
 *
 * 从 x8 获取系统调用号，分发到具体的处理函数。
 * 返回值写入 x0。
 */
void syscall_handler(uint64_t *regs);

/* ── 具体系统调用实现 ──────────────────────────────────────────── */

/**
 * sys_write - 输出字符串到 UART
 * @str: 字符串指针（用户虚拟地址）
 * @len: 字符串长度
 *
 * 返回实际写入的字符数
 */
int64_t sys_write(const char *str, uint64_t len);

/**
 * sys_exit - 退出当前用户进程
 * @status: 退出状态码
 */
void sys_exit(int status) __attribute__((noreturn));

/**
 * sys_yield - 让出 CPU
 *
 * 返回 0 表示成功
 */
int64_t sys_yield(void);

#endif /* KERNEL_SYSCALL_SYSCALL_H */
