/*
 * include/riscv64/hext.h — RISC-V Hypervisor Extension (H-ext) CSR 定义
 *
 * RISC-V Privileged Spec 1.12 §19-20 (H-extension)
 * 当内核运行在 HS-mode 时，使用这些 CSR 管理 VS-mode guest。
 *
 * 特权级对应关系：
 *   HS-mode  = Hypervisor Supervisor（本内核运行在此）
 *   VS-mode  = Virtual Supervisor（guest OS 运行在此）
 *   VU-mode  = Virtual User（guest 用户进程）
 *   U-mode   = 普通用户进程（非虚拟化，直接 ecall 到 HS-mode）
 */

#ifndef RISCV64_HEXT_H
#define RISCV64_HEXT_H

#include "types.h"
#include "riscv64/sysreg.h"

/* ================================================================
 * H-extension CSR 编号（用于 .insn 格式或 csrr/csrw 直接访问）
 * ================================================================ */

/* Hypervisor CSRs（HS-mode 控制虚拟化）*/
#define CSR_HSTATUS        0x600
#define CSR_HEDELEG        0x602   /* 异常委托到 VS-mode          */
#define CSR_HIDELEG        0x603   /* 中断委托到 VS-mode          */
#define CSR_HIE            0x604
#define CSR_HTIMEDELTA     0x605
#define CSR_HCOUNTEREN     0x606
#define CSR_HGEIE          0x607
#define CSR_HTVAL          0x643
#define CSR_HIP            0x644
#define CSR_HVIP           0x645
#define CSR_HTINST         0x64a
#define CSR_HGEIP          0xe12
#define CSR_HENVCFG        0x60a
#define CSR_HGATP          0x680   /* Second-level page table (Stage-2) */
#define CSR_HTIMEDELTAH    0x615   /* 32-bit 高位（RV32 only）    */

/* VS-mode CSRs（虚拟的 S-mode 寄存器，HS-mode 可直接读写）*/
#define CSR_VSSTATUS       0x200
#define CSR_VSIE           0x204
#define CSR_VSTVEC         0x205
#define CSR_VSSCRATCH      0x240
#define CSR_VSEPC          0x241
#define CSR_VSCAUSE        0x242
#define CSR_VSTVAL         0x243
#define CSR_VSIP           0x244
#define CSR_VSATP          0x280

/* ================================================================
 * hstatus 字段位定义
 * ================================================================ */
#define HSTATUS_VSBE    (1UL <<  5)  /* VS-mode big-endian              */
#define HSTATUS_GVA     (1UL <<  6)  /* guest virtual address in htval  */
#define HSTATUS_SPV     (1UL <<  7)  /* Supervisor Previous Virtualization
                                        =1: sret 进入 VS-mode
                                        =0: sret 进入 HS/U-mode          */
#define HSTATUS_SPVP    (1UL <<  8)  /* SPV 时的先前 VS priv 级别
                                        =1: VS-mode; =0: VU-mode        */
#define HSTATUS_HU      (1UL <<  9)  /* 允许 U-mode 使用 hypervisor 指令 */
#define HSTATUS_VGEIN   (6)          /* 虚拟化中断 external 号，shift   */
#define HSTATUS_VTVM    (1UL << 20)  /* 陷阱 sfence.vma in VS-mode      */
#define HSTATUS_VTW     (1UL << 21)  /* 陷阱 WFI in VS-mode             */
#define HSTATUS_VTSR    (1UL << 22)  /* 陷阱 sret in VS-mode            */
#define HSTATUS_VSXL    (32)         /* VS XLEN，shift                  */

/* ================================================================
 * hstatus.SPV + SPVP 组合：进入 VS-mode 时设置
 *   SPV=1: sret 进入 VS-mode
 *   SPVP=1: VS-mode（非 VU-mode）
 * ================================================================ */
#define HSTATUS_SPV_VS  (HSTATUS_SPV | HSTATUS_SPVP)

/* ================================================================
 * scause 中 VS-mode ecall 的编号
 * VS-mode ecall: cause=10（与普通 U-mode ecall=8 不同）
 * ================================================================ */
#define CAUSE_VS_ECALL   10u

/* ================================================================
 * hedeleg 建议委托给 VS-mode 的异常掩码
 * 常用：用户 ecall(8)、缺页(12/13/15)、非对齐(0/4/6)
 * ================================================================ */
