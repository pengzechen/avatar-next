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
#define CSR_HSTATUS     0x600
#define CSR_HEDELEG     0x602 /* 异常委托到 VS-mode          */
#define CSR_HIDELEG     0x603 /* 中断委托到 VS-mode          */
#define CSR_HIE         0x604
#define CSR_HTIMEDELTA  0x605
#define CSR_HCOUNTEREN  0x606
#define CSR_HGEIE       0x607
#define CSR_HTVAL       0x643
#define CSR_HIP         0x644
#define CSR_HVIP        0x645
#define CSR_HTINST      0x64a
#define CSR_HGEIP       0xe12
#define CSR_HENVCFG     0x60a
#define CSR_HGATP       0x680 /* Second-level page table (Stage-2) */
#define CSR_HTIMEDELTAH 0x615 /* 32-bit 高位（RV32 only）    */

/* VS-mode CSRs（虚拟的 S-mode 寄存器，HS-mode 可直接读写）*/
#define CSR_VSSTATUS  0x200
#define CSR_VSIE      0x204
#define CSR_VSTVEC    0x205
#define CSR_VSSCRATCH 0x240
#define CSR_VSEPC     0x241
#define CSR_VSCAUSE   0x242
#define CSR_VSTVAL    0x243
#define CSR_VSIP      0x244
#define CSR_VSATP     0x280

/* ================================================================
 * hstatus 字段位定义
 * ================================================================ */
#define HSTATUS_VSBE (1UL << 5) /* VS-mode big-endian              */
#define HSTATUS_GVA  (1UL << 6) /* guest virtual address in htval  */
#define HSTATUS_SPV \
    (1UL << 7) /* Supervisor Previous Virtualization
                                        =1: sret 进入 VS-mode
                                        =0: sret 进入 HS/U-mode          */
#define HSTATUS_SPVP \
    (1UL << 8)                    /* SPV 时的先前 VS priv 级别
                                        =1: VS-mode; =0: VU-mode        */
#define HSTATUS_HU    (1UL << 9)  /* 允许 U-mode 使用 hypervisor 指令 */
#define HSTATUS_VGEIN (6)         /* 虚拟化中断 external 号，shift   */
#define HSTATUS_VTVM  (1UL << 20) /* 陷阱 sfence.vma in VS-mode      */
#define HSTATUS_VTW   (1UL << 21) /* 陷阱 WFI in VS-mode             */
#define HSTATUS_VTSR  (1UL << 22) /* 陷阱 sret in VS-mode            */
#define HSTATUS_VSXL  (32)        /* VS XLEN，shift                  */

/* ================================================================
 * hstatus.SPV + SPVP 组合：进入 VS-mode 时设置
 *   SPV=1: sret 进入 VS-mode
 *   SPVP=1: VS-mode（非 VU-mode）
 * ================================================================ */
#define HSTATUS_SPV_VS (HSTATUS_SPV | HSTATUS_SPVP)

/* ================================================================
 * scause 中 VS-mode ecall 的编号
 * VS-mode ecall: cause=10（与普通 U-mode ecall=8 不同）
 * ================================================================ */
#define CAUSE_VS_ECALL 10u

/* ================================================================
 * hedeleg 建议委托给 VS-mode 的异常掩码
 * 常用：用户 ecall(8)、缺页(12/13/15)、非对齐(0/4/6)
 * ================================================================ */
#define HEDELEG_COMMON \
    ((1UL << 0) |  /* Insn addr misalign          */ \
     (1UL << 3) |  /* Breakpoint                  */ \
     (1UL << 4) |  /* Load addr misalign          */ \
     (1UL << 5) |  /* Load access fault           */ \
     (1UL << 6) |  /* Store addr misalign         */ \
     (1UL << 7) |  /* Store access fault          */ \
     (1UL << 8) |  /* U-mode ecall                */ \
     (1UL << 12) | /* Insn page fault             */ \
     (1UL << 13) | /* Load page fault             */ \
     (1UL << 15))  /* Store page fault            */
/* 注意：bit1(insn access fault) 和 bit2(illegal insn/WFI virtual trap)
 *       不委托：bit2=virtual instruction 陷阱（hstatus.VTW=1 时 WFI）需到 HS-mode */

