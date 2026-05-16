/*
 * Kernel main entry point
 * Called from architecture-specific boot code
 */

#include "../boot/common/boot.h"
#include "../boot/common/platform.h"
#include "arch.h"
#include "klog.h"
#include "string.h"
#include "task/task.h"
#include "task/sched.h"
#include "pmm.h"
#include "../driver/blk/ramblk.h"
#include "../fs/lwext4_port/fs_init.h"
#include "loader/elf_loader.h"
#include "timer/timer.h"

#if ARCH_AARCH64
#include "irq/irq.h"
#include "aarch64/cpu.h"
#include "vmm.h"
#include "aarch64/stage2.h"
#include "mm_vm.h"
#elif ARCH_RISCV64
#include "exception.h"
#include "vmm.h"
#elif ARCH_X86_64
#include "exception.h"
#include "vmm.h"
#endif


/* 用户测试程序入口（AArch64 / RISC-V / x86_64 嵌入内核） */
#if ARCH_AARCH64 || ARCH_RISCV64 || ARCH_X86_64
extern void user_test_program(void);
extern void hello_program(void);
extern void test_execve_program(void);
#endif

#if ARCH_AARCH64
extern void guest_test_entry(void);   /* apps/aarch64/guest_test.S */
extern void el0_loop_program(void);   /* apps/aarch64/el0_loop.S */

/* Phase 2: VMM 测试用静态 VM 实例 */
static vm_t g_test_vm;

/* EL2 内核无限循环线程（运行在 EL2 host 态）*/
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
#endif

#if ARCH_X86_64
extern void x86_guest_test_entry(void); /* apps/x86_64/guest_test.S */
extern void user_test_program(void);    /* apps/x86_64/user_test.S  */
extern int  vmx_vcpu_setup(vcpu_t *vcpu, void (*entry)(void));

/* x86 VMM 测试用静态 VM 实例 */
static vm_t g_x86_vm;

/* x86 host 内核无限循环线程（VMX root mode）*/
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
#endif

#if ARCH_RISCV64
/* RISC-V H-extension VMM 静态 VM 实例 */
static vm_t g_rv_vm;

extern void rv_guest_test_entry(void);  /* apps/riscv64/guest_test.S */
extern int  hext_vcpu_setup(vcpu_t *vcpu, void (*entry)(void));

/* Thread 1: HS-mode 内核监控线程（等价 AArch64 el2_loop）*/
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
#endif


/*
 * demo_load_busybox - 从文件系统加载并执行 busybox
 */
static void demo_load_busybox(void *arg)
{
    (void)arg;

    /* 等待文件系统初始化 */
    KLOG_INFO("[busybox_loader] Waiting for filesystem...\n");
    for (int i = 0; i < 100; i++) {
        task_yield();
    }

    KLOG_INFO("[busybox_loader] Loading /busybox from filesystem...\n");

#if ARCH_RISCV64
    uint64_t satp_val;
    __asm__ volatile("csrr %0, satp" : "=r"(satp_val));
    KLOG_INFO("[busybox_loader] satp=0x%llx\n", satp_val);
#endif

    /* 以 busybox 的 sh applet 进入交互 shell */
    char *bb_argv[] = { "sh", "-i", NULL };

    /* 调用 ELF 加载器执行 /busybox */
    const char *path = "/busybox";
    int rc = elf_loader_load_from_file(path, bb_argv, NULL);

    if (rc != 0) {
        KLOG_ERROR("[busybox_loader] Failed to load /busybox: %d\n", rc);
        KLOG_INFO("[busybox_loader] Run: ./install-apps.sh aarch64\n");
    }

    /* 任务完成 */
    KLOG_INFO("[busybox_loader] Exiting...\n");
    task_exit();
}


