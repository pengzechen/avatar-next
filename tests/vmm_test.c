/*
 * tests/vmm_test.c — 三架构 VMM 三线程并发测试
 *
 * 测试目标：验证 VMM（虚拟机监控器）能正确创建并调度三类并发线程：
 *   Thread 1: 宿主内核监控循环（EL2 / HS-mode / VMX root）
 *   Thread 2: vCPU guest 线程（EL1 guest / VS-mode / VMX non-root）
 *   Thread 3: 普通用户进程（EL0 / U-mode / Ring3）
 *
 * ── 前置条件（调用 run_vmm_test 前必须已完成初始化）────────────────
 *
 *   [所有架构]
 *     pmm_initialize()          物理内存管理器
 *     task_init()               任务子系统 + idle 任务
 *     timer_init() + enable     定时器（调度 tick 依赖）
 *
 *   [AArch64]
 *     irq_init()                GICv2 中断控制器
 *     aarch64_enable_neon()     EL0 FP/SIMD（musl/busybox 需要）
 *     exception 由 boot.S 设置  向量表已在 boot 阶段装载
 *
 *   [RISC-V64]
 *     exception_init()          stvec 异常向量（必须在 fs_init 之前）
 *
 *   [x86_64]
 *     exception_init()          IDT
 *     x86_tss_init()            TSS（特权级切换）
 *
 * ── 调用方式（在 task_init 之后、timer_set_tick_cb 之前调用）────────
 *
 *   run_vmm_test();
 *
 * ── 注意事项 ──────────────────────────────────────────────────────
 *   - 本测试创建的任务在 task_yield 循环中永久运行，不会自动退出
 *   - RISC-V Thread 2（VS-mode）默认启用，如需调试可将 #if 1 改为 #if 0
 *   - x86_64 VMX 需要 QEMU 开启 `-cpu host,+vmx` 或 `-cpu qemu64,+vmx`
 */

#include "arch.h"
#include "klog.h"
#include "task/task.h"
#include "vmm.h"

#if ARCH_AARCH64
#include "aarch64/stage2.h"
#include "mm_vm.h"
extern void guest_test_entry(void);  /* apps/aarch64/guest_test.S */
extern void el0_loop_program(void);  /* apps/aarch64/el0_loop.S   */

static vm_t g_test_vm;

static void el2_loop_thread(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    while (1) {
        n++;
        if (n % 20 == 0)
            KLOG_INFO("[el2_loop] tick=%u (EL2 kernel)\n", n);
        task_yield();
    }
}
#endif /* ARCH_AARCH64 */

#if ARCH_RISCV64
extern void rv_guest_test_entry(void);                          /* apps/riscv64/guest_test.S */
extern void user_test_program(void);                            /* apps/riscv64/user_test.S  */
extern int  hext_vcpu_setup(vcpu_t *vcpu, void (*entry)(void));

static vm_t g_rv_vm;

static void rv_host_loop(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    while (1) {
        n++;
        if (n % 20 == 0)
            KLOG_INFO("[rv_host] tick=%u (HS-mode)\n", n);
        task_yield();
    }
}
#endif /* ARCH_RISCV64 */

#if ARCH_X86_64
extern void x86_guest_test_entry(void);                         /* apps/x86_64/guest_test.S */
extern void user_test_program(void);                            /* apps/x86_64/user_test.S  */
extern int  vmx_vcpu_setup(vcpu_t *vcpu, void (*entry)(void));

static vm_t g_x86_vm;

static void x86_host_loop(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    while (1) {
        n++;
        if (n % 20 == 0)
            KLOG_INFO("[x86_host] tick=%u (VMX root)\n", n);
        task_yield();
    }
}
#endif /* ARCH_X86_64 */