/* hideleg 建议委托给 VS-mode 的中断掩码（VS-mode 软件/定时器/外部中断）*/
#define HIDELEG_COMMON \
    ((1UL << 2) | /* VS software int            */ \
     (1UL << 6) | /* VS timer int               */ \
     (1UL << 10)) /* VS external int            */

/* ================================================================
 * Hypercall 编号（与 apps/riscv64/guest_test.S 约定一致）
 *   guest 使用 ecall，a7 = hypercall 号（VS-mode ecall，scause=10）
 *   使用魔数 0x584b_00xx（"XK"）避免与真实 SBI 号（0/1/2/…）冲突，
 *   使 VMM 可同时模拟标准 SBI（见 hext_run.c handle_vs_ecall）。
 * ================================================================ */
#define GUEST_ECALL_DONE 0x584b0000UL /* guest 正常退出       → EL2_VMEXIT  */
#define GUEST_ECALL_PRINT \
    0x584b0001UL /* 打印迭代计数（a0=iter）→ EL2_RESUME  */

/* ================================================================
 * SBI（Supervisor Binary Interface）子集 —— 移植自 x-kernel
 *   arch/riscv64/mod.rs。guest 以 a7=ext, a6=func, a0..=args 调用 ecall。
 * ================================================================ */
#define SBI_SUCCESS           0
#define SBI_ERR_NOT_SUPPORTED ((uint64_t)(-2))
/* Legacy 扩展（a7 直接是功能号）*/
#define SBI_LEGACY_SET_TIMER       0x00
#define SBI_LEGACY_CONSOLE_PUTCHAR 0x01
#define SBI_LEGACY_CONSOLE_GETCHAR 0x02
/* Legacy 扩展续 */
#define SBI_LEGACY_SHUTDOWN 0x08

/* SBI 返回码（a0）*/
#define SBI_ERR_FAILED            ((uint64_t)(-1))
#define SBI_ERR_INVALID_PARAM     ((uint64_t)(-3))
#define SBI_ERR_ALREADY_AVAILABLE ((uint64_t)(-6))

/* 现代扩展（a7=EID, a6=FID）*/
#define SBI_EXT_BASE                    0x10
#define SBI_BASE_GET_SPEC_VERSION       0
#define SBI_BASE_GET_IMPL_ID            1
#define SBI_BASE_GET_IMPL_VERSION       2
#define SBI_BASE_PROBE_EXTENSION        3
#define SBI_BASE_GET_MVENDORID          4
#define SBI_BASE_GET_MARCHID            5
#define SBI_BASE_GET_MIMPID             6
#define SBI_EXT_TIME                    0x54494d45UL /* "TIME" */
#define SBI_EXT_RFENCE                  0x52464e43UL /* "RFNC" */
#define SBI_EXT_IPI                     0x735049UL   /* "sPI"  */
#define SBI_EXT_HSM                     0x48534dUL   /* "HSM"  */
#define SBI_EXT_SRST                    0x53525354UL /* "SRST" */
#define SBI_TIME_SET_TIMER              0
#define SBI_HSM_HART_START              0
#define SBI_HSM_HART_STOP               1
#define SBI_HSM_HART_GET_STATUS         2
#define SBI_SRST_RESET                  0
#define SBI_SRST_RESET_TYPE_SHUTDOWN    0
#define SBI_SRST_RESET_TYPE_COLD_REBOOT 1
#define SBI_SRST_RESET_REASON_NONE      0

