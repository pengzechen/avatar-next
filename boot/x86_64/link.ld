/*
 * x86_64 linker script for QEMU Q35 machine
 *
 * Memory layout
 * -------------
 *   Physical base : 0x00200000  (loaded here by multiboot)
 *   Virtual  base : 0xffff800000200000  (high half)
 *   Offset        : 0xffff800000000000
 *
 * The multiboot header (bit 16 set) provides the physical load
 * addresses, so the ELF program-header virtual addresses are
 * not used by the bootloader for loading.
 *
 * OUTPUT_ARCH ensures GNU ld emits a proper x86-64 ELF and
 * avoids the 2 MB file-offset that breaks QEMU's 8 KB header scan.
 * The Makefile also passes -Wl,-z,max-page-size=0x1000 so that
 * segment file offsets are page-aligned on 4 KB boundaries.
 */

OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

SECTIONS
{
    /* Virtual base (= physical 0x200000 + PHYS_OFFSET) */
    . = 0xffff800000200000;
    _skernel = .;

    /* 导出内核符号供 C 代码使用 */
    PROVIDE(__kernel_start = .);

    /* ---- text ---- */
    .text : ALIGN(4K) {
        _stext = .;
        *(.text.boot)        /* boot entry + multiboot hdr + page tables */
        *(.text .text.*)
        . = ALIGN(4K);
        _etext = .;
    }

    /* ---- read-only data ---- */
    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4K);
    _erodata = .;

    /* ---- initialised data ---- */
    .data : ALIGN(4K) {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4K);
    }

    . = ALIGN(4K);
    _edata = .;          /* end of file-backed data (multiboot load_end_addr) */

    /* ---- BSS (zero-initialised; not loaded from file) ---- */
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
        _ebss = .;       /* multiboot bss_end_addr */
    }

    /*
     * 内核结束标记（用于内存管理）
     */
    __kernel_end = .;

    _ekernel = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame .eh_frame_hdr)
        *(.note*)
    }
}
