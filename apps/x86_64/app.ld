/*
 * apps/x86_64/app.ld - x86_64 用户应用程序链接脚本
 *
 * 将用户程序链接到虚拟地址 0x10000
 * 注意：x86_64 的用户程序当前嵌入内核，此链接脚本仅用于生成独立 .bin 文件（未来使用）
 */

ENTRY(_start)

SECTIONS
{
    /* 代码段从 0x10000 开始 */
    . = 0x10000;

    .text.user : {
        *(.text.user)
        *(.text)
        *(.text.*)
    }

    /* 只读数据 */
    .rodata.user : {
        *(.rodata.user)
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
