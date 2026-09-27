/*
 * include/vmm/vmm_x86_chipset.h — x86_64 传统芯片组桩的每 VM 状态
 *
 * guest Linux 会去摸一堆 PC 兼容硬件：8259 PIC（端口 0x20/0xA0）、8254 PIT
 * （0x40-0x43 + 0x61）、IO-APIC（MMIO 0xFEC00000）、CMOS、PCI 配置口。
 * 这些**都不是真设备**，是 kernel/vmm/x86_64/vmx.c 里的一组"桩"：只保存
 * guest 写进来的值，让它探测/初始化流程能走完（真实中断走 vLAPIC）。
 *
 * ⚠️ 从前这些是 vmx.c 的文件级 static（g_pic_master_imr / g_ioapic_rt /
 * g_pit[] / g_port61），整机一份。于是：
 *   - 第二个 VM 启动时 vmx_vm_init() 里那段"复位设备桩"会把**第一个 VM 的**
 *     PIC/PIT/IOAPIC 状态一起清掉（那段复位本身是为"第二次启动"加的，
 *     在单 VM 下对，多 VM 下就是串台）；
 *   - 两个 guest 共用一张重定向表，中断向量互相覆盖。
 * 现在每 VM 一份，放进 vm_t 的 x86 段。
 *
 * 单独开一个头而不是塞进 vmm.h：vmm.h 已经被各架构的类型定义撑得很满，
 * 而且这几组状态只有 x86 用得上。
 */
#ifndef VMM_X86_CHIPSET_H
#define VMM_X86_CHIPSET_H

#include "types.h"

/* IO-APIC 重定向表项数（对应 version 寄存器报的 0x11，即 24 项）*/
#define X86_IOAPIC_NENT   24

/* 8254 PIT 的一个通道 */
typedef struct x86_pit_ch {
    uint16_t reload;      /* 装载值 */
    uint64_t base_ns;     /* 装载时刻（宿主单调 ns）*/
    int      running;     /* 已装载、正在计数 */
    int      read_hi;     /* 读指针：0=下次读 LSB，1=下次读 MSB */
    int      rw;          /* 控制字 RW 位：1=只 LSB 2=只 MSB 3=先 LSB 后 MSB */
    int      wstate;      /* RW=3 时已写的字节数 */
    uint8_t  wlo;         /* RW=3 时暂存的低字节 */
    uint16_t latched;     /* latch 命令冻结的值 */
    int      lat_valid;
} x86_pit_ch_t;

/* 每 VM 一份的传统芯片组桩状态 */
typedef struct x86_chipset_state {
    /* 8259 PIC 的两片中断屏蔽寄存器 */
    uint8_t  pic_master_imr;
    uint8_t  pic_slave_imr;

    /* IO-APIC：IOREGSEL 间接寄存器 + 重定向表 */
    uint32_t ioapic_sel;
    uint32_t ioapic_rt[X86_IOAPIC_NENT * 2];
    int      ioapic_inited;      /* "初值全屏蔽"那段的一次性守卫 */

    /* 8254 PIT：三个通道 + 端口 0x61（gate2/speaker）*/
    x86_pit_ch_t pit[3];
    uint8_t      port61;
} x86_chipset_state_t;

#endif /* VMM_X86_CHIPSET_H */
