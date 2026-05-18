#include "../../../include/aarch64/tcr.h"
#include "../../../include/aarch64/mair.h"

/*
 * mmu_init - 初始化并启用 MMU
 * x0: TTBR0_EL1 值（用户空间页表）
 * x1: TTBR_EL1 值（内核空间页表，物理地址）
 *
 * 配置：
 * - 48 位虚拟地址
 * - TTBR0：用户空间（0x0000_0000_0000_0000 - 0x0000_ffff_ffff_ffff）
 * - TTBR1：内核空间（0xffff_0000_0000_0000 - 0xffff_ffff_ffff_ffff）
 * - MAIR：设备内存、普通内存、无缓存内存
 */
.global mmu_init
mmu_init:
    /*
     * 先关闭 MMU（ATF/U-Boot 可能在移交时已经启用 MMU 并使用其自己的页表；
     * 如果不关掉，下面切换 TTBRx/TCR_EL1 会立即触发取指 translation fault，
     * VBAR_EL1 此时通常未设，CPU 直接挂死）。此时 PC 处于低物理地址区间，
     * 关 MMU 后仍可正常取指。注意：只清 M 位，保留 I/C cache 状态不变，
     * 避免在 BSP 路径上引入 cache-disable 副作用。
     */
    mrs     x2, sctlr_el1
    bic     x2, x2, #(1 << 0)         /* M  - MMU disable */
    msr     sctlr_el1, x2
    isb

    /* 切换 TTBR 前先刷 TLB，避免 U-Boot 遗留的 TLB 项干扰 */
    tlbi    alle2is
    dsb     sy
    isb

    /* 设置 TTBR0_EL1（用户空间页表） */
    msr ttbr0_el1, x0

    /* 设置 TTBR1_EL1（内核空间页表） */
    msr ttbr1_el1, x1

    ldr x0, =(TCR_EL1)
    msr tcr_el1, x0

    ldr x0, =(MAIR_VALUE)
    msr mair_el1, x0

    /*
     * 启用 MMU。同时打开 DCache (C) 和 ICache (I)：
     * 必须在切 SCTLR.M=1 这条指令"之前"DCache 已开，否则 page-table
     * walker 在 SCTLR.C=0 时把 cacheable 访问降级为 non-cacheable，
     * 直接读 RAM，看不到 BSP 仍在 DCache 里的页表项（inner-shareable
     * 一致性只对参与 snoop 的核生效，参与的前提是本核 DCache 已开）。
     * 在 secondary CPU 上这是 PSCI 拉起后 mmu_init 一直挂死的根因。
     */
    mrs x2, sctlr_el1
    orr x2, x2, #(1 << 0)      /* M  - MMU 使能 */
    orr x2, x2, #(1 << 2)      /* C  - DCache 使能（必须，否则 walker 看不到页表） */
    orr x2, x2, #(1 << 12)     /* I  - ICache 使能 */
    orr x2, x2, #(1 << 23)     /* SPAN=1 - 陷入 EL2 时不自动设置 PAN */
    msr sctlr_el1, x2

    /* 内存屏障 */
    dsb sy
    isb

    ret

/*
 * mmu_disable - 禁用 MMU（调试用）
 */
.global mmu_disable
mmu_disable:
    mrs x0, sctlr_el1
    bic x0, x0, #(1 << 0)      /* SCTLR_EL1.M - MMU 禁用 */
    bic x0, x0, #(1 << 2)      /* SCTLR_EL1.C - 数据缓存禁用 */
    bic x0, x0, #(1 << 12)     /* SCTLR_EL1.I - 指令缓存禁用 */
    msr sctlr_el1, x0

    /* 内存屏障 */
    dsb sy
    isb

    ret