void kernel_main(void)
{
#if ARCH_AARCH64
    /*
     * MMU 已开启（boot.S 中完成），现在运行在高虚拟地址。
     * 将 UART 基地址切换到 TTBR1 覆盖的高虚拟地址，
     * 使内核在任意 TTBR0（用户页表）下仍可正常输出。
     */
    extern volatile uintptr_t g_pl011_base;
    g_pl011_base = 0x09000000UL + 0xffff000000000000ULL;
#endif

    /* Initialize platform (UART, etc.) */
    platform_init();

    /* Print welcome message */
    KLOG_INFO("=== Avatar OS Kernel ===\n");
    KLOG_INFO("Architecture: "ARCH_NAME "\n");
    KLOG_INFO("Build time: " __DATE__ " " __TIME__ "\n");

    /* ── 初始化物理内存管理器 ───────────────────────────────── */
    KLOG_INFO("\n");
    pmm_initialize();

    /* ── 初始化文件系统 ─────────────────────────────────────────── */
    KLOG_INFO("\n");
#if ARCH_RISCV64
    /* 必须在 fs_init() 之前设置 stvec，否则 ext4_mount 中的任何
     * CPU 异常都会落到 M-mode (OpenSBI)，导致 hart 被重置 */
    KLOG_INFO("Initializing exception handler...\n");
    exception_init();
#endif
    ramblk_init();
    KLOG_INFO("\n");
    KLOG_INFO("Initializing filesystem...\n");
    fs_init();

    /* ── 运行 PMM 测试 ───────────────────────────────────────── */
    /* 测试时解开下面两行注释 */
    KLOG_INFO("\n");
#ifdef RUN_PMM_TESTS
    run_pmm_tests();
    do_platform_shutdown();  /* PMM 测试完成后关机，避免后续测试干扰 PMM 状态 */
#endif

#if ARCH_AARCH64
    // /* ── 运行 VMM 测试 ───────────────────────────────────────── */
    // KLOG_INFO("");
    KLOG_INFO("=== Running VMM Tests ===\n");
    kmem_test();
    KLOG_INFO("VMM tests completed\n");

    /* Initialize GIC (interrupt controller) */
    KLOG_INFO("Initializing GICv2 interrupt controller...\n");
    irq_init();
    KLOG_INFO("GICv2 initialized\n");

    /* Enable FP/SIMD for EL0 (busybox/musl use NEON instructions) */
    aarch64_enable_neon();
    KLOG_INFO("FP/SIMD enabled for EL0 (CPACR_EL1.FPEN=0b11)\n");

    /* Initialize timer */
    KLOG_INFO("Initializing timer...\n");
    timer_init();
    timer_enable();
    KLOG_INFO("Timer enabled\n");

#elif ARCH_RISCV64

    /* Initialize timer (also registers timer_handler via irq_install) */
    KLOG_INFO("Initializing timer...\n");
    timer_init();
    timer_enable();
    KLOG_INFO("Timer enabled\n");

#elif ARCH_X86_64

    /* Initialize IDT and LAPIC */
    KLOG_INFO("Initializing IDT + LAPIC...\n");
    exception_init();
    
    /* Initialize TSS (Task State Segment for privilege switching) */
    extern void x86_tss_init(void);
    x86_tss_init();

    /* Initialize timer (registers handler, calibrates LAPIC frequency) */
    KLOG_INFO("Initializing timer...\n");
    timer_init();
    timer_enable();
    KLOG_INFO("Timer enabled\n");

#endif


    /* ── 初始化任务子系统 ───────────────────────────────── */
    KLOG_INFO("Initializing task subsystem...\n");
    task_init();

    /* === 测试用户进程创建 === */
    KLOG_INFO("\n");
    KLOG_INFO("=== Testing User Process Creation ===\n");

#if ARCH_RISCV64

    /* ── RISC-V H-ext VMM 3 线程测试 ─────────────────────────────────
     * Thread 1: rv_host_loop  — HS-mode 内核监控循环
     * Thread 2: vcpu0          — VS-mode guest（WFI + ecall 循环）
     * Thread 3: u_loop         — 普通用户进程（U-mode）
     * ─────────────────────────────────────────────────────────────── */
    KLOG_INFO("\n=== Avatar OS: RISC-V H-ext VMM Test ===\n");

    /* Thread 1: HS-mode kernel loop */
    task_t *rv_host_task = task_create("rv_host", rv_host_loop, NULL, 5);
    if (rv_host_task)
        KLOG_INFO("Thread 1 [HS-mode]:   id=%u\n", rv_host_task->id);
    else
        KLOG_ERROR("Failed to create rv_host_loop thread!\n");

    /* Thread 2: VS-mode guest vCPU — TEMPORARILY DISABLED for debug */
#if 1
    g_rv_vm.cfg.mem_base  = 0;
    g_rv_vm.cfg.mem_size  = 0;   /* 无 hgatp，guest 共享 host 地址空间 */
    g_rv_vm.cfg.nr_vcpus  = 1;
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

    /* Thread 3: 普通用户进程（U-mode） */
    task_t *u_task_rv = process_create("u_loop",
                                       (uint64_t)user_test_program,
                                       0x200000,
                                       5);
    if (u_task_rv)
        KLOG_INFO("Thread 3 [U-mode]:    id=%u entry=%p\n",
                  u_task_rv->id, (void *)user_test_program);
    else
        KLOG_ERROR("Failed to create user loop thread!\n");

#elif ARCH_AARCH64

    /* ── Thread 1: EL2 内核线程（无限循环） ── */
    task_t *el2_task = task_create("el2_loop", el2_loop_thread, NULL, 5);
    if (el2_task)
        KLOG_INFO("Thread 1 [EL2 kernel]: id=%u\n", el2_task->id);
    else
        KLOG_ERROR("Failed to create EL2 loop thread!\n");

    /* ── Thread 2: vCPU guest 线程（EL1 guest 无限循环） ── */
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
        KLOG_ERROR("vm_create failed!\n");
    }

    /* ── Thread 3: EL0 用户线程（无限 svc yield 循环） ── */
    task_t *el0_task = process_create("el0_loop",
                                      (uint64_t)el0_loop_program,
                                      0x200000,
                                      5);
    if (el0_task)
        KLOG_INFO("Thread 3 [EL0 user]:   id=%u entry=0x%llx\n",
                  el0_task->id, (uint64_t)el0_loop_program);
    else
        KLOG_ERROR("Failed to create EL0 user thread!\n");

