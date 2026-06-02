/*
 * AArch64 linker script
 *
 * 内核加载和运行策略：
 * - 链接地址由平台配置通过 KERNEL_LINK_ADDR 传入
 *   QEMU virt: 0xffff000040080000（PA 0x40080000）
 *   RK3588:    0xffff000000400000（PA 0x00400000）
 *
 * 流程：
 * 1. 平台加载器将内核加载到 KERNEL_LINK_ADDR - KERNEL_VMA
 * 2. 内核在物理地址下运行（boot.S 用低 32 位恢复物理地址）
 * 3. 初始化页表（vm_init）
 * 4. 启用 MMU（mmu_init）
 * 5. 页表映射将物理加载地址映射到 KERNEL_LINK_ADDR
 * 6. 继续运行（虚拟地址自动生效）
 */

ENTRY(_start)

SECTIONS
{
    /*
        * 平台相关链接地址，由 Makefile 通过 --defsym 传入。
        * 默认值保留 QEMU virt 的加载地址，避免裸 ld 调用时没有符号。
     */
    . = DEFINED(KERNEL_LINK_ADDR) ? KERNEL_LINK_ADDR : 0xffff000040080000;

    /* 导出内核符号供 C 代码使用 */
    PROVIDE(__kernel_start = .);

    /*
     * Boot code section
     * Must be first for QEMU to find entry point
     */
    .text : {
        *(.text)
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
        *(.sbss .sbss.*)
        *(.scommon)
        *(COMMON)
        __bss_end = .;
    }

    /*
     * Stack allocation
     * 32KB stack, grows downward (enlarged for Lua VM call frames)
     *
     * 注意：stack 必须在 __kernel_end 之前，否则 PMM 把这段当作可用物理内存，
     * ELF loader/分配器会覆写 BSP 的 boot 栈，导致 idle/boot 任务上下文被踩。
     */
    . = ALIGN(16);
    . = . + 0x8000;  /* 32KB */
    stack_top = .;
    stack_bottom = . - 0x8000;

    /*
     * 内核结束标记（用于内存管理：[__kernel_start, __kernel_end] 区间被 PMM 保留）
     */
    __kernel_end = .;

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
