/*
 * kernel/vmm/riscv64/hext_vcpu.S — RISC-V H-extension VM 入口/出口汇编
 *
 * 提供两个符号：
 *
 * 1) hext_enter_guest(vcpu_t *a0) → int
 *    进入 VS-mode guest 执行一条路径，直到陷入 HS-mode。
 *    返回 1（a0=1）= 正常从 guest exit 返回。
 *
 * 2) hext_trap_vector
 *    guest 运行期间专属陷阱向量（Direct 模式，.align 2）。
 *    从 sscratch 原子取回 vcpu*，保存 guest GPR + VS-CSRs，
 *    恢复 host callee-saved 寄存器，ret 返回给调用者（a0=1）。
 *
 * vcpu_t 固定偏移（与 include/vmm.h VCPU_RV_* 宏完全一致）：
 *   VCPU_R0       =   0  (x0-x31, 32×8B = 256B)
 *   VCPU_VSEPC    = 256
 *   VCPU_VSSTATUS = 264
 *   VCPU_VSTVEC   = 272
 *   VCPU_VSSCRATCH= 280
 *   VCPU_VSATP    = 288
 *   VCPU_VSIE     = 296
 *   VCPU_SCAUSE   = 304
 *   VCPU_STVAL    = 312
 *   VCPU_HTVAL    = 320
 *   VCPU_HSTATUS  = 328  (陷阱时的 hstatus，用于还原 SPVP)
 *   VCPU_PC      = 336  (恢复 PC，来自 HS sepc)
 *   VCPU_HTINST  = 472  (htinst，MMIO 取指回退)
 *   VCPU_HOSTCTX  = 344  (uint64_t host_ctx[16])
 *     [0]ra  [8]s0 ... [96]s11 [104]sp [112]orig_stvec [120]tp
 *
 * sscratch 约定：
 *   进入 guest 前  = vcpu*（陷阱时找到 vcpu）
 *   hext_trap_vector 入口序列：
 *     csrrw a0,sscratch,a0 → a0=vcpu*, sscratch=guest_a0
 *     csrr  t0, sscratch   → t0=guest_a0（立即读，在清零前）
 *     csrw  sscratch, x0   → sscratch=0（恢复内核态约定）
 *     sd    t0, r[10](a0)  → 保存 guest a0
 */

/* vcpu_t 字段偏移 */
#define VCPU_R0         0
#define VCPU_VSEPC      256
#define VCPU_VSSTATUS   264
#define VCPU_VSTVEC     272
#define VCPU_VSSCRATCH  280
#define VCPU_VSATP      288
#define VCPU_VSIE       296
#define VCPU_SCAUSE     304
#define VCPU_STVAL      312
#define VCPU_HTVAL      320
#define VCPU_HSTATUS    328
/* 恢复 PC = 陷入时硬件放在 HS sepc 里的「guest 被打断的那条指令」。
 * 绝不能拿它覆盖 vsepc —— 见陷阱向量第 3 节。*/
#define VCPU_PC         336
/* htinst 放在 host_ctx 之后：不挪动 HCTX_* 偏移，少动一处少一分风险。*/
#define VCPU_HTINST     472

/* host_ctx 字段偏移（VCPU_HOSTCTX = 344，host_ctx[0..15]）*/

/* H-extension CSR 数值地址（-march=rv64gc 汇编器不识别符号名） */
#define CSR_HSTATUS     0x600
#define CSR_VSSTATUS    0x200
#define CSR_VSIE        0x204
#define CSR_VSTVEC      0x205
#define CSR_VSSCRATCH   0x240
#define CSR_VSEPC       0x241
#define CSR_VSATP       0x280
#define CSR_HTVAL       0x643
#define CSR_HTINST      0x64a   /* 触发陷阱的那条指令（htval 是 GPA！）*/
#define HCTX_RA         344
#define HCTX_S0         352
#define HCTX_S1         360
#define HCTX_S2         368
#define HCTX_S3         376
#define HCTX_S4         384
#define HCTX_S5         392
#define HCTX_S6         400
#define HCTX_S7         408
#define HCTX_S8         416
#define HCTX_S9         424
#define HCTX_S10        432
#define HCTX_S11        440
#define HCTX_SP         448
#define HCTX_STVEC      456
#define HCTX_TP         464

.section .text
.align 2

.extern __global_pointer$

