/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   Kernel main entry point
   Called from architecture-specific boot code
*/

#include "boot.h"
#include "platform_ops.h"
#include "arch.h"
#include "klog.h"
#include "debug/backtrace.h" /* backtrace_selftest() */
#include "cache.h"
#include "string.h"
#include "exception.h" /* exception_init / arch_irq_*：三个架构统一接口 */
#include "task/task.h"
#include "task/sched.h"
#include "task/cpu.h"
#include "pmm.h"
#include "mm_vm.h" /* virt_to_phys：把链接符号换算成物理地址 */
#include "driver/time.h"
#include "net/net.h"
#include "loader/elf_loader.h"
#include "../fs/lwext4_port/fs_init.h"

/*
   测试相关接口。boot_mode.h 是启动变体钩子；unit_test.h 是默认启动路径上的
   自检入口。具体实现都在 tests/unit-test/，main.c 不包含测试细节。
*/
#include "../tests/boot_mode.h"
#include "../tests/unit_test.h"

/*
   架构专属头。x86_64 没有额外需要: exception_init 走通用 exception.h。
*/
#if ARCH_AARCH64
#include "aarch64/cpu.h" /* aarch64_enable_neon() */
#endif

#include "driver/driver.h"

/*
   默认启动模式：正常 busybox。测试变体（GUEST_LINUX=1 / VMM_TEST=1）链接的
   tests/unit-test/{guest_test,vcpu_thread_test}.c 提供同名强定义，链接时自动覆盖这里的 weak 版本。

   必须 noinline：weak 函数可被 interpose，但 GCC 在 -O2 下仍可能把同 TU 的
   调用内联成常量；内联后就看不到测试变体的强定义了。
*/
__attribute__((weak, noinline)) bool test_boot_active(void)
{
    return false;
}

__attribute__((weak, noinline)) bool test_boot_skip_net(void)
{
    return false;
}

__attribute__((weak, noinline)) void test_boot_run(void)
{}

static void load_busybox(void *arg)
{
    (void)arg;

    /* 等待文件系统初始化 */
    KLOG_INIT("[busybox_loader] Waiting for filesystem...\n");
    for (int i = 0; i < 100; i++) {
        task_yield();
    }

    KLOG_INFO("[busybox_loader] Loading /busybox from filesystem...\n");

    /* 以 busybox 的 sh applet 进入交互 shell */
    char *bb_argv[] = { "sh", "-i", NULL };
    char *bb_envp[] = { "PATH=/bin:/usr/bin:/sbin:/usr/sbin", "HOME=/root", "TERM=vt100", NULL };

    /* 调用 ELF 加载器执行 /busybox */
    const char *path = "/busybox";
    int rc = elf_loader_load_from_file(path, bb_argv, bb_envp);

    if (rc != 0) {
        KLOG_ERROR("[busybox_loader] Failed to load /busybox: %d\n", rc);
        KLOG_INFO("[busybox_loader] Run: ./tools/install-apps.sh aarch64\n");
    }

    /* 任务完成 */
    KLOG_INIT("[busybox_loader] Exiting...\n");
    task_exit();
}

