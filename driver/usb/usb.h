/*
 * driver/usb/usb.h — USB 主机驱动公开 API
 *
 * 基于 Synopsys DWC2 控制器，目标平台 SG2002 (Milk-V Duo S)。
 * 所有函数可在内核初始化阶段调用（单核、无抢占）。
 *
 * 使用顺序：
 *   1. dwc2_usb_set_base_virt()         — 设置 DWC2 MMIO 虚拟基址
 *   2. dwc2_usb_set_phy_base_virt()     — 设置 CV182x USB2 PHY MMIO 虚拟基址
 *   3. dwc2_usb_init()                  — 初始化控制器 + 上电根端口
 *   4. dwc2_usb_device_connected()      — 轮询 CONNSTS 直到设备连接
 *   5. dwc2_usb_reset_root_port()       — 发 USB 总线复位
 *   6. dwc2_usb_enumerate_device()      — 获取描述符 + 设地址 + 设配置
 */
#ifndef __USB_H__
#define __USB_H__

#include "types.h"

/* ==========================================================================
 * 1. USB 标准数据结构
 * ========================================================================== */

/** USB 标准 SETUP 包 (8 字节) */
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_packet_t;

/** USB 标准设备描述符 (18 字节) */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

/** USB 标准配置描述符 (9 字节) */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

/** USB 标准接口描述符 (9 字节) */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

/** USB 标准端点描述符 (7 字节) */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

/** USB Hub 描述符 */
typedef struct {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;
    uint8_t  bHubContrCurrent;
    /* 可变长: DeviceRemovable + PortPwrCtrlMask */
} __attribute__((packed)) usb_hub_desc_t;

/* ==========================================================================
 * 2. 设备枚举结果
 * ========================================================================== */

/** 单个 USB 设备信息（拓扑扫描产出） */
typedef struct {
    uint8_t  dev_addr;        /* 分配的设备地址 (1-127) */
    uint8_t  port;            /* 父 Hub 上的端口号 (root=0) */
    uint8_t  speed;           /* USB_SPEED_HIGH/FULL/LOW */
    uint16_t id_vendor;
    uint16_t id_product;
    uint8_t  b_device_class;
    uint8_t  b_max_packet_size0;
    bool     is_msc;          /* Mass Storage Class */
    bool     is_uvc;          /* Video Class */
    bool     is_hub;          /* Hub 类 */
} usb_device_info_t;

/** 枚举结果汇总 */
#define USB_MAX_DEVICES 16

typedef struct {
    usb_device_info_t devices[USB_MAX_DEVICES];
    uint8_t           num_devices;
    uint8_t           first_msc_addr;   /* 首个 MSC 设备地址，0 表示无 */
    uint16_t          first_msc_vid;
    uint16_t          first_msc_pid;
} usb_enumerate_result_t;

/* ==========================================================================
 * 3. API 函数声明
 * ========================================================================== */

/**
 * dwc2_usb_set_base_virt — 设置 DWC2 控制器 MMIO 虚拟基址
 * @base: 控制器寄存器块起始虚拟地址
 *
 * 在 dwc2_usb_init() 前调用一次。SG2002 通常传入 phys_to_virt(0x04340000)。
 */
void dwc2_usb_set_base_virt(uintptr_t base);

/**
 * dwc2_usb_set_phy_base_virt — 设置 CV182x 片内 USB2 PHY MMIO 虚拟基址
 * @phy_base: PHY 寄存器块起始虚拟地址
 *
 * SG2002 通常传入 phys_to_virt(0x03006000)。
 * 仅在 platform_cfg.h 定义了 DEVICE_USB_PHY_BASE_RAW 时需要。
 */
void dwc2_usb_set_phy_base_virt(uintptr_t phy_base);

/**
 * dwc2_usb_init — 初始化 DWC2 主机控制器
 *
 * 执行流程：
 *   1. 读取 GHWCFG 确认硬件存在
 *   2. 软复位控制器
 *   3. Force Host 模式
 *   4. 配置 FIFO、DMA、PHY
 *   5. 上电根端口
 *
 * 成功时根口已供电，但尚未发总线复位。
 * 随后应调用 dwc2_usb_device_connected() 等待设备，再复位并枚举。
 */
int dwc2_usb_init(void);

/**
 * dwc2_usb_device_connected — 检查根端口是否有设备
 * @return true=已连接, false=未连接
 */
bool dwc2_usb_device_connected(void);

/**
 * dwc2_usb_reset_root_port — 对根端口发 USB 总线复位（~60ms）
 * @return 0=成功, 负值=错误
 *
 * 仅在确认 CONNSTS==1 后调用。
 */
int dwc2_usb_reset_root_port(void);

/**
 * dwc2_usb_get_root_speed — 获取根端口当前速度
 * @return USB_SPEED_HIGH(0)/FULL(1)/LOW(2), 失败返回 -1
 */
int dwc2_usb_get_root_speed(void);

/**
 * dwc2_usb_enumerate_device — 枚举根端口上单个设备
 * @result: 输出参数，储存枚举结果（设备地址、VID/PID 等）
 * @return 0=成功, 负值=错误
 *
 * 前置条件：已调用 dwc2_usb_reset_root_port() 并等待足够恢复时间。
 * 执行：获取 8 字节设备描述符 → SET_ADDRESS → 获取完整描述符 → SET_CONFIGURATION。
 */
int dwc2_usb_enumerate_device(usb_enumerate_result_t *result);

/**
 * dwc2_get_base_virt — 获取当前 DWC2 MMIO 虚拟基址
 * @return 基址，未设置时返回 0
 *
 * 供 usb_core.c 等兄弟编译单元访问寄存器使用。
 */
uintptr_t dwc2_get_base_virt(void);

/**
 * dwc2_usb_read_hprt0 — 读取 HPRT0 寄存器原始值（调试用）
 * @return HPRT0 当前值，未初始化时返回 0
 */
uint32_t dwc2_usb_read_hprt0(void);

/**
 * dwc2_usb_dump_regs — 打印关键寄存器快照（调试用）
 */
void dwc2_usb_dump_regs(void);

#endif /* __USB_H__ */