#elif ARCH_X86_64

    /* ── x86_64 VMX VMM 3 线程测试 ──────────────────────────────────
     * Thread 1: x86_host_loop — VMX root 内核无限循环
     * Thread 2: vcpu0          — VMX non-root guest (HLT + VMCALL)
     * Thread 3: u_loop         — 普通用户进程
     * ─────────────────────────────────────────────────────────────── */
    KLOG_INFO("\n=== Avatar OS: x86_64 VMX VMM Test ===\n");

    /* Thread 1: VMX root kernel loop */
    task_t *host_task = task_create("x86_host", x86_host_loop, NULL, 5);
    if (host_task)
        KLOG_INFO("Thread 1 [VMX root]:  id=%u\n", host_task->id);
    else
        KLOG_ERROR("Failed to create x86_host_loop thread!\n");

    /* Thread 2: VMX non-root guest vCPU */
    g_x86_vm.cfg.mem_base  = 0;
    g_x86_vm.cfg.mem_size  = 0;  /* no EPT, guest shares host CR3 */
    g_x86_vm.cfg.nr_vcpus  = 1;
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

    /* Thread 3: 普通用户进程（等同 AArch64 el0_loop）*/
    task_t *u_task = process_create("u_loop",
                                    (uint64_t)user_test_program,
                                    0x200000,
                                    5);
    if (u_task)
        KLOG_INFO("Thread 3 [user]:      id=%u entry=%p\n",
                  u_task->id, (void *)user_test_program);
    else
        KLOG_ERROR("Failed to create user loop thread!\n");

#endif /* ARCH_RISCV64 / ARCH_AARCH64 / ARCH_X86_64 */

    /*
     * 在内核初始化/创建用户进程完成后再启用抢占。
     * 避免 main 仍在内核路径时被 tick 打断，导致后续创建流程（如第二个进程）饿死。
     */
    timer_set_tick_cb(sched_tick);
    KLOG_INFO("Preemptive scheduling enabled\n");
    KLOG_INFO("\n");

    /* 切换到 idle 专用栈（防止 boot 栈在频繁中断下溢出） */
    task_switch_to_idle_stack();
    
    while (1) {

        task_yield();
        #if ARCH_AARCH64
                __asm__ volatile("wfe");
        #elif ARCH_X86_64
                __asm__ volatile("hlt");
        #else
                __asm__ volatile("wfi");
        #endif
    }

    /* Shutdown */
    KLOG_INFO("Kernel shutting down...\n");
    do_platform_shutdown();
}