/* ──────────────────────────────────────────────────────────────
 * hext_enter_guest(vcpu_t *a0) → int
 * ────────────────────────────────────────────────────────────── */
.global hext_enter_guest
.type hext_enter_guest, @function
hext_enter_guest:
    /* 1. 保存 host callee-saved + sp + ra 到 vcpu->host_ctx */
    sd      ra,  HCTX_RA(a0)
    sd      s0,  HCTX_S0(a0)
    sd      s1,  HCTX_S1(a0)
    sd      s2,  HCTX_S2(a0)
    sd      s3,  HCTX_S3(a0)
    sd      s4,  HCTX_S4(a0)
    sd      s5,  HCTX_S5(a0)
    sd      s6,  HCTX_S6(a0)
    sd      s7,  HCTX_S7(a0)
    sd      s8,  HCTX_S8(a0)
    sd      s9,  HCTX_S9(a0)
    sd      s10, HCTX_S10(a0)
    sd      s11, HCTX_S11(a0)
    sd      sp,  HCTX_SP(a0)

    /*
     * 保存宿主 tp（x4）。
     *
     * tp 不在标准的 caller-saved 集合里，函数调用约定上要求它跨调用保持，
     * 而宿主内核把它当 per-CPU 指针用 —— 下面第 9 步会把 guest 的 x4 装进
     * tp，所以这里不存、陷阱向量里不恢复，宿主一回到 C 代码就会拿 guest 的
     * 值去索引 per-CPU 数据。
     *
     * 实测症状：guest 第一次陷入之后，klog_cpu_id 取址缺页（tval 是 guest
     * 的虚拟地址），宿主陷在「异常处理 → 打印 → 再异常」的循环里，日志上
     * 只看到一串 exec/load page fault，看不出跟虚拟化有关。
     */
    sd      tp,  HCTX_TP(a0)

    /* 2. 保存当前 stvec（主 trap 向量），退出时还原 */
    csrr    t0, stvec
    sd      t0, HCTX_STVEC(a0)

    /* 3. 恢复 VS-CSRs */
    ld      t0, VCPU_VSEPC(a0)
    csrw    CSR_VSEPC, t0

    ld      t0, VCPU_VSSTATUS(a0)
    csrw    CSR_VSSTATUS, t0

    ld      t0, VCPU_VSTVEC(a0)
    csrw    CSR_VSTVEC, t0

    ld      t0, VCPU_VSSCRATCH(a0)
    csrw    CSR_VSSCRATCH, t0

    ld      t0, VCPU_VSATP(a0)
    csrw    CSR_VSATP, t0

    ld      t0, VCPU_VSIE(a0)
    csrw    CSR_VSIE, t0

    /* 4. 安装 VMM 专属陷阱向量（Direct 模式） */
    la      t0, hext_trap_vector
    csrw    stvec, t0

    /* 5. 将 vcpu* 存入 sscratch（guest 陷入时取回） */
    csrw    sscratch, a0

    /* 6. 设置 sepc = vcpu->pc（sret 的跳转目标）
     *
     * 目标取自 **HS sepc**，不是 vsepc：hstatus.SPV=1 时 sret 用的是 HS 的
     * sepc，而 vsepc 是 guest 自己的 sepc（guest 的 sret 回用户态读的就是
     * 它），必须原样还给它。两者混用会冲掉 guest 尚未消费的陷阱现场 ——
     * 见陷阱向量第 3 节。*/
    ld      t0, VCPU_PC(a0)
    csrw    sepc, t0

    /* 7. 设置 sstatus.SPP，SPIE=0
     *
     * SPP 必须跟着上次陷阱时的虚拟化模式走，**不能写死 1**：
     * HS 的 sret 在决定回到哪个特权级时会看 sstatus.SPP，而 SPP 与
     * hstatus.SPVP 的 bit 位置相同（都是 bit8），所以直接用快照里的 SPVP
     * 铺过去。写死 1 的后果是 guest 的**用户态永远回不去** —— 它会一直停在
     * VS-mode 执行用户代码，而 U=1 的页在内核态不可取指，于是 guest 内核报
     * "Unable to handle kernel access to user memory without uaccess routines"。
     * 这条与下面第 8 步的 SPVP 还原是**两件必须同时做的事**，缺一不可。
     */
    csrr    t0, sstatus
    li      t1, ~0x120
    and     t0, t0, t1          /* 清 SPP(bit8)+SPIE(bit5) */
    ld      t1, VCPU_HSTATUS(a0)
    andi    t1, t1, 0x100       /* 取快照里的 SPVP（与 SPP 同 bit 位置）*/
    or      t0, t0, t1
    csrw    sstatus, t0

    /* 8. 设置 hstatus：SPV=1，SPVP 从上次陷阱时的快照还原
     *
     * SPVP 决定这次 sret 回到 VS-mode(1) 还是 VU-mode(0) —— 也就是「回到
     * guest 内核」还是「回到 guest 用户态」。硬件在每次陷入 HS 时把它置成
     * 陷入前的虚拟化模式，而陷阱向量为了不污染宿主的 sret 又必须清掉
     * SPV|SPVP，所以这里只能从快照还原。
     *
     * 早期实现把它**写死成 1**：guest 只要从用户态陷入过一次 HS，之后再恢复
     * 就永远回到 VS-mode —— guest 的用户进程带着用户页表在内核态取指，而
     * U=1 的页在内核态不可执行（SUM 只管访存、不管取指），于是 guest 内核报
     * "Unable to handle kernel access to user memory without uaccess routines"，
     * 现场是内核栈 + 用户的 gp/tp。症状就是 init 刚 exec 到用户代码就 oops。
     */
    csrr    t0, CSR_HSTATUS
    li      t1, ~0x180
    and     t0, t0, t1          /* 清 SPV(bit7)+SPVP(bit8) */
    ori     t0, t0, 0x80        /* SPV=1：sret 进虚拟态 */
    ld      t1, VCPU_HSTATUS(a0)
    andi    t1, t1, 0x100       /* 快照里的 SPVP（bit8 位置对齐）*/
    or      t0, t0, t1
    csrw    CSR_HSTATUS, t0

    /* 9. 加载 guest GPR（a0 = x10 最后加载） */
    ld      x1,  VCPU_R0 +  1*8(a0)
    ld      x2,  VCPU_R0 +  2*8(a0)
    ld      x3,  VCPU_R0 +  3*8(a0)
    ld      x4,  VCPU_R0 +  4*8(a0)
    ld      x5,  VCPU_R0 +  5*8(a0)
    ld      x6,  VCPU_R0 +  6*8(a0)
    ld      x7,  VCPU_R0 +  7*8(a0)
    ld      x8,  VCPU_R0 +  8*8(a0)
    ld      x9,  VCPU_R0 +  9*8(a0)
    ld      x11, VCPU_R0 + 11*8(a0)
    ld      x12, VCPU_R0 + 12*8(a0)
    ld      x13, VCPU_R0 + 13*8(a0)
    ld      x14, VCPU_R0 + 14*8(a0)
    ld      x15, VCPU_R0 + 15*8(a0)
    ld      x16, VCPU_R0 + 16*8(a0)
    ld      x17, VCPU_R0 + 17*8(a0)
    ld      x18, VCPU_R0 + 18*8(a0)
    ld      x19, VCPU_R0 + 19*8(a0)
    ld      x20, VCPU_R0 + 20*8(a0)
    ld      x21, VCPU_R0 + 21*8(a0)
    ld      x22, VCPU_R0 + 22*8(a0)
    ld      x23, VCPU_R0 + 23*8(a0)
    ld      x24, VCPU_R0 + 24*8(a0)
    ld      x25, VCPU_R0 + 25*8(a0)
    ld      x26, VCPU_R0 + 26*8(a0)
    ld      x27, VCPU_R0 + 27*8(a0)
    ld      x28, VCPU_R0 + 28*8(a0)
    ld      x29, VCPU_R0 + 29*8(a0)
    ld      x30, VCPU_R0 + 30*8(a0)
    ld      x31, VCPU_R0 + 31*8(a0)
    ld      x10, VCPU_R0 + 10*8(a0)   /* a0 最后，此后 vcpu* 不可用 */

    /* 10. sret → VS-mode */
    sret

