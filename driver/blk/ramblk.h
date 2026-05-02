/*
 * driver/blk/ramblk.h - RAM 块设备公开接口
 *
 * 此头文件不依赖任何 lwext4 类型，可被普通内核代码直接包含。
 */
#ifndef __RAMBLK_H__
#define __RAMBLK_H__

/**
 * ramblk_init - 打印 RAM 块设备信息（调试用）
 *
 * 在 fs_init() 之前调用，确认内存区域配置正确。
 */
void ramblk_init(void);

#endif /* __RAMBLK_H__ */
