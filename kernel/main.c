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
#elif ARCH_RISCV64
#include "exception.h"
#elif ARCH_X86_64
#include "exception.h"
#endif

/* Forward declarations for test functions */
extern void test_arch(void);
extern void run_klog_tests(void);
extern void test_string_functions(void);
extern void run_assert_tests(void);
extern void run_mutex_tests(void);
extern void run_mutex_demo(void);
extern void run_mutex_stress_test(void);
extern void run_mutex_comparison_test(void);

#if ARCH_AARCH64
extern void kmem_test(void);
#endif

/* 用户测试程序入口（AArch64 / RISC-V / x86_64 嵌入内核） */
#if ARCH_AARCH64 || ARCH_RISCV64 || ARCH_X86_64
extern void user_test_program(void);
extern void hello_program(void);
#endif

#if ARCH_RISCV64
/*
 * demo_user_test_rv - 用嵌入内核的汇编程序直接创建 RISC-V 用户进程
 * 目的：绕过 ELF loader / 文件系统，单独验证 ecall 通路是否正常工作。
 */
static void demo_user_test_rv(void *arg)
{
    (void)arg;
    KLOG_INFO("[rv_test] Creating embedded RISC-V user test process...\n");

    /*
     * process_create 会：
     *   1. 调用 vm_create_user_process 把代码复制到用户 VA 0x10000
     *   2. 把内核 L2[0x100]/[0x102] 嫁接到新 PGD，保证 trap 仍可到达
     *      stvec (0xffffffc0...)
     *   3. 用户栈 [0x100000, 0x200000)  SP = 0x200000
     */
    task_t *p = process_create("rv_user_test",
                               (uint64_t)user_test_program,
                               0x200000,   /* user_sp (stack top) */
                               5);
    if (p) {
        KLOG_INFO("[rv_test] User test process created: id=%u entry=0x10000\n",
                  p->id);
    } else {
        KLOG_ERROR("[rv_test] Failed to create RISC-V user test process!\n");
    }
    task_exit();
}
#endif

/* ── 演示任务 ─────────────────────────────────────────────── */

static void __attribute__((unused)) demo_task_a(void *arg)
{
    (void)arg;
    uint32_t count = 0;
    while (1) {
        count++;
        if (count % 50 == 0) {
            KLOG_INFO("[task_a] count=%u ticks=%llu\n",
                      count, timer_get_system_ticks());
        }
        for (volatile int i = 0; i < 1000000; i++);  // 模拟工作负载
        // task_yield();
    }
}

static void __attribute__((unused)) demo_task_b(void *arg)
{
    (void)arg;
    uint32_t count = 0;
    while (1) {
        count++;
        if (count % 50 == 0) {
            KLOG_INFO("[task_b] count=%u ticks=%llu\n",
                      count, timer_get_system_ticks());
        }
        for (volatile int i = 0; i < 1000000; i++);  // 模拟工作负载
        // task_yield();
    }
}

static void __attribute__((unused)) demo_task_c(void *arg)
{
    (void)arg;
    uint32_t count = 0;
    while (1) {
        count++;
        if (count % 50 == 0) {
            KLOG_INFO("[task_c] count=%u ticks=%llu\n",
                      count, timer_get_system_ticks());
        }
        for (volatile int i = 0; i < 1000000; i++);  // 模拟工作负载
        // task_yield();
    }
}

/*
 * short_lived_task - 短生命周期任务，用于测试栈溢出
 * 目的：不断创建和退出任务，触发栈槽复用，暴露 idle 栈问题
 */
static void short_lived_task(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    KLOG_INFO("[short_task_%u] running and exiting immediately\n", id);
    
    /* 在栈上分配较多数据，增加栈使用，加速栈溢出 */
    volatile char stack_filler[1024];
    for (int i = 0; i < 1024; i++) {
        stack_filler[i] = (char)(id + i);
    }
    
    /* 模拟一些工作负载 */
    for (volatile int i = 0; i < 10000; i++);
    
    /* 短暂运行后退出，触发栈槽释放和复用 */
    task_exit();
}

/*
 * stack_overflow_test - 栈溢出测试任务
 * 创建一批短生命周期任务，触发 idle 栈问题
 */
