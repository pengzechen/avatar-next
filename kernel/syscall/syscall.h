/*
 * kernel/syscall/syscall.h - 系统调用接口定义
 */

#ifndef KERNEL_SYSCALL_SYSCALL_H
#define KERNEL_SYSCALL_SYSCALL_H

#include "types.h"
#include "exception.h"

/* ── 时间相关结构 ───────────────────────────────────────────────── */

/**
 * struct timeval - 时间值结构
 * @tv_sec: 秒
 * @tv_usec: 微秒
 */
struct timeval {
    int64_t tv_sec;     /* 秒 */
    int64_t tv_usec;    /* 微秒 */
};

/* ── 系统调用号定义 ──────────────────────────────────────────── */

typedef enum {
    /* 进程管理 */
    SYS_EXIT  = 0,    /* 退出当前进程 */
    SYS_YIELD = 1,    /* 让出 CPU */
    SYS_GETPID = 2,   /* 获取进程 ID */
    SYS_SLEEP = 3,    /* 睡眠指定毫秒数 */
    SYS_EXECVE = 4,   /* 执行程序 */

    /* 内存管理 */
    SYS_BRK   = 10,   /* 设置程序断点 */
    SYS_SBRK  = 11,   /* 增加程序断点 */

    /* 文件操作 — 使用私有号段避免与 Linux AArch64 号冲突 */
    SYS_WRITE = 0x4000,   /* 写入字符串到 UART */
    SYS_READ  = 0x4001,   /* 从设备读取 */
    SYS_OPEN  = 0x4002,   /* 打开文件 */
    SYS_CLOSE = 0x4003,   /* 关闭文件 */

    /* 时间相关 */
    SYS_GETTIMEOFDAY = 30,  /* 获取系统时间 */

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
void syscall_handler(trap_frame_t *frame);

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
 * sys_read - 从输入设备读取
 * @buf: 缓冲区指针（用户虚拟地址）
 * @len: 缓冲区大小
 *
 * 返回实际读取的字符数
 */
int64_t sys_read(char *buf, uint64_t len);

/**
 * sys_open - 打开文件（暂未实现）
 * @pathname: 文件路径
 * @flags: 打开标志
 * @mode: 权限模式
 *
 * 返回文件描述符或 -1
 */
int64_t sys_open(const char *pathname, int flags, int mode);

/**
 * sys_close - 关闭文件（暂未实现）
 * @fd: 文件描述符
 *
 * 返回 0 或 -1
 */
int64_t sys_close(int fd);

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

/**
 * sys_getpid - 获取当前进程 ID
 *
 * 返回进程 ID
 */
int64_t sys_getpid(void);

/**
 * sys_sleep - 睡眠指定毫秒数
 * @ms: 毫秒数
 *
 * 返回 0 表示成功
 */
int64_t sys_sleep(uint64_t ms);

/**
 * sys_execve - 执行程序
 * @pathname: 程序路径
 * @argv: 参数数组
 * @envp: 环境变量数组（未使用）
 *
 * 返回：成功不返回，失败返回 -1
 */
int64_t sys_execve(const char *pathname, char **argv, char **envp);

/**
 * sys_brk - 设置程序断点
 * @addr: 新的程序断点地址
 *
 * 返回新的程序断点（可能失败）
 */
void *sys_brk(void *addr);

/**
 * sys_sbrk - 增加程序断点
 * @increment: 增量的字节数
 *
 * 返回旧的程序断点
 */
void *sys_sbrk(int64_t increment);

/**
 * sys_gettimeofday - 获取系统时间
 * @tv: 时间值结构指针
 * @tz: 时区结构指针（未使用）
 *
 * 返回 0 表示成功，-1 表示失败
 */
int64_t sys_gettimeofday(struct timeval *tv, void *tz);

/**
 * sys_mmap - 匿名内存映射（供 musl malloc 使用）
 */
uint64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset);
uint64_t sys_munmap(uint64_t addr, uint64_t len);

#endif /* KERNEL_SYSCALL_SYSCALL_H */
