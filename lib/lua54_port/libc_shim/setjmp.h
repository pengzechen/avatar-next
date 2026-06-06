/* lib/lua54_port/libc_shim/setjmp.h — arch-specific jmp_buf for freestanding Lua */
#ifndef _COMPAT_SETJMP_H
#define _COMPAT_SETJMP_H

#include <types.h>   /* uint64_t */

#if defined(__aarch64__)
/* Save: x19-x28, x29 (fp), x30 (lr), sp, d8-d15 = 12 + 1 + 1 + 8 = 22 regs
 * We use 24 uint64_t for alignment padding. */
typedef uint64_t jmp_buf[24];

#elif defined(__riscv) && __riscv_xlen == 64
/* Save: ra, sp, s0-s11, fs0-fs11 = 2 + 12 + 12 = 26, pad to 32. */
typedef uint64_t jmp_buf[32];

#else  /* x86_64 */
/* Save: rbx, rbp, r12-r15, rsp, rip = 8 regs */
typedef uint64_t jmp_buf[8];
#endif

/* Assembly implementations in lib/setjmp/setjmp_<arch>.S */
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* _COMPAT_SETJMP_H */
