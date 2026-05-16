/*
 * include/kernel_stat.h — Linux ABI struct stat + dirent64 定义
 *
 * 从 kernel/syscall/syscall.c 提取，供 syscall.c 和 pseudofs.c 共用。
 * 必须与 Linux 内核 ABI 完全匹配（用户态直接读取）。
 */
#ifndef KERNEL_STAT_H
#define KERNEL_STAT_H

#include "types.h"
#include "arch.h"

#if ARCH_X86_64
/* Linux x86_64 ABI: sizeof(struct stat) = 144 */
struct kernel_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    uint64_t st_atime_nsec;
    int64_t  st_mtime_sec;
    uint64_t st_mtime_nsec;
    int64_t  st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};
#else
/* Linux AArch64/RISC-V64 ABI */
struct kernel_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t _pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  _pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    uint64_t st_atime_nsec;
    int64_t  st_mtime_sec;
    uint64_t st_mtime_nsec;
    int64_t  st_ctime_sec;
    uint64_t st_ctime_nsec;
    uint32_t _unused[2];
};
#endif

struct kernel_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1]; /* variable length */
};

#endif /* KERNEL_STAT_H */
