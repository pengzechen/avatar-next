/*
 * include/unistd.h - 用户空间系统调用接口
 */

#ifndef UNISTD_H
#define UNISTD_H

#include "types.h"

/* ── 系统调用号 ───────────────────────────────────────────────── */

#define SYS_EXIT          0
#define SYS_YIELD         1
#define SYS_GETPID        2
#define SYS_SLEEP         3
#define SYS_EXECVE        4
#define SYS_BRK          10
#define SYS_SBRK         11
#define SYS_WRITE        20
#define SYS_READ         21
#define SYS_OPEN         22
#define SYS_CLOSE        23
#define SYS_GETTIMEOFDAY 30

/* ── 时间相关结构 ───────────────────────────────────────────────── */

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* ── 系统调用声明 ───────────────────────────────────────────────── */

/**
 * write - 输出字符串到设备
 * @str: 字符串指针
 * @len: 字符串长度
 *
 * 返回实际写入的字符数，失败返回 -1
 */
int64_t write(const char *str, uint64_t len);

/**
 * read - 从输入设备读取
 * @buf: 缓冲区指针
 * @len: 缓冲区大小
 *
 * 返回实际读取的字符数，失败返回 -1
 */
int64_t read(char *buf, uint64_t len);

/**
 * open - 打开文件（未实现）
 * @pathname: 文件路径
 * @flags: 打开标志
 * @mode: 权限模式
 *
 * 返回文件描述符，失败返回 -1
 */
int64_t open(const char *pathname, int flags, int mode);

/**
 * close - 关闭文件（未实现）
 * @fd: 文件描述符
 *
 * 返回 0 表示成功，失败返回 -1
 */
int64_t close(int fd);

/**
 * exit - 退出当前进程
 * @status: 退出状态码
 */
void exit(int status) __attribute__((noreturn));

/**
 * getpid - 获取当前进程 ID
 *
 * 返回进程 ID
 */
int64_t getpid(void);

/**
 * sleep - 睡眠指定毫秒数
 * @ms: 毫秒数
 *
 * 返回 0 表示成功
 */
int64_t sleep(uint64_t ms);

/**
 * execve - 执行程序
 * @pathname: 程序路径
 * @argv: 参数数组
 * @envp: 环境变量数组（应传 NULL）
 *
 * 成功不返回，失败返回 -1
 */
int64_t execve(const char *pathname, char **argv, char **envp);

/**
 * brk - 设置程序断点
 * @addr: 新的程序断点地址
 *
 * 返回新的程序断点，失败返回 (void*)-1
 */
void *brk(void *addr);

/**
 * sbrk - 增加程序断点
 * @increment: 增量的字节数
 *
 * 返回旧的程序断点
 */
void *sbrk(int64_t increment);

/**
 * gettimeofday - 获取系统时间
 * @tv: 时间值结构指针
 * @tz: 时区结构指针（未使用，应传 NULL）
 *
 * 返回 0 表示成功，失败返回 -1
 */
int64_t gettimeofday(struct timeval *tv, void *tz);

#endif /* UNISTD_H */
