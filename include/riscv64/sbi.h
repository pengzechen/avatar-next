/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef RISCV64_SBI_H
#define RISCV64_SBI_H

/*
   include/riscv64/sbi.h — SBI(Supervisor Binary Interface)常量 + 主机侧调用封装

   常量同时服务两类使用者:
     - 宿主 bring-up(kernel/task/cpu.c)：HSM/IPI 扩展走这里的内联封装;
     - VMM 的 guest 模拟(kernel/vmm/riscv64/hext_run.c)：guest 以
       a7=EID, a6=FID, a0..=参数 发 ecall, 宿主按同一组编号分派。

   以前这组常量写在 hext.h(那是 H-ext CSR 的头)里, 宿主侧又自己写了一份
   功能号 —— 和 csr.h 顶部记录的问题同源: 改一边漏一边不会报错。
   现在是唯一定义, hext.h 不再重复。

   调用约定: a7=扩展号(EID), a6=功能号(FID), a0..a5=参数; 返回值在 a0。
*/

#include "types.h"

/* SBI 返回码(a0) */
#define SBI_SUCCESS               0
#define SBI_ERR_FAILED            ((uint64_t)(-1))
#define SBI_ERR_NOT_SUPPORTED     ((uint64_t)(-2))
#define SBI_ERR_INVALID_PARAM     ((uint64_t)(-3))
#define SBI_ERR_ALREADY_AVAILABLE ((uint64_t)(-6))

/* Legacy 扩展(a7 直接是功能号) */
#define SBI_LEGACY_SET_TIMER       0x00
#define SBI_LEGACY_CONSOLE_PUTCHAR 0x01
#define SBI_LEGACY_CONSOLE_GETCHAR 0x02
#define SBI_LEGACY_SHUTDOWN        0x08

/* 现代扩展(a7=EID, a6=FID) */
#define SBI_EXT_BASE              0x10
#define SBI_BASE_GET_SPEC_VERSION 0
#define SBI_BASE_GET_IMPL_ID      1
#define SBI_BASE_GET_IMPL_VERSION 2
#define SBI_BASE_PROBE_EXTENSION  3
#define SBI_BASE_GET_MVENDORID    4
#define SBI_BASE_GET_MARCHID      5
#define SBI_BASE_GET_MIMPID       6
#define SBI_EXT_TIME              0x54494d45UL /* "TIME" */
#define SBI_EXT_RFENCE            0x52464e43UL /* "RFNC" */
#define SBI_EXT_IPI               0x735049UL   /* "sPI" */
#define SBI_EXT_HSM               0x48534dUL   /* "HSM" */
#define SBI_EXT_SRST              0x53525354UL /* "SRST" */

/* TIME 扩展 FID(a6) */
#define SBI_TIME_SET_TIMER 0

/* IPI 扩展 FID(a6) */
#define SBI_IPI_SEND_IPI 0

/* HSM 扩展 FID(a6) */
#define SBI_HSM_HART_START      0
#define SBI_HSM_HART_STOP       1
#define SBI_HSM_HART_GET_STATUS 2

/* HSM 状态(hart_get_status 的 a1)。SUSPENDED 用不到, 不定义。 */
#define SBI_HSM_STATE_STARTED  0
#define SBI_HSM_STATE_STOPPED  1
#define SBI_HSM_STATE_STARTING 2
#define SBI_HSM_STATE_STOPPING 3

/* RFENCE FID(a6) */
#define SBI_RFENCE_REMOTE_FENCE_I          0
#define SBI_RFENCE_REMOTE_SFENCE_VMA       1
#define SBI_RFENCE_REMOTE_SFENCE_VMA_ASID  2
#define SBI_RFENCE_REMOTE_HFENCE_GVMA      3
#define SBI_RFENCE_REMOTE_HFENCE_GVMA_VMID 4
#define SBI_RFENCE_REMOTE_HFENCE_VVMA      5
#define SBI_RFENCE_REMOTE_HFENCE_VVMA_ASID 6

/* SRST 扩展 FID(a6) */
#define SBI_SRST_RESET                  0
#define SBI_SRST_RESET_TYPE_SHUTDOWN    0
#define SBI_SRST_RESET_TYPE_COLD_REBOOT 1
#define SBI_SRST_RESET_REASON_NONE      0

/*
   sbi_hart_start - HSM 扩展: 启动指定 hart
   @hartid:      目标 hart 的硬件号(mhartid 在 S-mode 不可读, 由调用者保存)
   @start_addr:  入口的**物理**地址(SBI 在目标核的 MMU 配置前进入)
   @opaque:      透传给入口的 a1(本内核用它传逻辑 CPU 号)

   返回: SBI 返回码, 0 = 成功
*/
static inline long sbi_hart_start(uint64_t hartid, uint64_t start_addr, uint64_t opaque)
{
    register uint64_t a0 __asm__("a0") = hartid;
    register uint64_t a1 __asm__("a1") = start_addr;
    register uint64_t a2 __asm__("a2") = opaque;
    register uint64_t a6 __asm__("a6") = SBI_HSM_HART_START;
    register uint64_t a7 __asm__("a7") = SBI_EXT_HSM;

    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a6), "r"(a7) : "memory");
    return (long)a0;
}

/*
   sbi_send_ipi - IPI 扩展: 向单个 hart 发软件中断
   @hartid: 目标 hart 的硬件号

   只支持 hartid < 64：这里把 "1 << hartid" 直接放在 a0(hart_mask 的低
   XLEN 位)。超过 64 个 hart 的平台需要改用 hart_mask_base + 稀疏掩码。

   返回: SBI 返回码, 0 = 成功, -1 = hartid 超出本封装范围
*/
static inline long sbi_send_ipi(uint64_t hartid)
{
    if (hartid >= 64U)
        return -1;

    register uint64_t a0 __asm__("a0") = 1ULL << hartid;
    register uint64_t a1 __asm__("a1") = 0;
    register uint64_t a6 __asm__("a6") = SBI_IPI_SEND_IPI;
    register uint64_t a7 __asm__("a7") = SBI_EXT_IPI;

    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
    return (long)a0;
}

#endif /* RISCV64_SBI_H */
