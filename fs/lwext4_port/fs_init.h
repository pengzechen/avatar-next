/*
 * fs/lwext4_port/fs_init.h - 文件系统初始化公开接口
 *
 * 此头文件不依赖任何 lwext4 类型，可被普通内核代码直接包含。
 */
#ifndef __FS_INIT_H__
#define __FS_INIT_H__

/**
 * fs_init - 初始化根文件系统
 *
 * 将 RAM 块设备注册到 lwext4 并挂载到 "/"。
 * 依赖 ramblk_init() 和 PMM 初始化已完成。
 *
 * 成功返回 0，失败返回负错误码。
 */
int fs_init(void);

#endif /* __FS_INIT_H__ */
