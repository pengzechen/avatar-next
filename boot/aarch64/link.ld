/*
 * AArch64 linker script for QEMU virt machine
 * Memory layout: RAM starts at 0x40000000
 *
 * 内核加载和运行策略：
 * - 加载地址（物理）：0x4008_0000
 * - 链接地址（虚拟）：0x4008_0000（临时，启用 MMU 前运行）
 * - 运行地址（虚拟）：KERNEL_VMA + 0x4008_0000（启用 MMU 后）
 *
 * 流程：
 * 1. QEMU 将内核加载到物理地址 0x4008_0000
 * 2. 内核在物理地址下运行（链接地址 = 物理地址）
 * 3. 初始化页表（vm_init）
 * 4. 启用 MMU（mmu_init）
 * 5. 页表映射将 0x4008_0000 映射到 KERNEL_VMA + 0x4008_0000
 * 6. 继续运行（虚拟地址自动生效）
 */

ENTRY(_start)


SECTIONS
{
    /*
     * 初始链接地址 = 物理地址
     * 启用 MMU 后，相同的虚拟地址会自动映射到高地址
     */
    . = 0xffff000040080000;  

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
