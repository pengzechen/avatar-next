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
#include "syscall/bin_loader.h"

#if ARCH_AARCH64
#include "irq/irq.h"
#include "timer/timer.h"
#include "mm/aarch64/vmm.h"
#include "aarch64/cpu.h"
#elif ARCH_RISCV64
#include "exception.h"
#include "timer/timer.h"
#elif ARCH_X86_64
#include "exception.h"
#include "timer/timer.h"
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

/* 用户测试程序入口（仅 AArch64 嵌入内核） */
#if ARCH_AARCH64
extern void user_test_program(void);
extern void hello_program(void);
#endif

/* ── 演示任务 ─────────────────────────────────────────────── */

static void __attribute__((unused)) demo_task_a(void *arg)
{
    (void)arg;
    uint32_t count = 0;
    while (1) {
        count++;
        if (count % 50 == 0) {
            KLOG_INFO("[task_a] count=%u ticks=%llu",
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
            KLOG_INFO("[task_b] count=%u ticks=%llu",
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
            KLOG_INFO("[task_c] count=%u ticks=%llu",
                      count, timer_get_system_ticks());
        }
        for (volatile int i = 0; i < 1000000; i++);  // 模拟工作负载
        // task_yield();
    }
}

#if 1
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

/*
 * demo_load_from_fs - 从文件系统加载并执行程序
 *
 * 这个内核任务会：
 * 1. 等待文件系统初始化
 * 2. 从文件系统读取 /test_exec
 * 3. 创建新的用户进程执行它
 */
static void demo_load_from_fs(void *arg)
{
    (void)arg;

    /* 等待文件系统初始化 */
    KLOG_INFO("[fs_loader] Waiting for filesystem...\n");
    for (int i = 0; i < 100; i++) {
        task_yield();
    }

    KLOG_INFO("[fs_loader] Loading /test_exec from filesystem...\n");

    /* 调用 bin_loader_load_from_file
     * 注意：这是内核调用，使用内核字符串
     */
    const char *path = "/test_exec";
    int rc = bin_loader_load_from_file(path, NULL, NULL);

    if (rc != 0) {
        KLOG_ERROR("[fs_loader] Failed to load /test_exec: %d\n", rc);
        KLOG_INFO("[fs_loader] This is expected if test_exec is not installed.\n");
        KLOG_INFO("[fs_loader] Run: ./install-apps.sh aarch64\n");
    }

    /* 任务完成 */
    KLOG_INFO("[fs_loader] Exiting...\n");
    task_exit();
}
#endif

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
    KLOG_INFO("Architecture: "
#if ARCH_AARCH64
        "AArch64 (ARM 64-bit)"
#elif ARCH_X86_64
        "x86_64 (AMD64/Intel 64)"
#elif ARCH_RISCV64
        "RISC-V 64-bit"
#else
        "Unknown"
#endif
    );

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

    /* 将 sched_tick 注册为 timer tick 回调，启用抢占 */
    timer_set_tick_cb(sched_tick);
    KLOG_INFO("Preemptive scheduling enabled\n");


#if 0
    /* Mutex tests */
    KLOG_INFO("--- Mutex Tests ---\n");
    run_mutex_demo();
#endif

#if 0
    /* 创建演示任务（如果需要） */
    task_create("task_a", demo_task_a, NULL, 1);
    task_create("task_b", demo_task_b, NULL, 1);
    task_create("task_c", demo_task_c, NULL, 1);
#endif

    /* === 测试用户进程创建 === */
    KLOG_INFO("\n");
    KLOG_INFO("=== Testing User Process Creation ===\n");

#if ARCH_AARCH64
    #if 1
        /* 创建用户进程：busybox（从文件系统加载 ELF）
        * busybox 将作为 init 进程直接启动
        * 这个任务会调用 execve 从文件系统加载 busybox
        */
        task_t *proc1 = task_create("busybox_loader", demo_load_busybox, NULL, 5);
        if (proc1) {
            KLOG_INFO("Task 1 (busybox_loader) created successfully!\n");
        } else {
            KLOG_ERROR("Failed to create task 1!\n");
        }
        /* 如果没有文件系统，使用普通的 user_test */
        // task_t *proc1 = process_create("user_test",
        //                                 (uint64_t)user_test_program,
        //                                 0x70000000ULL,
        //                                 10);
        // if (proc1) {
        //     KLOG_INFO("Process 1 (user_test) created successfully!\n");
        // } else {
        //     KLOG_ERROR("Failed to create process 1!\n");
        // }
    #endif /* AVATAR_HAS_FILESYSTEM */

        // /* 创建第二个用户进程：hello（测试用） */
        // task_t *proc2 = process_create("hello",
        //                                 (uint64_t)hello_program,
        //                                 0x70100000ULL,
        //                                 10);
        // if (proc2) {
        //     KLOG_INFO("Process 2 (hello) created successfully! id=%u\n", proc2->id);
        // } else {
        //     KLOG_ERROR("Failed to create process 2!\n");
        // }
#endif /* ARCH_AARCH64 */

    KLOG_INFO("\nProcesses will run in EL0 (user mode)\n");

/* === 测试从文件系统加载程序 === */
#if 0
    KLOG_INFO("\n");
    KLOG_INFO("=== Testing Filesystem Program Loading ===\n");

    /* 创建一个内核任务来加载并执行 test_exec */
    task_t *fs_loader = task_create("fs_loader", demo_load_from_fs, NULL, 5);
    if (fs_loader) {
        KLOG_INFO("Created filesystem loader task (id=%u)\n", fs_loader->id);
    } else {
        KLOG_ERROR("Failed to create fs_loader task!\n");
    }
#endif

    KLOG_INFO("\n");
    KLOG_INFO("Demo tasks created. Entering idle loop...\n");

#if ARCH_AARCH64 || ARCH_RISCV64 || ARCH_X86_64
    /* idle 循环：持续 yield，让其他任务运行 */
    uint64_t idle_count = 0;
    while (1) {
        idle_count++;
        if (idle_count % 100 == 0) {
            KLOG_DEBUG("[idle] yielding... count=%llu\n", idle_count);
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
#endif /* ARCH_AARCH64 || ARCH_RISCV64 || ARCH_X86_64 */

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

/* ── Mutex 演示 ───────────────────────────────────────────── */

void
run_mutex_demo(void)
{
    KLOG_INFO("--- Mutex Comparison Test (No Lock vs With Lock) ---\n");
    run_mutex_comparison_test();
    KLOG_INFO("");

#if 0
    /* Run exception test (undefined instruction) */
    KLOG_INFO("--- Exception Test (Undefined Instruction) ---");
    KLOG_INFO("Attempting to execute an undefined instruction...");

    /* Trigger an undefined instruction exception */
    /* 0x00000000 is not a valid AArch64 instruction */
    __asm__ volatile(
        ".inst 0x00000000\n"  /* Undefined instruction */
    );

    KLOG_INFO("If you see this, exception handling failed!");
#endif
}