/* ── 测试入口 ──────────────────────────────────────────────────── */
void run_vmm_test(void)
{
#if ARCH_AARCH64
    /* ── AArch64 VHE VMM 三线程测试 ────────────────────────────────
     *   Thread 1: el2_loop   — EL2 host 内核监控循环
     *   Thread 2: vcpu0      — EL1 guest 无限循环（通过 stage2 隔离）
     *   Thread 3: el0_loop   — EL0 用户态无限 svc 循环
     * ─────────────────────────────────────────────────────────────── */
    KLOG_INFO("\n=== Avatar OS: AArch64 VHE VMM Test ===\n");

    /* Thread 1: EL2 kernel loop */
    task_t *el2_task = task_create("el2_loop", el2_loop_thread, NULL, 5);
    if (el2_task)
        KLOG_INFO("Thread 1 [EL2 kernel]: id=%u\n", el2_task->id);
    else
        KLOG_ERROR("Failed to create EL2 loop thread!\n");

    /* Thread 2: EL1 guest vCPU */
    KLOG_INFO("guest_test_entry phys=0x%llx\n",
              (uint64_t)virt_to_phys(guest_test_entry));

    g_test_vm.cfg.mem_base = GUEST_RAM_BASE;
    g_test_vm.cfg.mem_size = GUEST_RAM_SIZE;
    g_test_vm.cfg.nr_vcpus = 1;
    if (vm_create(&g_test_vm) == 0) {
        vcpu_t *vcpu = &g_test_vm.vcpus[0];
        vcpu->elr    = virt_to_phys(guest_test_entry);
        vcpu->spsr   = 0x5ULL | (0xFULL << 6);   /* EL1h, DAIF masked */
        vcpu->sp_el1 = GUEST_RAM_BASE + GUEST_RAM_SIZE - 0x1000;

        task_t *vt = vcpu_task_create(vcpu, 5);
        if (vt)
            KLOG_INFO("Thread 2 [EL1 guest/vcpu]: id=%u entry=0x%llx\n",
                      vt->id, vcpu->elr);
        else
            KLOG_ERROR("vcpu_task_create failed!\n");
    } else {
        KLOG_ERROR("vm_create (AArch64) failed!\n");
    }

    /* Thread 3: EL0 user process */
    task_t *el0_task = process_create("el0_loop",
                                      (uint64_t)el0_loop_program,
                                      0x4000,  /* 16KB，覆盖 el0_loop_program 代码 */
                                      0x200000,
                                      5);
    if (el0_task)
        KLOG_INFO("Thread 3 [EL0 user]:       id=%u entry=0x%llx\n",
                  el0_task->id, (uint64_t)el0_loop_program);
    else
        KLOG_ERROR("Failed to create EL0 user thread!\n");

#elif ARCH_RISCV64
    /* ── RISC-V H-ext VMM 三线程测试 ───────────────────────────────
     *   Thread 1: rv_host   — HS-mode 内核监控循环
     *   Thread 2: vcpu0     — VS-mode guest（WFI + ecall 循环）
     *   Thread 3: u_loop    — U-mode 用户进程
     * ─────────────────────────────────────────────────────────────── */
    KLOG_INFO("\n=== Avatar OS: RISC-V H-ext VMM Test ===\n");

    /* Thread 1: HS-mode kernel loop */
    task_t *rv_host_task = task_create("rv_host", rv_host_loop, NULL, 5);
    if (rv_host_task)
        KLOG_INFO("Thread 1 [HS-mode]:   id=%u\n", rv_host_task->id);
    else
        KLOG_ERROR("Failed to create rv_host_loop thread!\n");

    /* Thread 2: VS-mode guest vCPU（调试时可将 #if 1 改为 #if 0 禁用）*/
#if 1
    g_rv_vm.cfg.mem_base = 0;
    g_rv_vm.cfg.mem_size = 0;   /* 无 hgatp，guest 共享 host 地址空间 */
    g_rv_vm.cfg.nr_vcpus = 1;
    if (vm_create(&g_rv_vm) == 0) {
        vcpu_t *vcpu = &g_rv_vm.vcpus[0];
        if (hext_vcpu_setup(vcpu, rv_guest_test_entry) == 0) {
            task_t *vcpu_task = vcpu_task_create(vcpu, 5);
            if (vcpu_task)
                KLOG_INFO("Thread 2 [VS-mode]:   id=%u entry=%p\n",
                          vcpu_task->id, (void *)rv_guest_test_entry);
            else
                KLOG_ERROR("Failed to create vCPU task!\n");
        } else {
            KLOG_ERROR("hext_vcpu_setup failed!\n");
        }
    } else {
        KLOG_ERROR("vm_create (H-ext) failed!\n");
    }
#endif

    /* Thread 3: U-mode user process */
    task_t *u_task_rv = process_create("u_loop",
                                       (uint64_t)user_test_program,
                                       0x4000,  /* 16KB，覆盖 user_test_program 代码 */
                                       0x200000,
                                       5);
    if (u_task_rv)
        KLOG_INFO("Thread 3 [U-mode]:    id=%u entry=%p\n",
                  u_task_rv->id, (void *)user_test_program);
    else
        KLOG_ERROR("Failed to create user loop thread!\n");

#elif ARCH_X86_64
    /* ── x86_64 VMX VMM 三线程测试 ─────────────────────────────────
     *   Thread 1: x86_host  — VMX root 内核无限循环
     *   Thread 2: vcpu0     — VMX non-root guest（HLT + VMCALL）
     *   Thread 3: u_loop    — Ring3 用户进程
     * ─────────────────────────────────────────────────────────────── */
    KLOG_INFO("\n=== Avatar OS: x86_64 VMX VMM Test ===\n");

    /* Thread 1: VMX root kernel loop */
    task_t *host_task = task_create("x86_host", x86_host_loop, NULL, 5);
    if (host_task)
        KLOG_INFO("Thread 1 [VMX root]:  id=%u\n", host_task->id);
    else
        KLOG_ERROR("Failed to create x86_host_loop thread!\n");

    /* Thread 2: VMX non-root guest vCPU */
    g_x86_vm.cfg.mem_base = 0;
    g_x86_vm.cfg.mem_size = 0;   /* no EPT, guest shares host CR3 */
    g_x86_vm.cfg.nr_vcpus = 1;
    if (vm_create(&g_x86_vm) == 0) {
        vcpu_t *vcpu = &g_x86_vm.vcpus[0];
        if (vmx_vcpu_setup(vcpu, x86_guest_test_entry) == 0) {
            task_t *vcpu_task = vcpu_task_create(vcpu, 5);
            if (vcpu_task)
                KLOG_INFO("Thread 2 [VMX guest]: id=%u entry=%p\n",
                          vcpu_task->id, (void *)x86_guest_test_entry);
            else
                KLOG_ERROR("Failed to create vCPU task!\n");
        } else {
            KLOG_ERROR("vmx_vcpu_setup failed!\n");
        }
    } else {
        KLOG_ERROR("vm_create (VMX) failed!\n");
    }

    /* Thread 3: Ring3 user process */
    task_t *u_task = process_create("u_loop",
                                    (uint64_t)user_test_program,
                                    0x4000,  /* 16KB，覆盖 user_test_program 代码 */
                                    0x200000,
                                    5);
    if (u_task)
        KLOG_INFO("Thread 3 [user]:      id=%u entry=%p\n",
                  u_task->id, (void *)user_test_program);
    else
        KLOG_ERROR("Failed to create user loop thread!\n");

#else
    KLOG_INFO("[vmm_test] No VMM test for this architecture\n");
#endif
}
