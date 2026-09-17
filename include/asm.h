#ifndef ASM_H
#define ASM_H

/*
 * include/asm.h — 汇编源文件（.S）函数符号定义宏
 *
 * 本文件只含 #define / #if，不含 typedef、struct 等 C 语法，因此可以同时被
 * C 文件与 .S 文件包含（.S 由 gcc 预处理）。
 * 架构判断沿用 include/arch.h 的 ARCH_* 约定（构建系统传入 -DARCH_xxx=1）。
 *
 * 用法:
 *   #include "asm.h"
 *
 *   FUNCS(_start)
 *       ... 代码 ...
 *   FUNCE(_start)
 *
 * 展开后等价于手写:
 *   .globl _start
 *   .type _start, %function
 *   _start:
 *       ... 代码 ...
 *   .size _start, . - _start
 *
 * 说明: 预处理会把宏展开成单行，因此各条汇编指示之间用 ';' 分隔。
 *       GNU as 在 aarch64 / riscv64 / x86_64 上均以 ';' 作为语句分隔符。
 */

#include "arch.h"

/*
 * ELF 函数符号类型指示的分隔字符。
 * AArch64 汇编中 '@' 是注释起始符，故用 '%'；x86_64 / RISC-V 用 '@'。
 * 两者产出的 ELF 符号类型相同（STT_FUNC）。
 */
#if ARCH_AARCH64
#  define ASM_TYPE_CHAR %
#else
#  define ASM_TYPE_CHAR @
#endif

/*
 * FUNCS(name) — 函数开始
 *
 * 导出全局符号、标记 ELF FUNC 类型、落下函数标签。
 * 必须放在函数代码之前，且在 .text 段内。
 */
#define FUNCS(name) \
    .globl name; .type name, ASM_TYPE_CHAR function; name:

/*
 * FUNCE(name) — 函数结束
 *
 * 产生 .size，供调试器与栈回溯确定函数边界。
 * 若函数结尾会 fall-through 到下一段代码（例如不返回的尾调用），省略本宏即可。
 */
#define FUNCE(name) \
    .size name, . - name

/*
 * LFUNCS(name) / LFUNCE(name) — 文件内局部函数
 *
 * 与 FUNCS/FUNCE 相同，但不产生 .globl，符号保持 LOCAL。
 * 用于只在本文件内被 bl/call 调用的子程序：给它正确的 FUNC 类型与函数边界，
 * 同时不把名字导出到链接期（避免与其它翻译单元的同名符号相互干扰）。
 *
 * 用法:
 *   LFUNCS(helper)
 *       ...
 *       ret
 *   LFUNCE(helper)
 */
#define LFUNCS(name) \
    .type name, ASM_TYPE_CHAR function; name:

#define LFUNCE(name) \
    .size name, . - name

#endif  // ASM_H
