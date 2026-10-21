/*
 * RISC-V 64-bit linker script for QEMU virt machine
 *
 * 设计：
 * - 引导段（.text.boot/.bss.boot）放在低地址物理空间，供 MMU 启用前执行
 * - 内核主体链接到高地址 VMA（高半区），但 LMA 仍在低地址连续布局
 */

ENTRY(_start)

SECTIONS
{
    /* KERNEL_LOAD_BASE / KERNEL_VMA_OFF 由 Makefile 通过 --defsym 传入：
     *   KERNEL_LOAD_BASE = PLATFORM_MEM_RAM_BASE + OpenSBI 保留区
     *   KERNEL_VMA_OFF   = 内核高半区偏移（mem_layout.h 同源）
     * 链接脚本不能 include C 头，也不能跑 C 预处理器，所以不在本文件
     * 重复写这两组常量。 */

    /* ── 低地址引导段（MMU 前） ───────────────────────────── */
    . = KERNEL_LOAD_BASE;

    .text.boot : {
        *(.text.boot)
    }

    .data.boot : {
        *(.data.boot)
    }

    .bss.boot (NOLOAD) : {
        *(.bss.boot)
    }

    /* 切换到高地址 VMA，LMA 通过 AT(...-KERNEL_VMA_OFF) 回落到低地址 */
    . = KERNEL_VMA_OFF + .;

    /* 导出内核符号供 C 代码使用（高地址） */
    PROVIDE(__kernel_start = .);

    /*
     * _stext/_etext 界定代码区间，backtrace 用它判断一个地址「像不像返回
     * 地址」—— 落在区间外的值一律不当成栈帧里的返回地址。
     * 见 kernel/debug/backtrace.c。
     */
    .text : AT(ADDR(.text) - KERNEL_VMA_OFF) {
        _stext = .;
        *(.text*)
        _etext = .;
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA_OFF) {
        *(.rodata*)
    }

    .data : AT(ADDR(.data) - KERNEL_VMA_OFF) {
        *(.data*)
    }

    /*
     * Small data section (RISC-V GP-relative optimization)
     * __global_pointer$ must be defined 0x800 bytes into .sdata so that
     * gp-relative accesses can reach ±2KB on both sides.
     */
    .sdata : AT(ADDR(.sdata) - KERNEL_VMA_OFF) {
        __global_pointer$ = . + 0x800;
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2) *(.srodata*)
        *(.sdata .sdata.* .gnu.linkonce.s.*)
    }

    /*
     * ---- 内嵌函数符号表（由 tools/gen_kallsyms.py 生成）----
     *
     * ⚠️ 位置必须严格在这里：.sdata 之后、.bss 之前。
     *
     * 表里的地址取自「不带本表」的那一趟链接（stage-1），只有 .text 里的
     * 函数地址两趟保持不变这张表才有效；构建后的 --verify 会校验。
     *
     * 这里比 x86_64/aarch64 多让了一步：那两边把 .kallsyms 放在 .rodata
     * 之后就够，这边放在 .sdata 之后 —— 目的是让两趟链接之间
     * .text/.rodata/.data/.sdata **一个都不动**，只有 .bss 后移。
     *
     * 为什么这边要更严格：RISC-V 链接器默认开松弛（relaxation），它会根据
     * 最终地址决定一段访问编成一条还是两条指令，也就意味着**代码长度依赖
     * 布局**。布局动得越少，两趟差出几个字节的机会就越小。.bss 后移是安全
     * 的：它离 __global_pointer$ 有 5KB 以上，本来就不走 GP 相对寻址，而
     * medany 的绝对寻址是等长的。
     *
     * x86_64 那边不能照抄这个位置 —— 它必须排在 _edata 之前，否则
     * multiboot 的 load_end_addr 会把这一段漏掉。
     */
    .kallsyms : AT(ADDR(.kallsyms) - KERNEL_VMA_OFF) {
        __kallsyms_start = .;
        KEEP(*(.kallsyms))
        __kallsyms_end = .;
    }

    /*
     * BSS section (zero-initialized data)
     * We need symbols for boot code to clear it
     * Stack is also defined here in boot.S
     */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(.sbss .sbss.*)
        *(.scommon)
        *(COMMON)
        __bss_end = .;
    }

    /*
     * 内核结束标记（用于内存管理）
     */
    __kernel_end = .;

    /* 内核物理加载区间（低地址） */
    PROVIDE(__kernel_load_start = __kernel_start - KERNEL_VMA_OFF);
    PROVIDE(__kernel_load_end   = __kernel_end   - KERNEL_VMA_OFF);

    /*
     * 内核镜像区间（半开），供 boot 层换算成物理地址后交给 PMM 预留。
     *
     * 这里是**虚拟地址**（高半区代码对低物理地址的 ABS 符号没法做
     * PC 相对寻址），换算在 main.c 做。起点必须是 KERNEL_LOAD_BASE 的
     * 高半区别名而不是 __kernel_start：本架构的 .text.boot/.data.boot/
     * .bss.boot 在 0x80200000，__kernel_start 的 LMA 已经是主体段的起点
     * （当前 0x80206000）。用后者会把 boot 段漏在 PMM 的可分配区里，而
     * boot 段里的栈/页表一旦被分配出去就是随机崩溃。
     */
    PROVIDE(__kernel_image_start = KERNEL_LOAD_BASE + KERNEL_VMA_OFF);
    PROVIDE(__kernel_image_end   = __kernel_end);

    /* 低地址别名：供 .text.boot 在 MMU 切换阶段使用 */
    PROVIDE(__bss_start_phys        = __bss_start        - KERNEL_VMA_OFF);
    PROVIDE(__bss_end_phys          = __bss_end          - KERNEL_VMA_OFF);
    PROVIDE(__global_pointer_phys   = __global_pointer$  - KERNEL_VMA_OFF);
    PROVIDE(stack_top_phys          = stack_top          - KERNEL_VMA_OFF);
    PROVIDE(secondary_boot_stacks_phys = secondary_boot_stacks - KERNEL_VMA_OFF);
    PROVIDE(kernel_main_phys        = kernel_main        - KERNEL_VMA_OFF);
    PROVIDE(riscv64_boot_hartid_phys = riscv64_boot_hartid - KERNEL_VMA_OFF);

    /*
     * Discard unwind tables and debug info
     */
    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
