/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef RISCV64_CSR_H
#define RISCV64_CSR_H

/*
   include/riscv64/csr.h — H-extension / VS-mode CSR 编号的唯一定义

   同时被 C 和 .S 使用, 所以这里只能有 #define, 不能有类型或函数。

   为什么要单独一份：
     - 本工具链下 `hgatp`/`hstatus`/`hvip` 这些符号名会被汇编器静默映射到
       VS 级编号(例如 0x680 → vsatp), 编译全绿, 运行期才炸。因此所有
       CSR 访问一律用数值, 这里就是那组数值的唯一来源。
     - 以前 hext.h 和 hext_vcpu.S 各写一份, 改一边漏一边不会报错, 只会
       表现为"某个 VM 字段莫名不更新"。
*/

/* Hypervisor CSRs(HS-mode 控制虚拟化) */
#define CSR_HSTATUS     0x600
#define CSR_HEDELEG     0x602 /* 异常委托到 VS-mode */
#define CSR_HIDELEG     0x603 /* 中断委托到 VS-mode */
#define CSR_HIE         0x604
#define CSR_HTIMEDELTA  0x605
#define CSR_HCOUNTEREN  0x606
#define CSR_HGEIE       0x607
#define CSR_HENVCFG     0x60a
#define CSR_HTIMEDELTAH 0x615 /* 32-bit 高位(RV32 only) */
#define CSR_HTVAL       0x643
#define CSR_HIP         0x644
#define CSR_HVIP        0x645
#define CSR_HTINST      0x64a
#define CSR_HGATP       0x680 /* Second-level page table (Stage-2) */
#define CSR_HGEIP       0xe12

/* 历史别名：调用点用 *_NUM 表示"这个值是数值地址" */
#define CSR_HVIP_NUM CSR_HVIP
#define CSR_HIE_NUM  CSR_HIE

/* VS-mode CSRs(虚拟的 S-mode 寄存器, HS-mode 可直接读写) */
#define CSR_VSSTATUS  0x200
#define CSR_VSIE      0x204
#define CSR_VSTVEC    0x205
#define CSR_VSSCRATCH 0x240
#define CSR_VSEPC     0x241
#define CSR_VSCAUSE   0x242
#define CSR_VSTVAL    0x243
#define CSR_VSIP      0x244
#define CSR_VSATP     0x280

#endif /* RISCV64_CSR_H */
