/*
 * include/riscv64/sysreg.h  —  RISC-V 64 CSR 访问宏
 *
 * 为 driver/timer_rv.h 及其他需要访问 RISC-V 控制状态寄存器的模块提供
 * 统一的 csrr/csrw/csrs/csrc 封装。
 */

#ifndef RISCV64_SYSREG_H
#define RISCV64_SYSREG_H

#include "types.h"

/* ============================================================
 * 通用 CSR 访问宏
 * ============================================================ */

#define CSR_READ(csr) \
    ({ uint64_t _v; __asm__ volatile("csrr %0, " #csr : "=r"(_v) :: "memory"); _v; })

#define CSR_WRITE(csr, val) \
    __asm__ volatile("csrw " #csr ", %0" :: "r"((uint64_t)(val)) : "memory")

#define CSR_SET(csr, bits) \
    __asm__ volatile("csrs " #csr ", %0" :: "r"((uint64_t)(bits)) : "memory")

#define CSR_CLEAR(csr, bits) \
    __asm__ volatile("csrc " #csr ", %0" :: "r"((uint64_t)(bits)) : "memory")

/* ============================================================
 * Supervisor 模式寄存器
 * ============================================================ */

#define READ_SSTATUS()          CSR_READ(sstatus)
#define WRITE_SSTATUS(val)      CSR_WRITE(sstatus, val)

#define READ_SIE()              CSR_READ(sie)
#define WRITE_SIE(val)          CSR_WRITE(sie, val)
#define SET_SIE(bits)           CSR_SET(sie, bits)
#define CLEAR_SIE(bits)         CSR_CLEAR(sie, bits)

#define READ_SIP()              CSR_READ(sip)

#define READ_SEPC()             CSR_READ(sepc)
#define READ_SCAUSE()           CSR_READ(scause)
#define READ_STVAL()            CSR_READ(stval)
#define READ_STVEC()            CSR_READ(stvec)
#define WRITE_STVEC(val)        CSR_WRITE(stvec, val)

/* ============================================================
 * 计时器 / 性能计数器
 * ============================================================ */

#define READ_TIME()             CSR_READ(time)
#define READ_MCYCLE()           CSR_READ(mcycle)
#define READ_MINSTRET()         CSR_READ(minstret)

/* ============================================================
 * SIE / SIP 位定义
 * ============================================================ */

#define SIE_SSIE    (1UL << 1)  /* Supervisor Software Interrupt Enable */
#define SIE_STIE    (1UL << 5)  /* Supervisor Timer Interrupt Enable    */
#define SIE_SEIE    (1UL << 9)  /* Supervisor External Interrupt Enable */

#endif  /* RISCV64_SYSREG_H */
