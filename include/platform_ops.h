/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/platform_ops.h — 平台抽象接口

   实现在 platforms/common/platform_ops.c, 所有平台共用。
   platforms/<platform>/platform.c 只放该板的差异(当前仅作为平台 marker)。
*/

#ifndef PLATFORM_OPS_H
#define PLATFORM_OPS_H

/*
   platform_conf_init — 载入平台配置(纯数据, 不碰硬件)

   必须在 platform_early_init() 之前调用: UART 驱动通过
   platform_get_mmio() 取自己的 MMIO 基址。
*/
void platform_conf_init(void);

/*
   platform_early_init — 初始化最早需要的设备(UART)

   必须在任何 KLOG_* 之前调用。此时异常向量/中断控制器可以尚未初始化,
   本函数只做轮询模式的串口配置。
*/
void platform_early_init(void);

/*
   Platform panic - never returns
*/
void platform_panic(void) __attribute__((noreturn));

/*
   Platform shutdown / power-off - never returns
*/
void platform_shutdown(void) __attribute__((noreturn));

#endif /* PLATFORM_OPS_H */
