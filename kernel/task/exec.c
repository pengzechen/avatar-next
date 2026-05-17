/*
 * kernel/task/exec.c — execve 进程执行语义
 *
 * 职责：
 *   1. 用户页表初始化（PGD 分配 + 内核映射复制）
 *   2. Linux ABI 初始栈构建（argc/argv/envp/auxv）
 *   3. 用户栈物理页分配 + 映射
 *   4. 进程创建（process_create_with_pgd）
 *   5. execve 阻塞等待语义（等待子进程退出后以相同码退出）
 */

#include "types.h"
#include "klog.h"
#include "pmm.h"
#include "mm_vm.h"
#include "string.h"
#include "arch.h"
#include "task/task.h"
#include "loader/elf_image.h"
#include "task/exec.h"

/* 内联获取系统时间（ns），用于 CPU 时间计账 */
extern volatile uint64_t g_system_ticks;
extern uintptr_t         g_timer_cfg_counter_hz;
extern unsigned          g_timer_cfg_tick_ms;
#if ARCH_X86_64
extern volatile uint64_t g_tsc_freq_hz;
#endif
static inline uint64_t exec_get_ns(void)
{
#if ARCH_RISCV64
    uint64_t ticks;
    __asm__ volatile("rdtime %0" : "=r"(ticks));
    uintptr_t freq = g_timer_cfg_counter_hz;
    if (!freq) freq = 10000000UL;
    return (ticks / freq) * 1000000000ULL + (ticks % freq) * 1000000000ULL / freq;
#elif ARCH_AARCH64
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    uintptr_t freq = g_timer_cfg_counter_hz;
    if (!freq) freq = 62500000UL;
    return (ticks / freq) * 1000000000ULL + (ticks % freq) * 1000000000ULL / freq;
#else
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t ticks = ((uint64_t)hi << 32) | (uint64_t)lo;
    uint64_t freq  = g_tsc_freq_hz;
    if (!freq) {
        uint64_t tick_ms = g_timer_cfg_tick_ms ? g_timer_cfg_tick_ms : 10ULL;
        return g_system_ticks * tick_ms * 1000000ULL;
    }
    return (ticks / freq) * 1000000000ULL + (ticks % freq) * 1000000000ULL / freq;
#endif
}

#if ARCH_X86_64
#include "x86_64/mmu.h"
#endif
#if ARCH_RISCV64
#include "riscv64/satp_utils.h"
#endif

#include "user_layout.h"

extern pmm_t *g_pmm;

/* USER_STACK_TOP / USER_STACK_SIZE / USER_MMAP_BASE_PIE / USER_MMAP_BASE_EXEC
 * 均来自 include/user_layout.h */

/* ── Linux ABI 初始栈构建 ─────────────────────────────────────── */

/**
 * elf_setup_stack - 在用户栈最高页写入 Linux ABI 初始栈
 * @pgd:       用户页表（内核虚拟地址）
 * @stack_top: 用户栈顶（USER_STACK_TOP）
 * @entry:     ELF 入口点
 * @pathname:  程序路径名
 * @argv:      来自 execve 的 argv（可为 NULL）
 * @envp:      来自 execve 的 envp（可为 NULL）
 * @out_sp:    输出：新的用户栈指针
 * @phdr_uaddr: 程序头的用户虚拟地址（AT_PHDR auxv）
 * @phnum:     e_phnum
 * @phent:     e_phentsize
 *
 * 构建布局（从高地址向低）：
 *   strings (argv[0..n], envp[0..m])
 *   8-byte aligned
 *   auxv: AT_PHDR, AT_PHENT, AT_PHNUM, AT_ENTRY, AT_PAGESZ, AT_RANDOM, AT_NULL
 *   envp ptrs + NULL
 *   argv ptrs + NULL
 *   argc
 *   <- new sp
 */
