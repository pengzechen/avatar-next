/*
 * include/aarch64/sysreg.h  —  AArch64 系统寄存器访问宏
 *
 * 为 driver/timer_aarch64.h 及其他需要访问 ARM 系统寄存器的模块提供
 * 统一的 mrs/msr 封装。
 */

#ifndef AARCH64_SYSREG_H
#define AARCH64_SYSREG_H

#include "types.h"

/* ============================================================
 * 通用读写宏
 * ============================================================ */

#define SYSREG_READ(reg) \
    ({ uint64_t _v; __asm__ volatile("mrs %0, " reg : "=r"(_v) :: "memory"); _v; })

#define SYSREG_WRITE(reg, val) \
    __asm__ volatile("msr " reg ", %0" :: "r"((uint64_t)(val)) : "memory")

/* ============================================================
 * Generic Timer 系统寄存器
 * ============================================================ */

/* 计数器频率（只读） */
#define READ_CNTFRQ_EL0()           SYSREG_READ("cntfrq_el0")

/* 虚拟计数器 */
#define READ_CNTVCT_EL0()           SYSREG_READ("cntvct_el0")

/* 物理计数器 */
#define READ_CNTPCT_EL0()           SYSREG_READ("cntpct_el0")

/* 虚拟定时器控制 */
#define READ_CNTV_CTL_EL0()         SYSREG_READ("cntv_ctl_el0")
#define WRITE_CNTV_CTL_EL0(val)     SYSREG_WRITE("cntv_ctl_el0", val)

/* 物理定时器控制 */
#define READ_CNTP_CTL_EL0()         SYSREG_READ("cntp_ctl_el0")
#define WRITE_CNTP_CTL_EL0(val)     SYSREG_WRITE("cntp_ctl_el0", val)

/* 虚拟定时器比较值 */
#define READ_CNTV_CVAL_EL0()        SYSREG_READ("cntv_cval_el0")
#define WRITE_CNTV_CVAL_EL0(val)    SYSREG_WRITE("cntv_cval_el0", val)

/* 虚拟定时器剩余值 */
#define READ_CNTV_TVAL_EL0()        SYSREG_READ("cntv_tval_el0")
#define WRITE_CNTV_TVAL_EL0(val)    SYSREG_WRITE("cntv_tval_el0", val)

/* 物理定时器剩余值（只写常用） */
#define WRITE_CNTP_TVAL_EL0(val)    SYSREG_WRITE("cntp_tval_el0", val)

/* ============================================================
 * Exception 系统寄存器
 * ============================================================ */

/* Exception Syndrome Register */
#define READ_ESR_EL1()              SYSREG_READ("esr_el1")
#define READ_FAR_EL1()              SYSREG_READ("far_el1")
#define READ_ELR_EL1()              SYSREG_READ("elr_el1")
#define READ_ESR_EL2()              SYSREG_READ("esr_el2")
#define READ_ESR_EL3()              SYSREG_READ("esr_el3")

/* Fault Address Register */
#define READ_FAR_EL1()              SYSREG_READ("far_el1")
#define READ_FAR_EL2()              SYSREG_READ("far_el2")
#define READ_FAR_EL3()              SYSREG_READ("far_el3")

/* Hypervisor IPA Fault Address Register */
#define READ_HPFAR_EL2()            SYSREG_READ("hpfar_el2")

/* Hypervisor Fault Address Register (EL2) */
#define READ_HYFAR_EL2()            SYSREG_READ("hyfar_el2")

#endif  /* AARCH64_SYSREG_H */