/* ================================================================
 * H-extension CSR 访问宏
 *
 * ⚠️⚠️ 这里**一律用数值 CSR 地址**，一个符号名都不能用。
 *
 * 原因不是「符号名不认」，而是更糟的「符号名认，但含义是错的」：
 * 本工具链是 `-march=rv64gc`（**不含 h**），而让 binutils 认识这些名字的
 * 唯一办法就是把 h 写进 -march —— 但 gcc 11.2.1 直接拒绝
 * （"-march=rv64gch: name of hypervisor extension must be more than 1 letter"，
 *  rv64gc_h / rv64imafdc_h 也都不认）。于是 binutils 落到「无 H 扩展」的
 * CSR 表上，把 hypervisor 寄存器名**静默**映射到同名 VS 级的那个编号：
 *
 *     写的名字    汇编出来的 CSR     实际含义
 *     hstatus  →  0x200            vsstatus     （不是 0x600！）
 *     hedeleg  →  0x202            vsedeleg
 *     hideleg  →  0x203            vsideleg
 *     hie      →  0x204            vsie
 *     hgatp / hvip / hcounteren / vsstatus ...  → 直接汇编报错
 *
 * 这是一个**静默的错误代码生成**，编译链接全绿，直到运行期才以
 * 「illegal instruction」（QEMU 8.2 没实现 vsedeleg/vsideleg）或「行为诡异」
 * 的形式炸出来。本文件历史上就踩过：`WRITE_HSTATUS(...|HSTATUS_VTW)` 实际写的是
 * vsstatus，VTW 位（bit21）在 vsstatus 里是保留位 —— 也就是**从来没生效过**；
 * 而"写 hedeleg 会 illegal instruction"的旧注释，真相是那条指令其实在写
 * 0x202（未实现），不是 QEMU 不允许写 hedeleg。
 *
 * 教训：在这棵树上，H 扩展的 CSR **只写数字**。新增访问前先在
 * `riscv64-linux-musl-objdump -d` 里确认一下编码。
 *
 * 下面用两级宏把参数强制展开成数字再字符串化：C 的 # 运算符不会展开
 * 宏参数，直接写 `"csrw " #csr` 会拼出字面量 "csr" 而不是它的值。
 * ================================================================ */
#define RV_STR_INNER(x) #x
#define RV_STR(x)       RV_STR_INNER(x)

#define RV_CSR_READ(csr) \
    __extension__({ \
        uint64_t _rv_v; \
        __asm__ volatile("csrr %0, " RV_STR(csr) : "=r"(_rv_v)); \
        _rv_v; \
    })
#define RV_CSR_WRITE(csr, val) \
    __asm__ volatile("csrw " RV_STR(csr) ", %0" ::"r"((uint64_t)(val)) \
                     : "memory")
#define RV_CSR_SET(csr, bits) \
    __asm__ volatile("csrs " RV_STR(csr) ", %0" ::"r"((uint64_t)(bits)) \
                     : "memory")
#define RV_CSR_CLEAR(csr, bits) \
    __asm__ volatile("csrc " RV_STR(csr) ", %0" ::"r"((uint64_t)(bits)) \
                     : "memory")

/* hstatus（数值 0x600 —— 写符号名会变成 0x200=vsstatus，见上）*/
#define READ_HSTATUS()      RV_CSR_READ(CSR_HSTATUS)
#define WRITE_HSTATUS(v)    RV_CSR_WRITE(CSR_HSTATUS, v)
#define SET_HSTATUS(bits)   RV_CSR_SET(CSR_HSTATUS, bits)
#define CLEAR_HSTATUS(bits) RV_CSR_CLEAR(CSR_HSTATUS, bits)

/* hedeleg / hideleg（数值 0x602 / 0x603）*/
#define WRITE_HEDELEG(v) RV_CSR_WRITE(CSR_HEDELEG, v)
#define READ_HEDELEG()   RV_CSR_READ(CSR_HEDELEG)
#define WRITE_HIDELEG(v) RV_CSR_WRITE(CSR_HIDELEG, v)
#define READ_HIDELEG()   RV_CSR_READ(CSR_HIDELEG)

/* hie（数值 0x604）*/
#define WRITE_HIE(v)  RV_CSR_WRITE(CSR_HIE, v)
#define SET_HIE(bits) RV_CSR_SET(CSR_HIE, bits)

/* hgatp（数值）*/
#define READ_HGATP()   RV_CSR_READ(CSR_HGATP)
#define WRITE_HGATP(v) RV_CSR_WRITE(CSR_HGATP, v)

/* hcounteren（数值）：不置位的话 guest 读 time/cycle/instret 会非法指令 */
#define READ_HCOUNTEREN()   RV_CSR_READ(CSR_HCOUNTEREN)
#define WRITE_HCOUNTEREN(v) RV_CSR_WRITE(CSR_HCOUNTEREN, v)
#define HCOUNTEREN_CY_TM_IR 0x7UL /* CY | TM | IR */

/* VS-mode 寄存器（HS-mode 可直接读写，一律数值）*/
#define READ_VSSTATUS()   RV_CSR_READ(CSR_VSSTATUS)
#define WRITE_VSSTATUS(v) RV_CSR_WRITE(CSR_VSSTATUS, v)

#define READ_VSIE()   RV_CSR_READ(CSR_VSIE)
#define WRITE_VSIE(v) RV_CSR_WRITE(CSR_VSIE, v)