static int
elf_setup_stack(void *pgd, uint64_t stack_top, uint64_t entry,
                const char *pathname,
                char **argv, char **envp, uint64_t *out_sp,
                uint64_t phdr_uaddr, uint16_t phnum, uint16_t phent,
                uint64_t at_base)
{
    uint64_t top_page_uvaddr = stack_top - PAGE_SIZE;  /* 0x6ffff000 */
    uint64_t top_page_paddr  = mm_vm_get_paddr(pgd, top_page_uvaddr);
    if (top_page_paddr == 0) {
        KLOG_ERROR("[elf] elf_setup_stack: cannot get stack page paddr\n");
        return -1;
    }

    uint8_t *page = (uint8_t *)phys_to_virt(top_page_paddr);
    uint8_t *ptr  = page + PAGE_SIZE;

#define UADDR(p)  (top_page_uvaddr + (uint64_t)((p) - page))

    const char *default_arg0 = pathname ? pathname : "/init";
    const char *default_argv[] = { NULL, NULL };
    static const char *default_envp[] = {
        "PATH=/bin:/usr/bin:/", "HOME=/", "TERM=vt100", NULL
    };

    default_argv[0] = default_arg0;

    const char **av = (const char **)( argv ? (void *)argv : (void *)default_argv );
    const char **ev = (const char **)( envp ? (void *)envp : (void *)default_envp );

    int argc = 0;
    while (av[argc]) argc++;
    int envc = 0;
    while (ev[envc]) envc++;

#define MAX_ARGS 32
    uint64_t av_uaddr[MAX_ARGS];
    uint64_t ev_uaddr[MAX_ARGS];

    if (argc > MAX_ARGS - 1) argc = MAX_ARGS - 1;
    if (envc > MAX_ARGS - 1) envc = MAX_ARGS - 1;

    for (int i = envc - 1; i >= 0; i--) {
        uint64_t len = strlen(ev[i]) + 1;
        ptr -= len;
        memcpy(ptr, ev[i], len);
        ev_uaddr[i] = UADDR(ptr);
    }
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t len = strlen(av[i]) + 1;
        ptr -= len;
        memcpy(ptr, av[i], len);
        av_uaddr[i] = UADDR(ptr);
    }

    /* AT_RANDOM 需要的 16 字节随机区（glibc/musl 启动必需） */
    ptr -= 16;
    {
        uint64_t seed0 = 0x6d5a56da5a6d1234ULL ^ entry;
        uint64_t seed1 = 0xa55aa55a33cc77eeULL ^ stack_top;
        *(uint64_t *)(ptr + 0) = seed0;
        *(uint64_t *)(ptr + 8) = seed1;
    }
    uint64_t at_random_uaddr = UADDR(ptr);

    ptr = (uint8_t *)((uint64_t)ptr & ~7ULL);

#define PUSH64(v) do { ptr -= 8; *(uint64_t *)ptr = (uint64_t)(v); } while(0)
    PUSH64(0);
    PUSH64(0);
    PUSH64(at_random_uaddr);
    PUSH64(25);      /* AT_RANDOM */
    PUSH64(4096);
    PUSH64(6);       /* AT_PAGESZ */
    PUSH64(entry);
    PUSH64(9);       /* AT_ENTRY */
    if (at_base) {
        PUSH64(at_base);
        PUSH64(7);   /* AT_BASE: interpreter 加载基址（静态连接时为 0） */
    }
    if (phdr_uaddr) {
        PUSH64(phnum);
        PUSH64(5);   /* AT_PHNUM */
        PUSH64(phent);
        PUSH64(4);   /* AT_PHENT */
        PUSH64(phdr_uaddr);
        PUSH64(3);   /* AT_PHDR */
    }

    PUSH64(0);
    for (int i = envc - 1; i >= 0; i--)
        PUSH64(ev_uaddr[i]);

    PUSH64(0);
    for (int i = argc - 1; i >= 0; i--)
        PUSH64(av_uaddr[i]);

    PUSH64(argc);

    *out_sp = UADDR(ptr);

    KLOG_INFO("[elf] Initial stack: sp=0x%llx, argc=%d, argv[0]=%s\n",
              *out_sp, argc, av[0]);
#if ARCH_RISCV64
    {
        uint64_t *stack_words = (uint64_t *)ptr;
        for (int i = 0; i < 24; i++) {
            KLOG_INFO("[elf]   stack[%02d] @0x%llx = 0x%llx\n",
                      i,
                      (uint64_t)(*out_sp + (uint64_t)i * 8),
                      stack_words[i]);
        }
    }
#endif

#undef UADDR
#undef PUSH64
#undef MAX_ARGS
    return 0;
}

/* ── execve 主逻辑 ───────────────────────────────────────────────── */

