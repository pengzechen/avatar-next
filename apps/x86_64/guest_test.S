/*
 * apps/x86_64/guest_test.S — x86_64 VMX non-root guest 测试程序
 *
 * 协议（对标 AArch64 guest_test.S）：
 *   - 无限循环
 *   - HLT  → VMM 步进 RIP，task_yield()，resume（等同 WFI）
 *   - 每 100 次 HLT 后：
 *       VMCALL rdi=VMX_HYPERCALL_PRINT(1), rsi=迭代计数
 *       → VMM 打印计数，task_yield()，resume
 *   - 永不执行 VMX_HYPERCALL_DONE（无限运行 guest）
 *
 * guest 与 host 共享 CR3，运行在同一虚拟地址空间，
 * 因此 x86_guest_test_entry 虚拟地址直接可用（无需 virt_to_phys）。
 */

.text
.globl x86_guest_test_entry
x86_guest_test_entry:
    xorl  %ebx, %ebx        /* rbx = 迭代计数器（初始化为 0）*/

.Lloop:
    hlt                     /* VM exit：VMM 步进 RIP，task_yield，resume */
    incl  %ebx              /* 计数 +1 */

    /* 每 20 次触发一次 VMCALL_PRINT */
    movl  %ebx, %eax
    movl  $20, %ecx
    xorl  %edx, %edx
    divl  %ecx              /* eax = ebx/20, edx = ebx%20 */
    testl %edx, %edx
    jnz   .Lloop            /* edx != 0：不是 20 的倍数，继续 */

    /* 执行 VMCALL VMX_HYPERCALL_PRINT(1)，rsi = 计数 */
    movq  $1, %rdi          /* VMX_HYPERCALL_PRINT = 1 */
    movq  %rbx, %rsi        /* arg1 = 迭代计数 */
    vmcall

    jmp   .Lloop

.section .note.GNU-stack,"",@progbits