static void stack_overflow_test(void *arg)
{
    (void)arg;
    KLOG_INFO("[stack_test] Creating short-lived tasks to trigger stack reuse...\n");
    
    /* 创建 30 个短生命周期任务，快速消耗和复用栈槽 */
    for (uint32_t i = 0; i < 30; i++) {
        task_t *t = task_create("short_task", short_lived_task, 
                                (void*)(uintptr_t)i, 5);
        if (t) {
            KLOG_INFO("[stack_test] Created task %u (id=%u)\n", i, t->id);
        } else {
            KLOG_ERROR("[stack_test] Failed to create task %u\n", i);
        }
        
        /* 让出 CPU，给短任务运行和退出的机会 */
        for (int j = 0; j < 10; j++) {
            task_yield();
        }
    }
    
    KLOG_INFO("[stack_test] All short tasks created. System should enter idle soon.\n");
    KLOG_INFO("[stack_test] Waiting for stack overflow to occur (if bug exists)...\n");
    KLOG_INFO("[stack_test] Bug should trigger within a few seconds of idle...\n");
    
    /* 等待一段时间，让系统进入 idle */
    for (int i = 0; i < 100; i++) {
        task_yield();
    }
    
    /* 持续创建新任务，覆盖已释放的栈内存 */
    KLOG_INFO("[stack_test] Phase 2: Creating more tasks to corrupt idle stack...\n");
    for (uint32_t i = 30; i < 50; i++) {
        task_t *t = task_create("corrupt_task", short_lived_task, 
                                (void*)(uintptr_t)i, 5);
        if (t) {
            KLOG_INFO("[stack_test] Corruption task %u (id=%u) created\n", i, t->id);
        }
        
        /* 频繁让出，增加调度压力 */
        for (int j = 0; j < 20; j++) {
            task_yield();
        }
    }
    
    KLOG_INFO("[stack_test] Test complete. If no crash, bug may be latent.\n");
    
    /* 本任务也退出，让系统完全进入 idle */
    task_exit();
}

/*
 * demo_load_loop_a - 加载第一个循环测试程序
 */
static void demo_load_loop_a(void *arg)
{
    (void)arg;

    /* 等待文件系统初始化 */
    KLOG_INFO("[loop_a_loader] Waiting for filesystem...\n");
    for (int i = 0; i < 100; i++) {
        task_yield();
    }

    KLOG_INFO("[loop_a_loader] Loading /loop_a from filesystem...\n");

    /* 调用 ELF 加载器执行 /loop_a */
    const char *path = "/loop_a";
    int rc = elf_loader_load_from_file(path, NULL, NULL);

    if (rc != 0) {
        KLOG_ERROR("[loop_a_loader] Failed to load /loop_a: %d\n", rc);
    }

    KLOG_INFO("[loop_a_loader] Exiting...\n");
    task_exit();
}

/*
 * demo_load_loop_b - 加载第二个循环测试程序
 */
static void demo_load_loop_b(void *arg)
{
    (void)arg;

    /* 等待文件系统初始化 */
    KLOG_INFO("[loop_b_loader] Waiting for filesystem...\n");
    for (int i = 0; i < 100; i++) {
        task_yield();
    }

    KLOG_INFO("[loop_b_loader] Loading /loop_b from filesystem...\n");

    /* 调用 ELF 加载器执行 /loop_b */
    const char *path = "/loop_b";
    int rc = elf_loader_load_from_file(path, NULL, NULL);

    if (rc != 0) {
        KLOG_ERROR("[loop_b_loader] Failed to load /loop_b: %d\n", rc);
    }

    KLOG_INFO("[loop_b_loader] Exiting...\n");
    task_exit();
}

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

    /* 调用 ELF 加载器执行 /busybox */
    const char *path = "/busybox";
    int rc = elf_loader_load_from_file(path, NULL, NULL);

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
    fs_init();

    /* ── 运行 PMM 测试 ───────────────────────────────────────── */
    KLOG_INFO("\n");
    //run_pmm_tests();

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

#if 0
    /* Run all tests */
    KLOG_INFO("Running tests...\n");
    run_all_tests();

    /* All tests completed */
    KLOG_INFO("All tests completed successfully!\n");