#define HEDELEG_COMMON  ((1UL << 0)  |   /* Insn addr misalign          */ \
                         (1UL << 3)  |   /* Breakpoint                  */ \
                         (1UL << 4)  |   /* Load addr misalign          */ \
                         (1UL << 5)  |   /* Load access fault           */ \
                         (1UL << 6)  |   /* Store addr misalign         */ \
                         (1UL << 7)  |   /* Store access fault          */ \
                         (1UL << 8)  |   /* U-mode ecall                */ \
                         (1UL << 12) |   /* Insn page fault             */ \
                         (1UL << 13) |   /* Load page fault             */ \
                         (1UL << 15))    /* Store page fault            */
/* 注意：bit1(insn access fault) 和 bit2(illegal insn/WFI virtual trap)
 *       不委托：bit2=virtual instruction 陷阱（hstatus.VTW=1 时 WFI）需到 HS-mode */

/* hideleg 建议委托给 VS-mode 的中断掩码（VS-mode 软件/定时器/外部中断）*/
#define HIDELEG_COMMON  ((1UL << 2)  |   /* VS software int            */ \
                         (1UL << 6)  |   /* VS timer int               */ \
                         (1UL << 10))    /* VS external int            */

/* ================================================================
 * Hypercall 编号（与 apps/riscv64/guest_test.S 约定一致）
 *   guest 使用 ecall，a7 = hypercall 号（VS-mode ecall，scause=10）
 * ================================================================ */
#define GUEST_ECALL_DONE   0   /* guest 正常退出       → EL2_VMEXIT  */
#define GUEST_ECALL_PRINT  1   /* 打印迭代计数（a0=iter）→ EL2_RESUME  */

/* ================================================================
 * VS-CSR 访问宏（riscv gcc 工具链通过 CSR 编号直接 csrr/csrw）
 * 注意：-march=rv64gc 已包含 H-ext；若编译器不认识寄存器名
 *       可改用 .insn r SYSTEM, ... 格式，但直接命名更简洁。
 * ================================================================ */

/* hstatus */
#define READ_HSTATUS()          CSR_READ(hstatus)
#define WRITE_HSTATUS(v)        CSR_WRITE(hstatus, v)
#define SET_HSTATUS(bits)       CSR_SET(hstatus, bits)
#define CLEAR_HSTATUS(bits)     CSR_CLEAR(hstatus, bits)

/* hedeleg / hideleg */
#define WRITE_HEDELEG(v)        CSR_WRITE(hedeleg, v)
#define WRITE_HIDELEG(v)        CSR_WRITE(hideleg, v)

/* hgatp (Stage-2 page table) */
#define READ_HGATP()            CSR_READ(hgatp)
#define WRITE_HGATP(v)          CSR_WRITE(hgatp, v)

/* VS-mode 寄存器（HS-mode 可直接读写）*/
#define READ_VSSTATUS()         CSR_READ(vsstatus)
#define WRITE_VSSTATUS(v)       CSR_WRITE(vsstatus, v)

#define READ_VSIE()             CSR_READ(vsie)
#define WRITE_VSIE(v)           CSR_WRITE(vsie, v)

#define READ_VSTVEC()           CSR_READ(vstvec)
#define WRITE_VSTVEC(v)         CSR_WRITE(vstvec, v)

#define READ_VSSCRATCH()        CSR_READ(vsscratch)
#define WRITE_VSSCRATCH(v)      CSR_WRITE(vsscratch, v)

#define READ_VSEPC()            CSR_READ(vsepc)
#define WRITE_VSEPC(v)          CSR_WRITE(vsepc, v)

#define READ_VSCAUSE()          CSR_READ(vscause)
#define WRITE_VSCAUSE(v)        CSR_WRITE(vscause, v)

#define READ_VSTVAL()           CSR_READ(vstval)
#define WRITE_VSTVAL(v)         CSR_WRITE(vstval, v)

#define READ_VSATP()            CSR_READ(vsatp)
#define WRITE_VSATP(v)          CSR_WRITE(vsatp, v)

/* htval（Stage-2 陷阱：guest 物理地址）*/
#define READ_HTVAL()            CSR_READ(htval)

#endif /* RISCV64_HEXT_H */