void kernel_main(void)
{
    /*
       平台配置是纯数据(来自 build/<platform>/platform_static.c), 不碰硬件;
       UART 驱动要用它查 MMIO 基址, 所以必须先跑。
    */
    platform_conf_init();

    /*
       UART 必须早于任何 KLOG —— 下面架构初始化里就有日志。此时异常向量/
       中断控制器可以尚未初始化, 本步只做轮询模式的串口配置。
    */
    platform_early_init();

#if ARCH_AARCH64
    /* Keep NEON enabled for AArch64 code paths that require it. */
    aarch64_enable_neon();
#endif
#if ARCH_X86_64
    /* Initialize IDT and LAPIC */
    KLOG_INIT("Initializing IDT + LAPIC...\n");
    exception_init();
    /*
       TSS(特权级切换用)在 cpu_init_bsp() 之后初始化：TSS.RSP0 要同步进
       cpu_t::kernel_rsp0, 而 cpu_init_bsp() 会把 g_cpus[0] 整个 memset 掉。
       IDT 不使用 IST(ist=0), 所以这里晚一点设置不影响异常处理。
    */
#endif
#if ARCH_RISCV64
    /* 必须在 fs_init() 之前设置 stvec, 否则 ext4_mount 中的任何
       CPU 异常都会落到 M-mode (OpenSBI), 导致 hart 被重置
    */
    KLOG_INIT("Initializing exception handler...\n");
    exception_init();
#endif

#if ARCH_RISCV64
    /* mmu.S 建立的 1GiB 内核映射必须与 RV_KERNEL_MAP_LIST 完全一致；
       缺一条就 panic 在启动期, 而不是等 syscall 期间 Store PF。
    */
    mm_vm_verify_kernel_mappings();
#endif

    /*
       应用编译期模块白名单(Makefile 的 LOG_MODULES=)并打印生效配置。
       必须在 UART 可用之后, 任何模块日志之前。
    */
    klog_init();

    /*
       调用栈回溯自检：造一条已知的调用链, 验证"展开 + 符号化"整条路是通的。

       放在这么早(任务系统, 页表都还没起来)是有意的 —— 它只读栈和
       .kallsyms 表, 两者在镜像加载完就都在了。正常时完全静默, 检查失败
       才用 kprintf 报错(不受 LOG= 影响)。

       为什么值得每次开机都跑：kallsyms 表是两步链接生成的, 一旦段序被挪动,
       镜像本身完全正常, 但 panic 打出来的函数名会整体错位 —— 假的调用栈比
       打不出来更危险, 它会把人带到完全错误的方向。这里当场喊出来。
    */
    /* backtrace_selftest(); */

    /*
       探测/设定缓存行大小(cache API 的 range 操作用它做步长)。
       以前从来没人调用, 于是所有架构都吃硬编码的 64。
    */
    init_cache();

    /* Print welcome message */
    /* 一条事实一行 —— 拆成三行没有多出任何信息 */
    KLOG_INFO("=== Avatar OS Kernel " ARCH_NAME " build " __DATE__ " " __TIME__ " ===\n");

    /* ── 字符串/内存函数自检(STRING_TEST=1, 跑完继续启动)──────── */
#ifdef RUN_STRING_TEST
    extern void test_string_functions(void); /* tests/unit-test/string_test.c */
    test_string_functions();
#endif

    /* ── 初始化物理内存管理器 ───────────────────────────────── */
    /*
       链接脚本导出的 __kernel_image_start/end 是虚拟地址：高半区代码
       对低物理地址的 ABS 符号做 PC 相对寻址会超出 ADRP/auipc/RIP-rel 的
       范围(实测 aarch64 报 R_AARCH64_ADR_PREL_PG_HI21 truncated to fit)。
       换算放在这里 —— boot 层知道 KERNEL_VMA, PMM 只收物理地址。
    */
    extern char __kernel_image_start[];
    extern char __kernel_image_end[];
    pmm_initialize(virt_to_phys((uint64_t)__kernel_image_start), virt_to_phys((uint64_t)__kernel_image_end));

    /*
       驱动阶段（一）：中断控制器、时间基准、根块设备。

       IRQ/TIME 不依赖文件系统，提前初始化；STORAGE 必须先于 fs_init()，
       由 ramblk/sdblk 在 probe 里注册根块设备。
    */
    drivers_init(DRV_PHASE_IRQ);
    drivers_init(DRV_PHASE_TIME);
    drivers_init(DRV_PHASE_STORAGE);

    KLOG_FS("Initializing filesystem...\n");
    fs_init();

    /*
       测试入口：普通启动在这里什么都不跑；只有 PMM_TEST=1 / KMEM_TEST=1
       等测试变体才会执行对应测试（具体内容在 tests/unit-test/unit_test.c）。
    */
    run_unit_tests();

    /* 驱动阶段（二）：DMA/ION/NPU/TPU */
    drivers_init(DRV_PHASE_CORE);

    /* ── 初始化任务子系统 ───────────────────────────────── */
    KLOG_TASK("Initializing task subsystem...\n");
    cpu_init_bsp(); /* Phase 0：安装 BSP per-CPU 指针 */

#if ARCH_X86_64
    extern void x86_tss_init(void);
    x86_tss_init(); /* 必须在 cpu_init_bsp() 之后(见上方说明) */
#endif

    task_init();

    /*
       先把 AP 拉起来, 再创建任何可能被摊核的任务。

       vCPU 任务的亲和性在创建时就算好了((slot + vcpu_id) % g_num_cpus)。
       从前这一步在模式分支之后, 直启 guest 时 g_num_cpus 还是 1, 于是
       4 个 vCPU 全被钉在 cpu0 —— guest 侧看是 4 核, 宿主只有一颗核在
       分时, AP bring-up 和 IPI 都会被拖死。放到任务创建之前, 任何模式
       (直启 guest / busybox / 网络)都能按真实核数摊开。
    */
    cpu_bring_up_all();

    /*
       驱动阶段（三）：网卡；net_init() 需要驱动先注册 netdev。
       guest 直启和 VMM 自测不需要网络，由测试变体的 boot 对象声明跳过。
    */
    if (!test_boot_skip_net()) {
        drivers_init(DRV_PHASE_NET);
        if (driver_present(DRV_NET)) {
            KLOG_NET("Initializing network stack...\n");
            net_init();
        }
    }

    /*
       选择启动模式：
         默认 / 普通 shell      → busybox
         GUEST_LINUX=1          → tests/unit-test/guest_test.c
         VMM_TEST=1             → tests/unit-test/vcpu_thread_test.c
    */
    if (test_boot_active()) {
        test_boot_run();
    } else {
        uint64_t startup_task_irq_flags = arch_irq_save();

        if (driver_present(DRV_NET)) {
            KLOG_NET("Starting network polling task...\n");
            task_t *eth_task = task_create("net-poll", net_poll_task, NULL, 20);
            if (eth_task)
                KLOG_TASK("network task created: id=%u\n", eth_task->id);
            else
                KLOG_ERROR("Failed to create network task!\n");
        }

        /* SMP 检查完成, 现在才启动 busybox 交互 shell */
        task_t *bb_task = task_create("busybox_loader", load_busybox, NULL, 5);
        arch_irq_restore(startup_task_irq_flags);
        if (bb_task)
            KLOG_TASK("busybox loader task created: id=%u\n", bb_task->id);
        else
            KLOG_ERROR("Failed to create busybox loader task!\n");
    }

    /*
       SMP 结果打一条 INFO。

       cpu_bring_up_all() 内部逐核的 "cpuN online" 和收尾的
       "SMP bringup done: %u CPU(s) online" 都是 KLOG_SMP(== 模块 DEBUG),
       于是 LOG=info 下一条都看不见 —— 整个启动日志里就没有"这台机器到底
       几个核在跑"这个事实, 而它决定了后面所有的并发行为(调度器, per-CPU
       状态, VMM 的核间竞态, GIC 投递)。实测在 rk3588 上就是这个症状。

       两个数一起报：CONFIG_SMP_CPUS 是编译期请求的, g_num_cpus 是实际
       上线的。两者不等(PSCI CPU_ON 失败, 核被固件扣住)是最该一眼看出来的
       情况, 只报一个的话那种情况会伪装成一切正常。
    */
    KLOG_INFO("[cpu] SMP: %u/%u CPU(s) online\n", (unsigned)g_num_cpus, (unsigned)CONFIG_SMP_CPUS);

    /* ── 启用抢占, 进入 idle 循环(所有模式共用)─────────
       在所有任务创建完成后启用, 避免 tick 打断内核初始化路径。
       切换到 idle 专用栈(防止 boot 栈在频繁中断下溢出)。
       ────────────────────────────────────────────────────
    */
    timer_set_tick_cb(sched_tick);

    KLOG_INFO("Preemptive scheduling enabled\n");

    /* SMP 健康检查：抢占启用后立刻验证所有核 timer 都在 tick。 */
    cpu_smp_timer_test(3, 100);

    /* Phase 4a：多核线程分发自检。SMP=1 时也会跑(验证 round-robin
       自身不破坏单核)。失败仅 KLOG_ERROR, 不 panic, 避免影响后续
       busybox / vmm 测试。
    */
    /*
       extern void smp_thread_test_run(uint32_t, uint32_t, uint32_t);
       smp_thread_test_run(4, 200, 800);
    */

    /* 切到 idle 栈并进入 idle 主循环(永不返回)。 */
    task_switch_to_idle_stack();
}
