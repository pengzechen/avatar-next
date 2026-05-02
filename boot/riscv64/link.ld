/*
 * RISC-V 64-bit linker script for QEMU virt machine
 * Memory layout: RAM starts at 0x80000000
 */

ENTRY(_start)

SECTIONS
{
    /*
     * QEMU virt machine RAM starts at 0x80000000
     * We'll place our kernel at 0x80200000 (leaving space for OpenSBI)
     */
    . = 0x80200000;

    /* 导出内核符号供 C 代码使用 */
    PROVIDE(__kernel_start = .);

    /*
     * Boot code section
     * Must be first for QEMU to find entry point
     */
    .text.boot : {
        *(.text.boot)
    }

    /*
     * Regular code and read-only data
     */
    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    /*
     * Initialized data
     */
    .data : {
        *(.data*)
    }

    /*
     * BSS section (zero-initialized data)
     * We need symbols for boot code to clear it
     * Stack is also defined here in boot.S
     */
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /*
     * 内核结束标记（用于内存管理）
     */
    __kernel_end = .;

    /*
     * Stack symbols (defined in boot.S .bss section)
     * These are automatically placed in .bss section
     */

    /*
     * Discard unwind tables and debug info
     */
    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
