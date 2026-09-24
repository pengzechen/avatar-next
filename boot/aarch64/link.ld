/*
 * AArch64 linker script
 *
 * 内核加载和运行策略：
 * - 链接地址由平台配置通过 KERNEL_LINK_ADDR 传入
 *   QEMU virt: 0xffff000040080000（PA 0x40080000）
 *   RK3588:    0xffff000000400000（PA 0x00400000）
 *
 * 流程：
 * 1. 平台加载器将内核加载到 KERNEL_LINK_ADDR - KERNEL_VMA_OFF
 * 2. 内核在物理地址下运行（boot.S 使用本脚本导出的 *_phys 符号）
 * 3. 初始化页表（vm_init）
 * 4. 启用 MMU（mmu_init）
 * 5. 页表映射将物理加载地址映射到 KERNEL_LINK_ADDR
 * 6. 继续运行（虚拟地址自动生效）
 */

ENTRY(_start)

SECTIONS
{
    KERNEL_VMA_OFF = 0xffff000000000000;

    /*
     * 平台相关链接地址，由 Makefile 通过 --defsym 传入。
     * 默认值保留 QEMU virt 的加载地址，避免裸 ld 调用时没有符号。
     */
    . = DEFINED(KERNEL_LINK_ADDR) ? KERNEL_LINK_ADDR : 0xffff000040080000;

    /* 导出内核符号供 C 代码使用 */
    PROVIDE(__kernel_start = .);

    /*
     * _stext/_etext 界定代码区间，backtrace 用它判断一个地址「像不像返回
     * 地址」—— 落在区间外的值一律不当成栈帧里的返回地址。
     * 见 kernel/debug/backtrace.c。
     */
    .text : {
        _stext = .;
        *(.text.boot)
        *(.text .text.*)
        _etext = .;
    }

    .rodata : {
        *(.rodata*)
    }

    /*
     * ---- 内嵌函数符号表（由 tools/gen_kallsyms.py 生成）----
     *
     * ⚠️ 必须排在 .text 之后、.data 之前，理由见 boot/x86_64/link.ld 里
     * 同一段的长注释：表里的地址取自不带本表的那一趟链接，只有 .text
     * 不移动它们才有效。--verify 会在构建时校验。
     */
    .kallsyms : ALIGN(8) {
        __kallsyms_start = .;
        KEEP(*(.kallsyms))
        __kallsyms_end = .;
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
        *(.sbss .sbss.*)
        *(.scommon)
        *(COMMON)
        __bss_end = .;
    }

    /*
     * 内核结束标记（用于内存管理：[__kernel_start, __kernel_end] 区间被 PMM 保留）
     */
    __kernel_end = .;

    /* 物理地址别名：供 MMU 开启前的 boot.S 使用，避免截断高 VA。 */
    PROVIDE(__kernel_load_start = __kernel_start - KERNEL_VMA_OFF);
    PROVIDE(__kernel_load_end   = __kernel_end   - KERNEL_VMA_OFF);
    PROVIDE(__bss_start_phys    = __bss_start    - KERNEL_VMA_OFF);
    PROVIDE(__bss_end_phys      = __bss_end      - KERNEL_VMA_OFF);
    PROVIDE(stack_top_phys      = stack_top      - KERNEL_VMA_OFF);
    PROVIDE(secondary_boot_stacks_phys = secondary_boot_stacks - KERNEL_VMA_OFF);

    /*
     * Discard unwind tables and debug info
     */
    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }

    PROVIDE(__heap_flag = .);
}
