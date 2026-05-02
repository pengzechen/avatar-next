/*
 * mmu_init - 初始化并启用 MMU (RISC-V)
 * a0: 页表基址
 *
 * RISC-V Sv39 或 Sv48 分页
 */
.global mmu_init
mmu_init:
    /* TODO: 实现 RISC-V MMU 初始化 */
    /* 目前直接返回，不启用 MMU */
    ret

/*
 * mmu_disable - 禁用 MMU（调试用）
 */
.global mmu_disable
mmu_disable:
    /* TODO: 实现 RISC-V MMU 禁用 */
    ret
