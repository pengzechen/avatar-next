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

    /*
     * ---- 内嵌函数符号表（由 tools/gen_kallsyms.py 生成）----
     *
     * ⚠️ 位置是有约束的，别挪：
     *
     * 表里的地址取自「不带本表」的那一趟链接（stage-1）。之所以两趟能对
     * 得上，是因为 .kallsyms 排在 .text **之后** —— 新增这么一段不会移动
     * 任何函数。表里也只收 STT_FUNC（全在 .text 内）；数据符号的地址两趟
     * 之间确实会变，所以不能收。
     *
     * 放到 .text 之前会让所有函数地址整体后移，panic backtrace 打出的是
     * 一堆对不上号的函数名 —— 构建里的 `gen_kallsyms.py --verify` 就是
     * 专门挡这个的。
     *
     * 也不能放到 .bss 之后：.bss 是 NOBITS，`objcopy -O binary` 会在中间
     * 填出几 MB 的洞。
     */
    .kallsyms : ALIGN(8) {
        __kallsyms_start = .;
        KEEP(*(.kallsyms))
        __kallsyms_end = .;
    }

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
        *(.sbss .sbss.*)
        *(.scommon)
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
