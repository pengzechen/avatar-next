/*
 * RISC-V 64-bit linker script for QEMU virt machine
 *
 * 设计：
 * - 引导段（.text.boot/.bss.boot）放在低地址物理空间，供 MMU 启用前执行
 * - 内核主体链接到高地址 VMA（高半区），但 LMA 仍在低地址连续布局
 */

ENTRY(_start)

SECTIONS
{
    KERNEL_LOAD_BASE = 0x80200000;
    KERNEL_VMA_OFF   = 0xffffffc000000000;

    /* ── 低地址引导段（MMU 前） ───────────────────────────── */
    . = KERNEL_LOAD_BASE;

    .text.boot : {
        *(.text.boot)
    }

    .bss.boot (NOLOAD) : {
        *(.bss.boot)
    }

    /* 切换到高地址 VMA，LMA 通过 AT(...-KERNEL_VMA_OFF) 回落到低地址 */
    . = KERNEL_VMA_OFF + .;

    /* 导出内核符号供 C 代码使用（高地址） */
    PROVIDE(__kernel_start = .);

    .text : AT(ADDR(.text) - KERNEL_VMA_OFF) {
        *(.text*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA_OFF) {
        *(.rodata*)
    }

    .data : AT(ADDR(.data) - KERNEL_VMA_OFF) {
        *(.data*)
    }

    /*
     * Small data section (RISC-V GP-relative optimization)
     * __global_pointer$ must be defined 0x800 bytes into .sdata so that
     * gp-relative accesses can reach ±2KB on both sides.
     */
    .sdata : AT(ADDR(.sdata) - KERNEL_VMA_OFF) {
        __global_pointer$ = . + 0x800;
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2) *(.srodata*)
        *(.sdata .sdata.* .gnu.linkonce.s.*)
    }

    /*
     * BSS section (zero-initialized data)
     * We need symbols for boot code to clear it
     * Stack is also defined here in boot.S
     */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /*
     * 内核结束标记（用于内存管理）
     */
    __kernel_end = .;

    /* 内核物理加载区间（低地址） */
    PROVIDE(__kernel_load_start = __kernel_start - KERNEL_VMA_OFF);
    PROVIDE(__kernel_load_end   = __kernel_end   - KERNEL_VMA_OFF);

    /* 低地址别名：供 .text.boot 在 MMU 切换阶段使用 */
    PROVIDE(__bss_start_phys        = __bss_start        - KERNEL_VMA_OFF);
    PROVIDE(__bss_end_phys          = __bss_end          - KERNEL_VMA_OFF);
    PROVIDE(__global_pointer_phys   = __global_pointer$  - KERNEL_VMA_OFF);
    PROVIDE(stack_top_phys          = stack_top          - KERNEL_VMA_OFF);
    PROVIDE(kernel_main_phys        = kernel_main        - KERNEL_VMA_OFF);

    /*
     * Discard unwind tables and debug info
     */
    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
