/*
 * kernel/task/user.ld - 用户程序链接脚本
 *
 * 将用户程序链接到用户空间地址（0x00000000-0x7FFFFFFF）
 */

OUTPUT_ARCH(aarch64)
ENTRY(user_test_program)

SECTIONS
{
    /* 用户代码段：0x10000（避免 NULL 指针） */
    . = 0x10000;

    .text : {
        *(.text.user)
        *(.text*)
    }

    /* 只读数据段 */
    .rodata : {
        *(.rodata.user)
        *(.rodata*)
    }

    /* 数据段 */
    .data : {
        *(.data.user)
        *(.data*)
    }

    /* BSS 段 */
    .bss : {
        *(.bss.user)
        *(.bss*)
    }

    /* 用户栈：0x70000000（1MB） */
    . = 0x70000000;
    .user_stack : {
        _user_stack_start = .;
        . = . + 0x100000;  /* 1MB 栈 */
        _user_stack_end = .;
    }

    /* 丢弃不需要的段 */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
