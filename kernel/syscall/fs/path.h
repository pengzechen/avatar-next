/*
 * fs/path.h - 路径规范化与用户/内核字符串拷贝辅助
 *
 * 仅供 kernel/syscall/fs/*.c 内部使用（除 resolve_path /
 * copy_string_from_user 之外，这两个在 syscall_internal.h 中也声明，
 * 供 core/ 模块共享）。
 */
#ifndef KERNEL_SYSCALL_FS_PATH_H
#define KERNEL_SYSCALL_FS_PATH_H

#include "types.h"
#include "task/task.h"

/* 拼接 cwd + path 并规范化（消除 //、./、../）。out 大小 outlen，含 NUL */
void resolve_path(const char *cwd, const char *path, char *out, int outlen);

/*
 * *at 系列：根据 dirfd 决定基准目录后调用 resolve_path。
 * 返回 0 成功；负值为 -errno。
 */
int  resolve_path_at(task_t *task, int dirfd, const char *pathname,
                     char *abspath, int abspath_len);

/* 跟随符号链接（最多 8 层）。仅作用于 ext4，pseudofs 路径不受影响 */
void follow_symlinks(char *out, size_t outsz);

/* 从用户空间读 C 字符串到内核缓冲区；返回写入长度（不含 NUL）或 -1 */
int  copy_string_from_user(const char *ustr, char *kbuf, int maxlen);
/* 内核字符串拷贝到用户缓冲区；返回写入长度（不含 NUL）或 -1 */
int  copy_string_to_user  (const char *kstr, char *ubuf, int maxlen);

/* 根据路径填充 kernel_stat（regular file / dir 自动区分）*/
struct kernel_stat;
void fill_stat_from_ext4(struct kernel_stat *st, const char *path);

#endif /* KERNEL_SYSCALL_FS_PATH_H */
