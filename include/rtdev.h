/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef RTDEV_H
#define RTDEV_H

#include "types.h"

/*
   rtdev — RT 模板可用的设备契约

   只有通过 RT 一致性要求的驱动才注册 rtdev_ops：
     - setup/teardown 在控制面调用，可以睡眠/分配/开关电源；
     - write/read/ioctl_rt 在 RT 任务里调用，必须有界、无分配、无睡眠、
       无 VFS、不能长时间持锁；
     - safe_state 在 STOP/DESTROY 时调用，把设备置回安全值。

   模板不直接认识具体驱动，只拿 create 时绑定的 struct rtdev *。
   设备是独占的：同一个 rtdev 同时只能绑给一个 RT worker。
*/

#define RTDEV_NAME_LEN 16

struct rtdev;

struct rtdev_ops {
    const char *name;

    int (*setup)(struct rtdev *d, const void *cfg, uint32_t cfg_len);
    int (*write)(struct rtdev *d, const void *buf, uint32_t len);
    int (*read)(struct rtdev *d, void *buf, uint32_t len);
    int (*ioctl_rt)(struct rtdev *d, uint32_t cmd, uint64_t arg);
    void (*safe_state)(struct rtdev *d);
    void (*teardown)(struct rtdev *d);
};

struct rtdev {
    char name[RTDEV_NAME_LEN];
    const struct rtdev_ops *ops;
    void *ctx;
    uint32_t owner; /* rt worker id + 1；0 = 空闲 */
};

/* 由驱动在初始化阶段调用；注册失败返回负 errno。 */
int rtdev_register(const char *name, const struct rtdev_ops *ops, void *ctx);

/* 由 worker 控制面调用：按名字独占认领；失败返回 NULL。 */
struct rtdev *rtdev_claim(const char *name, uint32_t owner);
void rtdev_release(struct rtdev *d, uint32_t owner);

/* RT 数据面入口；未绑定或驱动没实现对应操作时返回负 errno。 */
int rtdev_write(struct rtdev *d, const void *buf, uint32_t len);
int rtdev_read(struct rtdev *d, void *buf, uint32_t len);
void rtdev_safe_state(struct rtdev *d);

/* 内置示例设备 "vout"：把每次写当 u64 记录下来，用于验证契约。 */
void rtdev_register_builtin(void);

#endif /* RTDEV_H */
