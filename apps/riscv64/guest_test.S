/*
 * apps/riscv64/guest_test.S — VS-mode guest 测试程序
 *
 * 运行在 RISC-V H-extension VS-mode 下。
 * 等价于 AArch64 guest_test.S（HVC 循环）和 x86_64 guest_test.S（HLT/VMCALL 循环）。
 *
 * 行为：
 *   1. 无限 WFI 循环（WFI 在 hstatus.VTW=1 时陷入 HS-mode，等价 AArch64 WFI trap）
 *   2. 每执行 20 次 WFI，发出一次 ecall(GUEST_ECALL_PRINT, iter)
 *   3. 永不调用 GUEST_ECALL_DONE，无限循环
 *
 * VS-mode ecall 约定（与 HS-mode hext_run.c 配合）：
 *   a7 = hypercall 号（0=DONE, 1=PRINT）
 *   a0 = 参数（PRINT 时为 iter 计数）
 *   ecall 指令 → scause=10（VS-mode ecall），VMM 捕获后处理
 *
 * 注意：VS-mode 没有页表（vsatp=0），代码地址由 VMM 直接传入 vsepc，
 *       因此用 auipc/addi 做 PC 相对寻址，不依赖重定位。
 */

/* Hypercall 编号（与 include/riscv64/hext.h GUEST_ECALL_* 一致）*/
#define GUEST_ECALL_DONE   0
#define GUEST_ECALL_PRINT  1

.section .text
.align 2
.global rv_guest_test_entry

rv_guest_test_entry:
    /* s0 = 迭代计数器 */
    li      s0, 0

.Lwfi_loop:
    /*
     * WFI：hstatus.VTW=1 时陷入 HS-mode。
     * VMM 在 exit handler 中步进 sepc += 4，task_yield，然后 resume。
     */
    wfi

    /* 计数 +1 */
    addi    s0, s0, 1

    /* 每 20 次发一次 PRINT hypercall */
    li      t0, 20
    rem     t1, s0, t0
    bnez    t1, .Lwfi_loop

    /* ecall(GUEST_ECALL_PRINT, iter=s0) */
    li      a7, GUEST_ECALL_PRINT
    mv      a0, s0
    ecall

    j       .Lwfi_loop
