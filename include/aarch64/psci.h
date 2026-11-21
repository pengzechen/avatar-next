/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef AARCH64_PSCI_H
#define AARCH64_PSCI_H

/*
   include/aarch64/psci.h — PSCI 0.2+ 调用封装(SMC64 conduit)

   只提供 CPU bring-up 需要的两个功能:
     psci_cpu_on          拉起指定 MPIDR 的核
     psci_affinity_info   查询核当前状态(超时诊断用)

   为什么放在头文件里而不是 .c：两个调用都是"固定寄存器 + 一条 smc"的纯
   内联汇编, 没有任何全局状态, 和 arch_cpu_self_get() 一样按架构单独提供
   最自然; 放进 .c 只会让每个使用者各写一份 extern。

   conduit 约定：本内核用 SMC(EL2/VHE 下由 ATF 转发; QEMU virt 也是 SMC),
   功能号取 SMC64 的 0xC40000xx。若以后出现 HVC conduit 的平台, 在这里加
   一个 conduit 选择, 不要把 smc/hvc 散到调用点。
*/

#include "types.h"

/* SMC64 功能号(PSCI 0.2+) */
#define PSCI_CPU_ON        0xC4000003UL
#define PSCI_AFFINITY_INFO 0xC4000004UL

/* 返回码(a0) */
#define PSCI_RET_SUCCESS 0

/*
   psci_cpu_on - 拉起一颗核
   @mpidr:       目标核的 MPIDR
   @entry_phys:  入口的**物理**地址(PSCI 在目标核 MMU 打开前进入)
   @context_id:  透传给入口的 context_id(本内核用它传逻辑 CPU 号)

   返回: PSCI 返回码, 0 = 成功, 负数 = 错误
*/
static inline int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry_phys, uint64_t context_id)
{
    register uint64_t x0 __asm__("x0") = PSCI_CPU_ON;
    register uint64_t x1 __asm__("x1") = mpidr;
    register uint64_t x2 __asm__("x2") = entry_phys;
    register uint64_t x3 __asm__("x3") = context_id;

    __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}

/*
   psci_affinity_info - 查询核状态
   @mpidr: 目标核的 MPIDR

   返回:
     0   = ON
     1   = OFF
     2   = ON_PENDING
     <0  = 错误(例如 -2 INVALID_PARAMS 表示 ATF 不认该 mpidr)

   lowest_affinity_level 固定传 0：本内核按完整的 core 粒度查询。
*/
static inline int64_t psci_affinity_info(uint64_t mpidr)
{
    register uint64_t x0 __asm__("x0") = PSCI_AFFINITY_INFO;
    register uint64_t x1 __asm__("x1") = mpidr;
    register uint64_t x2 __asm__("x2") = 0;

    __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2) : "memory");
    return (int64_t)x0;
}

#endif /* AARCH64_PSCI_H */
