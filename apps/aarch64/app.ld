/*
 * apps/app.ld - 用户应用程序链接脚本
 *
 * 将用户程序链接到虚拟地址 0x10000
 */

ENTRY(_start)

SECTIONS
{
    /* 代码段从 0x10000 开始 */
    . = 0x10000;

    .text : {
        *(.text)
        *(.text.*)
    }

    /* 只读数据 */
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    /* 数据段 */
    .data : {
        *(.data)
        *(.data.*)
    }

    /* BSS 段 */
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /* 栈在运行时分配 */
}
