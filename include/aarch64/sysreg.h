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

#define READ_ID_AA64PFR0_EL1()      SYSREG_READ("S3_0_C0_C4_0")
#define READ_ID_AA64PFR1_EL1()      SYSREG_READ("S3_0_C0_C4_1")
#define READ_ID_AA64ZFR0_EL1()      SYSREG_READ("S3_0_C0_C4_4")
#define READ_ID_AA64SMFR0_EL1()     SYSREG_READ("S3_0_C0_C4_5")
#define READ_ID_AA64DFR0_EL1()      SYSREG_READ("S3_0_C0_C5_0")
#define READ_ID_AA64DFR1_EL1()      SYSREG_READ("S3_0_C0_C5_1")
#define READ_ID_AA64ISAR0_EL1()     SYSREG_READ("S3_0_C0_C6_0")
#define READ_ID_AA64ISAR1_EL1()     SYSREG_READ("S3_0_C0_C6_1")
#define READ_ID_AA64ISAR2_EL1()     SYSREG_READ("S3_0_C0_C6_2")
#define READ_ID_AA64MMFR0_EL1()     SYSREG_READ("S3_0_C0_C7_0")
#define READ_ID_AA64MMFR1_EL1()     SYSREG_READ("S3_0_C0_C7_1")
#define READ_ID_AA64MMFR2_EL1()     SYSREG_READ("S3_0_C0_C7_2")
#define READ_ID_AA64MMFR3_EL1()     SYSREG_READ("S3_0_C0_C7_3")
#define READ_ID_PFR0_EL1()          SYSREG_READ("S3_0_C0_C1_0")
#define READ_ID_PFR1_EL1()          SYSREG_READ("S3_0_C0_C1_1")
#define READ_ID_DFR0_EL1()          SYSREG_READ("S3_0_C0_C1_2")
#define READ_ID_AFR0_EL1()          SYSREG_READ("S3_0_C0_C1_3")
#define READ_ID_MMFR0_EL1()         SYSREG_READ("S3_0_C0_C1_4")
#define READ_ID_MMFR1_EL1()         SYSREG_READ("S3_0_C0_C1_5")
#define READ_ID_MMFR2_EL1()         SYSREG_READ("S3_0_C0_C1_6")
#define READ_ID_MMFR3_EL1()         SYSREG_READ("S3_0_C0_C1_7")
#define READ_ID_ISAR0_EL1()         SYSREG_READ("S3_0_C0_C2_0")
#define READ_ID_ISAR1_EL1()         SYSREG_READ("S3_0_C0_C2_1")
#define READ_ID_ISAR2_EL1()         SYSREG_READ("S3_0_C0_C2_2")
#define READ_ID_ISAR3_EL1()         SYSREG_READ("S3_0_C0_C2_3")
#define READ_ID_ISAR4_EL1()         SYSREG_READ("S3_0_C0_C2_4")
#define READ_ID_ISAR5_EL1()         SYSREG_READ("S3_0_C0_C2_5")
#define READ_ID_MMFR4_EL1()         SYSREG_READ("S3_0_C0_C2_6")
#define READ_ID_ISAR6_EL1()         SYSREG_READ("S3_0_C0_C2_7")
#define READ_MVFR0_EL1()            SYSREG_READ("S3_0_C0_C3_0")
#define READ_MVFR1_EL1()            SYSREG_READ("S3_0_C0_C3_1")
#define READ_MVFR2_EL1()            SYSREG_READ("S3_0_C0_C3_2")
#define READ_ID_PFR2_EL1()          SYSREG_READ("S3_0_C0_C3_4")
#define READ_ID_DFR1_EL1()          SYSREG_READ("S3_0_C0_C3_5")
#define READ_ID_MMFR5_EL1()         SYSREG_READ("S3_0_C0_C3_6")

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

/* 虚拟计数器相对物理计数器的偏移（EL2，虚拟定时器投递判据用） */
#define READ_CNTVOFF_EL2()          SYSREG_READ("cntvoff_el2")
#define WRITE_CNTVOFF_EL2(val)      SYSREG_WRITE("cntvoff_el2", val)

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
