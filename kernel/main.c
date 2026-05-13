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
#include "task/cpu.h"
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
extern int  el2_vcpu_setup(vcpu_t *vcpu, void (*entry)(void));

/* 每个物理核一个 vCPU，SMP=N 时最多 N 个 */
static vm_t g_test_vm;

/* smp_loop_thread / el0_loop 已删除，改为 per-CPU VCPU 任务 */
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
    
    /* ── 初始化 CPU0 的 per-CPU 数据 ──────────────────────── */
#if ARCH_AARCH64 || ARCH_RISCV64 || ARCH_X86_64
    KLOG_INFO("Initializing CPU0 per-CPU data...\n");
    
    /* 初始化 g_cpus[0] 的基本字段 */
    extern uint32_t g_num_cpus;
    extern cpu_t g_cpus[8];
    
    memset(&g_cpus[0], 0, sizeof(cpu_t));
    g_cpus[0].cpu_id = 0;
    g_cpus[0].mpidr = 0;
    list_init(&g_cpus[0].run_queue);
    g_cpus[0].current_task = NULL;
    g_cpus[0].idle_task = NULL;
    g_cpus[0].need_resched = false;
    
    KLOG_DEBUG("[main] Initialized g_cpus[0] structure\n");
    
    /* 设置 CPU0 的 TPIDR_EL1 / TP / FS_BASE，指向 &g_cpus[0]
     * 这样 get_current_cpu_id() 和 cpu_current() 才能工作 */
#if ARCH_AARCH64
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&g_cpus[0]));
    __asm__ volatile("isb");
#elif ARCH_RISCV64
    __asm__ volatile("mv tp, %0" :: "r"(&g_cpus[0]));
#elif ARCH_X86_64
    {
        uint32_t lo = (uint32_t)((uint64_t)&g_cpus[0] & 0xFFFFFFFFU);
        uint32_t hi = (uint32_t)(((uint64_t)&g_cpus[0] >> 32) & 0xFFFFFFFFU);
        __asm__ volatile("wrmsr" :: "c"(0xC0000100U), "a"(lo), "d"(hi));
    }
#endif
    
    KLOG_DEBUG("[main] Set CPU0 thread pointer register\n");
    
    /* 启动其他 CPU 核心 */
    KLOG_INFO("\n");
    KLOG_INFO("Starting secondary CPUs...\n");
    cpu_bring_up_all();
    
    /* 给其他 CPU 一些时间来启动并完成初始化 */
    for (volatile int i = 0; i < 10000000; i++);
    
    KLOG_INFO("CPU initialization complete (num_cpus=%u)\n", g_num_cpus);
#endif

    task_init();

    /* 切换到 idle 专用栈（防止 boot 栈在频繁中断下溢出） */
    task_switch_to_idle_stack();


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

    /*
     * SMP VCPU 测试：每个物理 CPU 核创建一个 vCPU 任务。
     * vCPU 运行 guest_test_entry（WFI + HVC 循环），
     * 由时钟抢占驱动切换（vcpu 任务主动 yield 让出 CPU）。
     */
    KLOG_INFO("\n=== Avatar OS: SMP VCPU test (%u cores) ===\n", g_num_cpus);

    /*
     * mem_base/mem_size：Stage-2 identity map 的物理 RAM 范围。
     * 必须覆盖 guest 代码（guest_test_entry PA）和栈（g_guest_stacks PA）。
     * GUEST_RAM_BASE=0x40000000, GUEST_RAM_SIZE=128MB（定义于 aarch64/stage2.h）。
     */
    g_test_vm.cfg.mem_base  = GUEST_RAM_BASE;
    g_test_vm.cfg.mem_size  = GUEST_RAM_SIZE;
    g_test_vm.cfg.nr_vcpus  = (int)g_num_cpus;
    if (vm_create(&g_test_vm) != 0) {
        KLOG_ERROR("[main] vm_create failed\n");
    } else {
        for (uint32_t c = 0; c < g_num_cpus; c++) {
            vcpu_t *vcpu = &g_test_vm.vcpus[c];

            if (el2_vcpu_setup(vcpu, guest_test_entry) != 0) {
                KLOG_ERROR("[main] el2_vcpu_setup failed for vcpu%u\n", c);
                continue;
            }

            struct task *t = vcpu_task_create(vcpu, 5);
            if (!t) {
                KLOG_ERROR("[main] vcpu_task_create failed for vcpu%u\n", c);
                continue;
            }

            /* vcpu_task_create 默认入队 CPU0；迁移到对应物理核 */
            if (c != 0) {
                sched_dequeue(t);
                t->cpu_affinity = c;
                sched_enqueue_on_cpu(t, c);
            }
            KLOG_INFO("  vcpu%u task id=%u -> cpu%u\n", c, t->id, c);
        }
    }

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

    extern volatile uint32_t g_syscall_entry_count;
    uint64_t idle_count = 0;
    uint32_t last_syscall_count = 0;
    
    while (1) {
        idle_count++;
        if (idle_count % 500 == 0) {
            if (g_syscall_entry_count != last_syscall_count) {
                KLOG_INFO("[idle] syscalls=%u\n", g_syscall_entry_count);
                last_syscall_count = g_syscall_entry_count;
            }
        }
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

