#ifndef USB_API_H
#define USB_API_H

/*
 * include/usb_api.h — Rust USB 驱动的 C 接口声明
 *
 * 由 rust/avatar_usb/src/lib.rs 实现，通过 #[no_mangle] extern "C" 导出。
 * 需要 ARCH=riscv64 + USB=dwc2 编译选项。
 *
 * 硬件：SG2002 / CV1812H DWC2 USB 控制器
 */

#include "types.h"

typedef struct {
    uint32_t num_devices;
    uint8_t  first_uvc_addr;
    uint32_t first_uvc_ep0_mps;
    uint16_t first_uvc_vid;
    uint16_t first_uvc_pid;
    uint8_t  has_uvc;
    uint8_t  has_msc;
} usb_enumerate_result_t;

typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint32_t transfers;
    uint32_t data_packets;
    uint8_t  fid;
} usb_uvc_frame_t;

/*
 * dwc2_usb_set_base_virt - 设置 DWC2 控制器 MMIO 虚拟基址
 */
void dwc2_usb_set_base_virt(uintptr_t addr);

/*
 * dwc2_usb_set_phy_base_virt - 设置 CV182x USB2 PHY MMIO 虚拟基址
 */
void dwc2_usb_set_phy_base_virt(uintptr_t addr);

/*
 * dwc2_usb_power_up - USB 上电序列（时钟、PHY 复位、VBUS GPIO）
 *
 * 必须在 dwc2_usb_init 之前调用。所有参数为虚拟地址。
 * 内部有 2 秒等待让 VBUS 稳定。
 *
 * 返回：0 = 成功，< 0 = 失败
 */
int dwc2_usb_power_up(uintptr_t clkgen_base, uintptr_t top_base,
                       uintptr_t fmux_base, uintptr_t ioblk_base,
                       uintptr_t gpio1_base);

/*
 * dwc2_usb_init - 初始化 DWC2 主机控制器
 *
 * 返回：0 = 成功，< 0 = 失败
 */
int dwc2_usb_init(void);

/*
 * dwc2_usb_device_connected - 检查根口是否有设备连接
 *
 * 返回：1 = 已连接，0 = 未连接
 */
int dwc2_usb_device_connected(void);

/*
 * dwc2_usb_enumerate_device - 枚举 USB 总线设备
 *
 * @out: 枚举结果输出
 *
 * 返回：0 = 成功，< 0 = 失败
 */
int dwc2_usb_enumerate_device(usb_enumerate_result_t *out);

/*
 * dwc2_usb_capture_first_uvc_frame - 捕获一帧 UVC 视频
 *
 * @enum_result: 枚举结果（需先调用 dwc2_usb_enumerate_device）
 * @out:         帧数据输出（data 指向内部 DMA 缓冲区，下次 capture 会覆盖）
 *
 * 返回：0 = 成功，< 0 = 失败
 */
int dwc2_usb_capture_first_uvc_frame(
    const usb_enumerate_result_t *enum_result,
    usb_uvc_frame_t *out);

#endif /* USB_API_H */
