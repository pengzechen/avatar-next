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
#include "lua_driver.h"
#include "task/switch.h"     /* arch_irq_enable */


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

extern void run_vmm_test(void);  /* tests/vmm_test.c */


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
    char *bb_envp[] = {
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
        "HOME=/root",
        "TERM=vt100",
        NULL
    };

    /* 调用 ELF 加载器执行 /busybox */
    const char *path = "/busybox";
    int rc = elf_loader_load_from_file(path, bb_argv, bb_envp);

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
    /* Must enable FP/NEON before any FP code runs (including Lua VM) */
    aarch64_enable_neon();
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
    KLOG_INFO("=== Running VMM Tests ===\n");
    kmem_test();
    KLOG_INFO("VMM tests completed\n");
#elif ARCH_X86_64
    /* Initialize IDT and LAPIC */
    KLOG_INFO("Initializing IDT + LAPIC...\n");
    exception_init();
    /* Initialize TSS (Task State Segment for privilege switching) */
    extern void x86_tss_init(void);
    x86_tss_init();
#endif

    KLOG_WARN("=== Running Lua scripts ===\n");
    /* ── Lua platform initialization (runs platform.lua phases) ──────── */
    /* Reuse the Lua VM that was opened during platform_conf_scan(). */
    lua_State *lua_L = platform_lua_state();
    if (lua_L) {
        lua_selftest(lua_L);
        lua_run_phase(lua_L, "earlycon");  /* 早期控制台           */
        lua_run_phase(lua_L, "irqcore");   /* GICv2 (AArch64)     */
        lua_run_phase(lua_L, "drivers");   /* timer init + enable  */
    }
    KLOG_INFO("Lua platform phases (earlycon/irqcore/drivers) complete\n");

    /* Run remaining Lua platform phases then close the VM */
    if (lua_L) {
        lua_run_phase(lua_L, "fs");
        lua_run_phase(lua_L, "late");
        lua_platform_close(lua_L);
        lua_L = NULL;
    }


    /* ── 初始化任务子系统 ───────────────────────────────── */
    KLOG_INFO("Initializing task subsystem...\n");
    cpu_init_bsp();          /* Phase 0：安装 BSP per-CPU 指针 */
    task_init();

    /* ── 选择启动模式 ────────────────────────────────────
     *   默认 (run-fs):           启动 busybox 交互 shell
     *   VMM_TEST=1:              VMM 三线程上下文切换测试
     *   （新测试：在此处添加 #elif defined(RUN_XXX_TEST)）
     * ──────────────────────────────────────────────────── */
#if defined(RUN_VMM_TEST)
    KLOG_INFO("=== VMM_TEST mode: 3-thread context switch test ===\n");
    run_vmm_test();
#else
    /* 默认：启动 busybox 交互 shell */
    KLOG_INFO("\n=== Launching busybox shell ===\n");
    task_t *bb_task = task_create("busybox", demo_load_busybox, NULL, 5);
    if (bb_task)
        KLOG_INFO("busybox loader task created: id=%u\n", bb_task->id);
    else
        KLOG_ERROR("Failed to create busybox loader task!\n");
#endif

    /*
     * Phase 3：在 task_init 之后、timer_set_tick_cb 之前拉起 AP。
     * 此时 BSP 的 idle/任务池已就绪；AP 上 timer 中断会触发，但
     * g_tick_cb 仍为 NULL，所以暂不驱动调度。一旦下面
     * timer_set_tick_cb(sched_tick) 写入回调，BSP 和 AP 同时开始
     * 抢占式调度。
     */
    cpu_bring_up_all();

    /* ── 启用抢占，进入 idle 循环（所有模式共用）─────────
     * 在所有任务创建完成后启用，避免 tick 打断内核初始化路径。
     * 切换到 idle 专用栈（防止 boot 栈在频繁中断下溢出）。
     * ──────────────────────────────────────────────────── */
    timer_set_tick_cb(sched_tick);
    KLOG_INFO("Preemptive scheduling enabled\n");

    /* SMP 健康检查：抢占启用后立刻验证所有核 timer 都在 tick。
     * 单核时此函数直接 return，不影响 SMP=1 默认路径。 */
    // cpu_smp_timer_test(3, 500);

    /* Phase 4a：多核线程分发自检。SMP=1 时也会跑（验证 round-robin
     * 自身不破坏单核）。失败仅 KLOG_ERROR，不 panic，避免影响后续
     * busybox / vmm 测试。 */
    // extern void smp_thread_test_run(uint32_t, uint32_t, uint32_t);
    // smp_thread_test_run(4, 200, 800);

    /* 切到 idle 栈并进入 idle 主循环（永不返回）。 */
    task_switch_to_idle_stack();
}

