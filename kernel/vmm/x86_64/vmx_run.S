/*
 * kernel/vmm/x86_64/vmx_run.S — VMX guest 入口/出口汇编
 *
 * vmx_enter_guest(vcpu_t *vcpu) — 执行一次 VMLAUNCH 或 VMRESUME
 *   rdi = vcpu_t*（vcpu->launched 决定 launch vs resume）
 *   返回 1（rax=1）= 成功进入并已从 VM exit 返回
 *   返回 0（rax=0）= vmlaunch/vmresume 早期失败
 *
 * vcpu_t.regs 偏移（与 include/vmm.h VCPU_X86_* 一致）：
 *   rax +0x00  rbx +0x08  rcx +0x10  rdx +0x18
 *   rbp +0x20  rsi +0x28  rdi +0x30
 *   r8  +0x38  r9  +0x40  r10 +0x48  r11 +0x50
 *   r12 +0x58  r13 +0x60  r14 +0x68  r15 +0x70
 *
 * HOST_RSP VMCS 字段编码 = 0x6c14
 * HOST_RIP VMCS 字段编码 = 0x6c16
 *
 * 栈帧（进入 guest 前，RSP = HOST_RSP = 已推入 vcpu* 的栈顶）：
 *   [rsp+0x00] rdi (= vcpu*)        ← HOST_RSP 指向此处
 *   [rsp+0x08] r15 (host)
 *   [rsp+0x10] r14 (host)
 *   [rsp+0x18] r13 (host)
 *   [rsp+0x20] r12 (host)
 *   [rsp+0x28] rbx (host)
 *   [rsp+0x30] rbp (host)
 *   [rsp+0x38] return address
 */

.text
.globl vmx_enter_guest
.globl vmx_return

/*
 * int vmx_enter_guest(vcpu_t *vcpu)
 */
vmx_enter_guest:
    /* 1. 保存 host callee-saved 寄存器 */
    push  %rbp
    push  %rbx
    push  %r12
    push  %r13
    push  %r14
    push  %r15
    push  %rdi                  /* rdi = vcpu*，保存在栈顶 */

    /* 2. 设置 HOST_RSP = 当前 RSP（vmexit 后恢复到此） */
    movq  %rsp, %rax
    movq  $0x6c14, %rcx         /* HOST_RSP 编码 */
    vmwrite %rax, %rcx

    /* 3. 设置 HOST_RIP = vmx_return */
    leaq  vmx_return(%rip), %rax
    movq  $0x6c16, %rcx         /* HOST_RIP 编码 */
    vmwrite %rax, %rcx

    /* 4. 检查是否已 launched（在加载 guest GPR 之前，rdi 仍有效）
     * x86 vcpu_t 布局：regs(0x80B) → vcpu_id(0x80) → launched(0x84) */
    movl  0x84(%rdi), %r11d     /* r11d = vcpu->launched */
    test  %r11d, %r11d          /* ZF=1 if not launched */

    /* 5. 从 vcpu->regs 加载 guest GPR（先加载除 rdi/rsi 外的所有寄存器）*/
    movq  0x00(%rdi), %rax
    movq  0x08(%rdi), %rbx
    movq  0x10(%rdi), %rcx
    movq  0x18(%rdi), %rdx
    movq  0x20(%rdi), %rbp
    movq  0x38(%rdi), %r8
    movq  0x40(%rdi), %r9
    movq  0x48(%rdi), %r10
    movq  0x58(%rdi), %r12
    movq  0x60(%rdi), %r13
    movq  0x68(%rdi), %r14
    movq  0x70(%rdi), %r15
    /* 最后加载 rsi、rdi（加载后不再能引用 vcpu*）*/
    movq  0x28(%rdi), %rsi
    movq  0x30(%rdi), %rdi      /* ← 此后 rdi = guest rdi，vcpu* 已在栈上 */

    /* 6. VMLAUNCH 或 VMRESUME */
    test  %r11d, %r11d          /* ZF=1 if not launched */
    jnz   .Ldo_resume
    vmlaunch
    jmp   .Lentry_failed
.Ldo_resume:
    vmresume
    /* vmresume 失败则顺序执行到此 */

.Lentry_failed:
    /*
     * vmlaunch/vmresume 早期失败：RSP 未被 CPU 修改，
     * 仍然等于 HOST_RSP，即 &saved_vcpu*（栈顶）。
     * GPR 保存区（vcpu->regs）未改变，无需写回。
     * 直接恢复 host callee-saved 寄存器并返回 0。
     */
    add   $8, %rsp              /* 跳过 saved rdi (vcpu*) */
    pop   %r15
    pop   %r14
    pop   %r13
    pop   %r12
    pop   %rbx
    pop   %rbp
    xor   %eax, %eax            /* 返回 0 = 失败 */
    ret

vmx_return:
    /*
     * VM exit 触发：CPU 恢复 HOST_RSP / HOST_RIP / HOST_CR3 / 段寄存器，
     * 通用寄存器 rax-r15 仍保存着 guest 退出时的值。
     * RSP = HOST_RSP = &saved_vcpu*（栈顶）。
     *
     * 保存 guest GPR 到 vcpu->regs：
     *   1. xchg r15 与 (%rsp)，使 r15 = vcpu*，(%rsp) = guest r15
     *   2. 保存其余 GPR 到 vcpu->regs
     *   3. 将 (%rsp) 读出保存为 guest r15
     *   4. 恢复 host callee-saved 寄存器并返回 1
     */
    xchg  %r15, (%rsp)          /* r15 = vcpu*, (rsp) = guest r15 */

    movq  %rax, 0x00(%r15)
    movq  %rbx, 0x08(%r15)
    movq  %rcx, 0x10(%r15)
    movq  %rdx, 0x18(%r15)
    movq  %rbp, 0x20(%r15)
    movq  %rsi, 0x28(%r15)
    movq  %rdi, 0x30(%r15)
    movq  %r8,  0x38(%r15)
    movq  %r9,  0x40(%r15)
    movq  %r10, 0x48(%r15)
    movq  %r11, 0x50(%r15)
    movq  %r12, 0x58(%r15)
    movq  %r13, 0x60(%r15)
    movq  %r14, 0x68(%r15)
    movq  (%rsp), %rax          /* rax = guest r15（xchg 后放在此处）*/
    movq  %rax, 0x70(%r15)

    /* 恢复 host callee-saved 寄存器 */
    add   $8, %rsp              /* 跳过 guest r15 槽 */
    pop   %r15
    pop   %r14
    pop   %r13
    pop   %r12
    pop   %rbx
    pop   %rbp

    mov   $1, %eax              /* 返回 1 = 成功 */
    ret

.section .note.GNU-stack,"",@progbits