int
task_execve(const char *pathname,
            uint8_t *file_data, uint64_t file_size,
            uint8_t *interp_data, uint64_t interp_size,
            char **argv, char **envp)
{
    elf_image_info_t info;
    uint64_t pgd_phys;
    void    *pgd;
    int      rc;

    /* 1. 分配并清零用户页表 */
    pgd_phys = pmm_alloc_pages(g_pmm, 1);
    if (pgd_phys == 0) {
        KLOG_ERROR("[exec] Failed to allocate PGD\n");
        return -1;
    }
    pgd = phys_to_virt(pgd_phys);
    memset(pgd, 0, PAGE_SIZE);

    /* 2. 复制内核高半区映射（架构特定） */
#if ARCH_X86_64
    {
        extern uint64_t g_kernel_pgd_phys;
        uint64_t *kernel_pml4 = (uint64_t *)phys_to_virt(g_kernel_pgd_phys);
        x86_copy_kernel_mappings((uint64_t *)pgd, kernel_pml4);
        KLOG_INFO("[exec] x86_64 copied kernel PML4[%u..%u] to user PGD=0x%llx\n",
                  X86_PML4_KERNEL_START, X86_PML4_ENTRIES - 1U, pgd_phys);
    }
#endif
#if ARCH_RISCV64
    {
        uint64_t *kernel_l1 = (uint64_t *)phys_to_virt(satp_read_pgd_phys());
        uint64_t *user_l1   = (uint64_t *)pgd;
        riscv64_copy_kernel_mappings(user_l1, kernel_l1);
        KLOG_INFO("[exec] RISC-V kernel mappings: l1[0x%x]=0x%llx l1[0x%x]=0x%llx\n",
                  RISCV64_KERNEL_L1_MMIO_IDX, user_l1[RISCV64_KERNEL_L1_MMIO_IDX],
                  RISCV64_KERNEL_L1_RAM_IDX,  user_l1[RISCV64_KERNEL_L1_RAM_IDX]);
    }
#endif
    /* AArch64: TTBR0/TTBR1 硬件分割，无需复制内核映射 */

    /* 3. 加载主程序 ELF 段 + RELA 重定位 */
    rc = elf_image_load(file_data, file_size, pgd, &info);
    if (rc < 0) {
        KLOG_ERROR("[exec] elf_image_load failed: %d\n", rc);
        return rc;
    }

    /* 3b. 加载动态连接器（如果有）——加载到同一 PGD 的独立地址区 */
    uint64_t exec_entry = info.entry_point;  /* 静态连接时：直接跳入主程序 */
    uint64_t at_base    = 0;
    if (interp_data && interp_size > 0) {
        elf_image_info_t interp_info;
        rc = elf_image_load_at(interp_data, interp_size, pgd,
                               USER_INTERP_BASE, &interp_info);
        if (rc < 0) {
            KLOG_ERROR("[exec] Failed to load interpreter: %d\n", rc);
            return rc;
        }
        exec_entry = interp_info.entry_point;   /* 动态连接：跳入 ld-musl */
        at_base    = interp_info.min_vaddr;     /* AT_BASE = interpreter 实际加载基址 */
        KLOG_INFO("[exec] Interpreter loaded: entry=0x%llx base=0x%llx\n",
                  exec_entry, at_base);
    }

    /* 4. 分配并映射用户栈 */
    uint64_t stack_bottom = ALIGN_DOWN(USER_STACK_TOP - USER_STACK_SIZE, PAGE_SIZE);
    uint64_t stack_pages  = (USER_STACK_TOP - stack_bottom) / PAGE_SIZE;
    uint64_t stack_base_paddr = pmm_alloc_pages(g_pmm, (uint32_t)stack_pages);
    if (stack_base_paddr == 0) {
        KLOG_ERROR("[exec] Failed to allocate %llu stack pages\n", stack_pages);
        return -2;
    }

    KLOG_INFO("[exec] Stack pages allocated: phys=0x%llx - 0x%llx (%llu pages)\n",
              stack_base_paddr, stack_base_paddr + stack_pages * PAGE_SIZE, stack_pages);

    memset(phys_to_virt(stack_base_paddr), 0, stack_pages * PAGE_SIZE);

    /* 检查是否意外分配到了包含 g_pmm 的物理页 */
    {
        uint64_t g_pmm_check_va = (uint64_t)&g_pmm;
        uint64_t g_pmm_check_pa = g_pmm_check_va - KERNEL_VMA;
        if (g_pmm_check_pa >= stack_base_paddr &&
            g_pmm_check_pa < stack_base_paddr + stack_pages * PAGE_SIZE) {
            KLOG_ERROR("[exec] CRITICAL BUG: Stack uses physical page containing g_pmm!\n");
            KLOG_ERROR("[exec]   g_pmm PA=0x%llx in stack range [0x%llx, 0x%llx)\n",
                       g_pmm_check_pa, stack_base_paddr,
                       stack_base_paddr + stack_pages * PAGE_SIZE);
        }
    }

    if (mm_vm_map_pages(pgd, stack_bottom, stack_base_paddr, (int32_t)stack_pages, 0) != 0) {
        KLOG_ERROR("[exec] Failed to map user stack: vaddr=0x%llx pages=%llu\n",
                   stack_bottom, stack_pages);
        pmm_free_pages(g_pmm, stack_base_paddr, (uint32_t)stack_pages);
        return -3;
    }
    KLOG_INFO("[exec] User stack mapped: 0x%llx - 0x%llx\n",
              stack_bottom, (uint64_t)USER_STACK_TOP);

#if ARCH_RISCV64
    /* 调试：检查栈映射后 L1[1] 是否被正确设置 */
    {
        uint64_t *user_l1 = (uint64_t *)pgd;
        KLOG_INFO("[exec] After stack mapping: L1[1]=0x%llx\n", user_l1[1]);
        if (user_l1[1] != 0) {
            uint64_t l1_ppn            = (user_l1[1] >> 10) & 0xfffffffffff;
            uint64_t l1_next_table_pa  = l1_ppn << 12;
            KLOG_INFO("[exec]   L1[1] points to next-level table at phys=0x%llx\n",
                      l1_next_table_pa);

            extern pmm_t *g_pmm;
            extern pmm_t pmm;
            uint64_t g_pmm_va   = (uint64_t)&g_pmm;
            uint64_t g_pmm_pa   = g_pmm_va - KERNEL_VMA;
            uint64_t g_pmm_page = g_pmm_pa & ~0xfff;
            KLOG_INFO("[exec]   g_pmm variable at VA=0x%llx PA=0x%llx (page=0x%llx)\n",
                      g_pmm_va, g_pmm_pa, g_pmm_page);
            if (l1_next_table_pa == g_pmm_page)
                KLOG_ERROR("[exec] BUG: L1[1] points to page containing g_pmm!\n");

            uint64_t test_vaddr    = 0x6ffff000;
            uint64_t mapped_paddr  = mm_vm_get_paddr(pgd, test_vaddr);
            uint64_t expected_paddr = stack_base_paddr + (test_vaddr - stack_bottom);
            KLOG_INFO("[exec] Stack VA 0x%llx -> PA 0x%llx (expected 0x%llx)\n",
                      test_vaddr, mapped_paddr, expected_paddr);
            if (mapped_paddr != expected_paddr)
                KLOG_ERROR("[exec] BUG: Stack mapping incorrect!\n");
            if ((mapped_paddr & ~0xfff) == g_pmm_page)
                KLOG_ERROR("[exec] CRITICAL: Stack page maps to g_pmm page!\n");
        }
    }
#endif

    /* 5. 构建 Linux ABI 初始栈 */
    uint64_t user_sp = USER_STACK_TOP;
    if (elf_setup_stack(pgd, USER_STACK_TOP, info.entry_point, pathname,
                        argv, envp, &user_sp,
                        info.phdr_uaddr, info.phnum, info.phent,
                        at_base) != 0) {
        KLOG_ERROR("[exec] Failed to setup initial stack\n");
        return -4;
    }

    /* 6. 创建用户进程 */
    task_t *current  = task_current();
    uint64_t mmap_base = (info.min_vaddr == USER_CODE_BASE) ? USER_MMAP_BASE_PIE
                                                              : USER_MMAP_BASE_EXEC;

    task_t *new_task = process_create_with_pgd(
        pathname, exec_entry, user_sp, 10, pgd_phys,
        ALIGN_UP(info.max_vaddr, PAGE_SIZE), mmap_base);

    if (new_task == NULL) {
        KLOG_ERROR("[exec] Failed to create task\n");
        return -5;
    }

    uint32_t new_task_id = new_task->id;
    KLOG_INFO("[exec] Process '%s' created, PID=%u, pgd=0x%llx, parent=%u\n",
              pathname, new_task->id, pgd_phys, new_task->parent_id);
    KLOG_INFO("[exec]   heap_start=0x%llx, mmap_base=0x%llx\n",
              new_task->heap_end, new_task->mmap_next);

    /* 7. 阻塞当前进程，等待新进程退出 */
    current->is_waiting = true;
    current->wait_pid   = new_task_id;
    task_block(NULL);

    /* 8. 新进程已退出：获取退出状态并释放槽位，然后以相同状态退出 */
    {
        extern task_t  g_task_pool[];
        extern uint8_t g_stack_used[];
        for (uint32_t i = 0; i < TASK_MAX; i++) {
            if (!g_stack_used[i]) continue;
            if (g_task_pool[i].id == new_task_id) {
                current->exit_status = g_task_pool[i].exit_status;
                current->stime_ns   += g_task_pool[i].stime_ns;  /* 继承子进程内核时间 */
                g_stack_used[i] = 0;
                g_task_pool[i].stack_base = NULL;
                break;
            }
        }
    }

    /* 在 task_exit 前计算 utime： wall_time − stime（已包含子进程的 stime） */
    if (current->is_user_process) {
        uint64_t _now = exec_get_ns();
        if (current->sc_entry_ns != 0) {
            current->stime_ns += _now - current->sc_entry_ns;
            current->sc_entry_ns = 0;
        }
        if (current->create_ns != 0) {
            uint64_t _wall = _now - current->create_ns;
            current->utime_ns = (_wall > current->stime_ns) ? _wall - current->stime_ns : 0;
        }
    }

    task_exit();

    return 0;  /* unreachable */
}