#define READ_VSTVEC()   RV_CSR_READ(CSR_VSTVEC)
#define WRITE_VSTVEC(v) RV_CSR_WRITE(CSR_VSTVEC, v)

#define READ_VSSCRATCH()   RV_CSR_READ(CSR_VSSCRATCH)
#define WRITE_VSSCRATCH(v) RV_CSR_WRITE(CSR_VSSCRATCH, v)

#define READ_VSEPC()   RV_CSR_READ(CSR_VSEPC)
#define WRITE_VSEPC(v) RV_CSR_WRITE(CSR_VSEPC, v)

#define READ_VSCAUSE()   RV_CSR_READ(CSR_VSCAUSE)
#define WRITE_VSCAUSE(v) RV_CSR_WRITE(CSR_VSCAUSE, v)

#define READ_VSTVAL()   RV_CSR_READ(CSR_VSTVAL)
#define WRITE_VSTVAL(v) RV_CSR_WRITE(CSR_VSTVAL, v)

#define READ_VSATP()   RV_CSR_READ(CSR_VSATP)
#define WRITE_VSATP(v) RV_CSR_WRITE(CSR_VSATP, v)

/* htval（Stage-2 陷阱：guest 物理地址，数值）*/
#define READ_HTVAL() RV_CSR_READ(CSR_HTVAL)

/*
 * 注：**不要**试图在这里读 misa 来判断 H 扩展。
 *
 * misa 的 CSR 地址是 0x301（bits[11:10]=11，机器级），S/HS-mode 读它一律
 * 触发 illegal instruction —— 实测确认（stval 就是那条 csrr 的编码
 * 0x30102af3）。它只在 M-mode（OpenSBI）里可读：OpenSBI 启动时打印的
 * "Boot HART Base ISA : rv64imafdch" 就是从 misa 来的。
 *
 * 所以「有没有 H 扩展」只能靠**试探性读一个 hypervisor CSR** 来判断：
 * 读成功即有，非法指令即无。见 hext_run.c 的 hext_check_support()。
 */

/* ================================================================
 * 虚拟中断注入（CSR hvip / hie）
 *
 * 移植自 x-kernel arch/riscv64/mod.rs：
 *   hvip 的 VS 位由 HS-mode 置位后，VS-mode 看到对应的 sip 位挂起，
 *   从而实现「不依赖 vPLIC/vGIC 的定时器与外部中断注入」。
 * ================================================================ */
#define HVIP_VSTIP (1UL << 6)  /* VS timer 中断挂起      */
#define HVIP_VSEIP (1UL << 10) /* VS external 中断挂起   */
#define HIE_VSTIE  (1UL << 6)  /* VS timer 中断使能      */
#define HIE_VSEIE  (1UL << 10) /* VS external 中断使能   */

/*
 * 注：本工具链（gcc/as）的 RISC-V 汇编器不识别 hvip/hie 的符号名
 *     （与 hgatp 同理），必须用数值 CSR 地址 + 内联汇编。
 */
#define CSR_HVIP_NUM 0x645u
#define CSR_HIE_NUM  0x604u

static inline uint64_t hext_read_hvip(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, 0x645" : "=r"(v)::"memory");
    return v;
}

static inline void hext_set_hvip_bits(uint64_t bits)
{
    __asm__ volatile("csrs 0x645, %0" ::"r"(bits) : "memory");
}

static inline void hext_clear_hvip_bits(uint64_t bits)
{
    __asm__ volatile("csrc 0x645, %0" ::"r"(bits) : "memory");
}

static inline void hext_set_hie_bits(uint64_t bits)
{
    __asm__ volatile("csrs 0x604, %0" ::"r"(bits) : "memory");
}

/* 设置/清除 VS 定时器中断挂起 */
static inline void hext_set_vs_timer_irq(int pending)
{
    if (pending)
        hext_set_hvip_bits(HVIP_VSTIP);
    else
        hext_clear_hvip_bits(HVIP_VSTIP);
}

/* 设置/清除 VS 外部中断挂起 */
static inline void hext_set_vs_external_irq(int pending)
{
    if (pending)
        hext_set_hvip_bits(HVIP_VSEIP);
    else
        hext_clear_hvip_bits(HVIP_VSEIP);
}

#endif /* RISCV64_HEXT_H */
