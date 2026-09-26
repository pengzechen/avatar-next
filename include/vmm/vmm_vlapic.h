/*
 * include/vmm/vmm_vlapic.h — x86_64 虚拟 LAPIC（xAPIC / MMIO）
 *
 * 对标 include/vmm/vmm_vgic.h（aarch64 的 vGIC）与 vmm_vplic.h（riscv64）。
 * 实现见 kernel/vmm/vdev/vlapic.c。
 */
#ifndef VMM_VLAPIC_H
#define VMM_VLAPIC_H

#include "types.h"

/* xAPIC 的 MMIO 窗口（经典地址；EPT 把非 RAM 区置无效后自动陷入）*/
#define VLAPIC_MMIO_BASE    0xFEE00000ULL
#define VLAPIC_MMIO_SIZE    0x1000ULL

/*
 * 寄存器编号 = MMIO 偏移 >> 4（xAPIC 每个寄存器占 16 字节）。
 * 名字与 Intel SDM Vol 3A §11.4 的表一致。
 */
#define VLAPIC_REG_ID           0x02
#define VLAPIC_REG_VERSION      0x03
#define VLAPIC_REG_TPR          0x08
#define VLAPIC_REG_APR          0x09
#define VLAPIC_REG_PPR          0x0a
#define VLAPIC_REG_EOI          0x0b
#define VLAPIC_REG_RRR          0x0c
#define VLAPIC_REG_LDR          0x0d
#define VLAPIC_REG_DFR          0x0e
#define VLAPIC_REG_SVR          0x0f
#define VLAPIC_REG_ISR          0x10   /* 0x10-0x17：256 位 */
#define VLAPIC_REG_TMR          0x18   /* 0x18-0x1f        */
#define VLAPIC_REG_IRR          0x20   /* 0x20-0x27        */
#define VLAPIC_REG_ESR          0x28
#define VLAPIC_REG_LVT_CMCI     0x2f
#define VLAPIC_REG_ICR_LO       0x30
#define VLAPIC_REG_ICR_HI       0x31
#define VLAPIC_REG_LVT_TIMER    0x32
#define VLAPIC_REG_LVT_THERMAL  0x33
#define VLAPIC_REG_LVT_PMI      0x34
#define VLAPIC_REG_LVT_LINT0    0x35
#define VLAPIC_REG_LVT_LINT1    0x36
#define VLAPIC_REG_LVT_ERROR    0x37
#define VLAPIC_REG_TIMER_INIT   0x38
#define VLAPIC_REG_TIMER_CUR    0x39
#define VLAPIC_REG_TIMER_DCR    0x3e
#define VLAPIC_REG_COUNT        0x40   /* 寄存器数组长度（含保留区）*/

/* IA32_APIC_BASE MSR 的位（与 include/x86_64/vmx.h 的 APIC_BASE_* 一致）*/
#define VLAPIC_BASE_ENABLE      (1ULL << 11)
#define VLAPIC_BASE_X2APIC      (1ULL << 10)

void     vlapic_init(uint32_t vcpu_id);
int      vlapic_mmio_handle(uint64_t addr, int is_write, uint8_t size,
                            uint64_t *val);

/* 定时器：每次进 guest 前轮询；返回 1 表示产生了待注入的中断 */
int      vlapic_timer_poll(void);

/* 待注入向量：取走 IRR 里编号最大的那个（返回 1 = 取到）*/
int      vlapic_take_pending(uint32_t *vec);

/*
 * vlapic_raise_irq — 设备侧拉高中断线（电平触发语义）
 *
 * 调用方应在**每次进入 guest 前**按设备状态重新调用；APIC 被软件关闭
 * （SVR.EN=0）时丢弃。定时器、ICR 自发中断、外部设备（vUART 等）都经由
 * 这里进 IRR —— 取走时才清位，所以「只在 push 时置一次」会丢中断。
 */
void     vlapic_raise_irq(uint32_t vector);

/* 中断已投递进 guest（填 ISR/TMR，EOI 靠它工作）*/
void     vlapic_accept_interrupt(uint32_t vector, int level_triggered);

/* APIC 软件使能位（SVR bit8）；关掉时一切中断都不投 */
int      vlapic_sw_enabled(void);

/* IA32_APIC_BASE 的读写（guest 由 MSR 影子路径进来）*/
uint64_t vlapic_apic_base(void);
void     vlapic_set_apic_base(uint64_t val);

uint64_t vlapic_now_ns(void);

#endif /* VMM_VLAPIC_H */