/* ──────────────────────────────────────────────────────────────
 * hext_trap_vector — guest 运行期间专属陷阱向量（HS-mode 入口）
 *
 * 关键：sscratch 管理的正确顺序
 *   csrrw a0, sscratch, a0  → a0=vcpu*, sscratch=guest_a0
 *   csrr  t0, sscratch      → t0=guest_a0（在清零前读！）
 *   csrw  sscratch, x0      → sscratch=0
 *   sd    t0, r[10](a0)     → 保存 guest a0
 * ────────────────────────────────────────────────────────────── */
.global hext_trap_vector
.type hext_trap_vector, @function
.align 2
hext_trap_vector:
    /* 原子取 vcpu*，sscratch 变为 guest_a0 */
    csrrw   a0, sscratch, a0

    /* ⚠️ 先把 guest 的 x5 存掉，再动 t0。
     *
     * t0 就是 x5：下面用 `csrr t0, sscratch` 暂存 guest_a0 的那一刻，
     * guest 的 x5 就没了。原来的代码把 `sd x5` 排在这条 csrr **之后**，
     * 等于每次抢断都把 guest 的 t0 替换成它的 a0（dump 里 t0 == a0）。
     *
     * 用户态受害有限（t0 是 caller-saved），但**抢断落在 guest 内核里时**
     * 就是致命的：guest 内核只会被自己的 vstvec 保存现场，VMM 抢断绕过了
     * 那一步，回来时它的 t0 已经变成 a0 的值 —— 于是算出垃圾地址，
     * 表现为「epc 是用户地址而 SPP=1」或用户态随机 SIGSEGV。*/
    sd      x5,  VCPU_R0 +  5*8(a0)

    /* 立即读出 guest_a0（sscratch 仍是 guest_a0）*/
    csrr    t0, sscratch

    /* 清零 sscratch（内核态 = 0 约定）*/
    csrw    sscratch, x0

    /* 1. 保存 guest GPR → vcpu->r[] */
    sd      x1,  VCPU_R0 +  1*8(a0)
    sd      x2,  VCPU_R0 +  2*8(a0)
    sd      x3,  VCPU_R0 +  3*8(a0)
    sd      x4,  VCPU_R0 +  4*8(a0)
    /* x5 已在本函数开头存过（见上面那条警告），此处不可再存 —— 此刻的
     * x5 已经是 guest_a0 的值了。*/
    sd      x6,  VCPU_R0 +  6*8(a0)
    sd      x7,  VCPU_R0 +  7*8(a0)
    sd      x8,  VCPU_R0 +  8*8(a0)
    sd      x9,  VCPU_R0 +  9*8(a0)
    sd      t0,  VCPU_R0 + 10*8(a0)  /* guest a0（从 t0）*/
    sd      x11, VCPU_R0 + 11*8(a0)
    sd      x12, VCPU_R0 + 12*8(a0)
    sd      x13, VCPU_R0 + 13*8(a0)
    sd      x14, VCPU_R0 + 14*8(a0)
    sd      x15, VCPU_R0 + 15*8(a0)
    sd      x16, VCPU_R0 + 16*8(a0)
    sd      x17, VCPU_R0 + 17*8(a0)
    sd      x18, VCPU_R0 + 18*8(a0)
    sd      x19, VCPU_R0 + 19*8(a0)
    sd      x20, VCPU_R0 + 20*8(a0)
    sd      x21, VCPU_R0 + 21*8(a0)
    sd      x22, VCPU_R0 + 22*8(a0)
    sd      x23, VCPU_R0 + 23*8(a0)
    sd      x24, VCPU_R0 + 24*8(a0)
    sd      x25, VCPU_R0 + 25*8(a0)
    sd      x26, VCPU_R0 + 26*8(a0)
    sd      x27, VCPU_R0 + 27*8(a0)
    sd      x28, VCPU_R0 + 28*8(a0)
    sd      x29, VCPU_R0 + 29*8(a0)
    sd      x30, VCPU_R0 + 30*8(a0)
    sd      x31, VCPU_R0 + 31*8(a0)

    /* 2. 保存 VS-CSRs → vcpu */
    csrr    t0, CSR_VSEPC
    sd      t0, VCPU_VSEPC(a0)

    csrr    t0, CSR_VSSTATUS
    sd      t0, VCPU_VSSTATUS(a0)

    csrr    t0, CSR_VSTVEC
    sd      t0, VCPU_VSTVEC(a0)

    csrr    t0, CSR_VSSCRATCH
    sd      t0, VCPU_VSSCRATCH(a0)

    csrr    t0, CSR_VSATP
    sd      t0, VCPU_VSATP(a0)

    csrr    t0, CSR_VSIE
    sd      t0, VCPU_VSIE(a0)

    /* 3. 保存 HS 陷阱信息
     *
     * ⚠️ sepc 存进 **VCPU_PC**，绝不写进 VCPU_VSEPC。
     *
     * vsepc 是 guest 的 sepc：guest 自己的陷阱（VU→VS）由硬件往那里写现场，
     * guest 的 sret 也读它回用户态。VMM 在 guest 处理程序入口上抢断时，这个
     * 现场还没被读走；若用 HS sepc 覆盖它，guest 之后 sret 就会跳到 VMM 的
     * 恢复点 —— 症状是用户态在 trap 向量上取指（SIGSEGV at vstvec），或者
     * 反过来 epc 是用户地址而 SPP=1。
     * 恢复 PC 有独立字段 VCPU_PC，只写回 HS sepc（见入口第 6 步）。*/
    csrr    t0, sepc
    sd      t0, VCPU_PC(a0)

    csrr    t0, scause
    sd      t0, VCPU_SCAUSE(a0)

    csrr    t0, stval
    sd      t0, VCPU_STVAL(a0)

    csrr    t0, CSR_HTVAL
    sd      t0, VCPU_HTVAL(a0)

    /* htinst：H 扩展提供的「触发陷阱的那条指令」，MMIO 模拟取指失败时的
     * 回退来源。⚠️ 不要拿 htval 顶替 —— htval 存的是出错 GPA>>2，不是指令。
     * 详见 hext_run.c mmio_instruction()。*/
    csrr    t0, CSR_HTINST
    sd      t0, VCPU_HTINST(a0)

    csrr    t0, CSR_HSTATUS
    sd      t0, VCPU_HSTATUS(a0)   /* 快照 SPVP：入口要还原 */

    /* 3b. 清 hstatus.SPV(bit7)+SPVP(bit8)
     *
     * 硬件在 VS-mode → HS-mode 陷入时设置 hstatus.SPV=1，且不会自动清零。
     * 若不清，host 端任何后续 sret（无论 sched_check_and_yield 切到哪条
     * 路径）都会被误判为"返回虚拟态"，造成 sret 进入 VS/VU-mode：
     *   - 调度回内核任务后 trap_vector 出口（SPP=1）→ 进入 VS-mode
     *   - 调度到用户进程时若漏清 → sret 进入 VU-mode → vsatp(=0) 翻译失败 → Inst PF @ 0x10000
     * 在此处一次性清零，使整个 host 处理路径回到普通 HS-mode 语义。
     */
    csrr    t0, CSR_HSTATUS
    li      t1, ~0x180
    and     t0, t0, t1
    csrw    CSR_HSTATUS, t0

    /* 4. 恢复原主 stvec */
    ld      t0, HCTX_STVEC(a0)
    csrw    stvec, t0

    /* 5. 恢复 host callee-saved + sp */
    ld      ra,  HCTX_RA(a0)
    ld      s0,  HCTX_S0(a0)
    ld      s1,  HCTX_S1(a0)
    ld      s2,  HCTX_S2(a0)
    ld      s3,  HCTX_S3(a0)
    ld      s4,  HCTX_S4(a0)
    ld      s5,  HCTX_S5(a0)
    ld      s6,  HCTX_S6(a0)
    ld      s7,  HCTX_S7(a0)
    ld      s8,  HCTX_S8(a0)
    ld      s9,  HCTX_S9(a0)
    ld      s10, HCTX_S10(a0)
    ld      s11, HCTX_S11(a0)
    ld      sp,  HCTX_SP(a0)
    ld      tp,  HCTX_TP(a0)     /* 恢复宿主 per-CPU 指针（见 hext_enter_guest）*/

    /* 5b. 恢复内核 gp（x3）
     *
     * hext_enter_guest 在步骤 9 中加载了 guest 的 x3（vcpu->r[3]=0），
     * 导致 gp = 0。陷入 HS-mode 后 gp 仍为 guest 值，返回 C 代码前
     * 必须显式重置为内核的 __global_pointer$，否则 gp 相对寻址全部错误。
     *
     * .option norelax 禁止链接器把 la gp, __global_pointer$ 优化成
     * mv gp, gp（当 gp 已损坏时该优化是错误的）。
     */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    /* 6. 返回 1（正常 guest exit）*/
    li      a0, 1
    ret
