/*
 * mmu_init - 初始化并启用 MMU (x86_64)
 * rdi: 页表基址
 *
 * x86_64 4 级页表
 */
.global mmu_init
mmu_init:
    /* TODO: 实现 x86_64 MMU 初始化 */
    /* 目前直接返回，不启用 MMU */
    ret

/*
 * mmu_disable - 禁用 MMU（调试用）
 */
.global mmu_disable
mmu_disable:
    /* TODO: 实现 x86_64 MMU 禁用 */
    ret