#endif

    /* ── 初始化任务子系统 ───────────────────────────────── */
    KLOG_INFO("Initializing task subsystem...\n");
    task_init();

    /* 切换到 idle 专用栈（防止 boot 栈在频繁中断下溢出） */
    task_switch_to_idle_stack();

#if 0
    /* Mutex tests */
    KLOG_INFO("--- Mutex Tests ---\n");
    run_mutex_comparison_test();
    KLOG_INFO("");
#endif

#if !defined(ARCH_AARCH64) && !defined(ARCH_RISCV64) && !defined(ARCH_X86_64)
    /* 创建演示任务（仅用于没有用户进程支持的架构） */
    task_create("task_a", demo_task_a, NULL, 1);
    task_create("task_b", demo_task_b, NULL, 1);
    task_create("task_c", demo_task_c, NULL, 1);
#endif

    /* === 测试用户进程创建 === */
    KLOG_INFO("\n");
    KLOG_INFO("=== Testing User Process Creation ===\n");

#if ARCH_RISCV64

    /* RISC-V：现在基础设施已验证正常，测试 busybox */
    KLOG_INFO("Creating busybox loader...\n");
    
    task_t *proc1 = task_create("busybox_loader", demo_load_busybox, NULL, 5);
    if (proc1) {
        KLOG_INFO("Busybox loader created successfully!\n");
    } else {
        KLOG_ERROR("Failed to create busybox loader!\n");
    }

#elif ARCH_AARCH64

    /* AArch64：继续使用 busybox */
    task_t *proc1 = task_create("busybox_loader", demo_load_busybox, NULL, 5);
    if (proc1) {
        KLOG_INFO("Task 1 (busybox_loader) created successfully!\n");
    } else {
        KLOG_ERROR("Failed to create task 1!\n");
    }
    
#elif ARCH_X86_64

    /* x86_64：创建两个 Ring 3 测试程序 */
    KLOG_INFO("Creating x86_64 Ring 3 test processes...\n");
    
    task_t *proc1 = process_create("x86_user_test", 
                                   (uint64_t)user_test_program,
                                   0x1ffff0,   /* 用户栈顶（16字节对齐，在映射范围内）*/
                                   5);
    if (proc1) {
        KLOG_INFO("User test process created: id=%u\n", proc1->id);
    } else {
        KLOG_ERROR("Failed to create user test process!\n");
    }
    
    task_t *proc2 = process_create("x86_hello",
                                   (uint64_t)hello_program,
                                   0x1ffff0,   /* 用户栈顶（16字节对齐，在映射范围内）*/
                                   5);
    if (proc2) {
        KLOG_INFO("Hello process created: id=%u\n", proc2->id);
    } else {
        KLOG_ERROR("Failed to create hello process!\n");
    }

#endif /* ARCH_RISCV64 / ARCH_AARCH64 / ARCH_X86_64 */

    /*
     * 在内核初始化/创建用户进程完成后再启用抢占。
     * 避免 main 仍在内核路径时被 tick 打断，导致后续创建流程（如第二个进程）饿死。
     */
    timer_set_tick_cb(sched_tick);
    KLOG_INFO("Preemptive scheduling enabled\n");

    KLOG_INFO("\nProcesses will run in Ring 3 (user mode)\n");

    KLOG_INFO("\n");
    KLOG_INFO("Demo tasks created. Entering idle loop...\n");

    extern volatile uint32_t g_syscall_entry_count;
    // extern volatile uint64_t g_exception_entry_count;
    
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

void run_all_tests(void)
{
    /* Run architecture test */
    KLOG_INFO("--- Architecture Detection Test ---\n");
    test_arch();
    KLOG_INFO("");

    /* Run klog test */
    KLOG_INFO("--- Kernel Log Test ---\n");
    run_klog_tests();
    KLOG_INFO("");

    /* Run string test */
    KLOG_INFO("--- String Test ---\n");
    test_string_functions();
    KLOG_INFO("");

    /* Run assert test */
    KLOG_INFO("--- Assert Test ---\n");
    run_assert_tests();
    KLOG_INFO("");
}