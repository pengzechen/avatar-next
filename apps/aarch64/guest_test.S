/*
 * apps/aarch64/guest_test.S — Phase 2 VMM Guest 程序（无限循环版）
 *
 * 运行在 guest EL1（EL1 MMU 关，Stage-2 identity map）。
 * 无限循环：每次 WFI 陷入后打印一条消息，模拟 guest 持续运行。
 *
 * HVC 协议（与 el2_run.c 保持一致）：
 *   HVC_PRINT (x0=1): x1 = iteration count，VMM 打印
 *   HVC_DONE  (x0=0): 通知 VMM 退出（本版本不使用）
 */

.section .text
.global guest_test_entry
.type guest_test_entry, @function
guest_test_entry:
    mov  x19, #0                /* 循环计数器 */

.Lloop:
    /* ── 模拟 guest EL1 工作 ──────────────────────────── */

    /* WFI：触发 TWI 陷入（EC=0x01），VMM advance PC 后 resume */
    wfi

    /* HVC_PRINT：通知 VMM 打印当前迭代次数 */
    mov  x0, #1                 /* HVC_PRINT */
    mov  x1, x19               /* iteration count */
    hvc  #0

    add  x19, x19, #1
    b    .Lloop                 /* 无限循环 */

    /* unreachable */
1:  b    1b
