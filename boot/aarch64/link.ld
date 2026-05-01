/*
 * AArch64 linker script for QEMU virt machine
 * Memory layout: RAM starts at 0x40000000
 */

ENTRY(_start)

SECTIONS
{
    /*
     * QEMU virt machine RAM starts at 0x40000000
     * We'll place our kernel there
     */
    . = 0x40080000;

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
     */
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /*
     * Stack allocation
     * 16KB stack, grows downward
     */
    . = ALIGN(16);
    . = . + 0x4000;  /* 16KB */
    stack_top = .;
    stack_bottom = . - 0x4000;

    /*
     * Discard unwind tables and debug info
     */
    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
