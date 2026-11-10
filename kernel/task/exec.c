/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/exec.c — execve 进程执行语义(Linux 风格: 原地替换映像)

   职责：
     1. 新用户页表初始化(PGD 分配 + 内核映射复制)
     2. Linux ABI 初始栈构建(argc/argv/envp/auxv)
     3. 新映像的段/解释器加载 + 新用户栈映射
     4. 原地提交：换页表/入口, 保留 PID、fd、信号掩码等进程属性
     5. 失败时旧映像保持原样(Linux execve 语义)

   与旧实现的区别：以前是"新建任务 + 调用者阻塞当 wrapper, 等子进程退出,
   再以相同状态退出"。那样每次 exec 都会在任务表里留下一个同名的阻塞空壳
   (ps 里能直接看到), 而且被 exec 程序的 PID 会变。现在直接替换当前任务的
   地址空间和 trap_frame：pid/tgid 不变, 没有 wrapper, ps 的父子关系与
   Linux 一致。
*/

#include "types.h"
#include "klog.h"
#include "pmm.h"
#include "mm_vm.h"
#include "string.h"
#include "arch.h"
#include "exception.h"
#include "syscall_abi.h"
#include "task/task.h"
#include "task/switch.h"
#include "loader/elf_image.h"
#include "task/exec.h"
#include "vm_user.h"
#include "user_layout.h"

/*
   fd 池接口在这里只能手写 extern：本文件用普通 CFLAGS 编译, 没有 lwext4
   的头文件路径, 而 syscall/fs/fd_pool.h -> vfs.h -> ext4.h 会直接爆
   `ext4.h: No such file or directory`。签名以 fd_pool.h 为准, 改那边时
   记得同步这几行。
*/
extern void fd_pool_free(int idx);
extern void fd_obj_close(int idx);
extern int task_take_fd(task_t *task, int fd);

#if ARCH_X86_64
/* 定义在 kernel/syscall/mm/pmap_compat.c / boot/x86_64/tss.c */
extern void x86_write_fs_base(uint64_t fs_base);
extern void x86_tss_set_rsp0(uint64_t rsp0);
#endif

/* USER_STACK_TOP / USER_STACK_SIZE / USER_MMAP_BASE_PIE / USER_MMAP_BASE_EXEC
   均来自 include/user_layout.h
*/

/* ── Linux ABI 初始栈构建 ─────────────────────────────────────── */

/*
   elf_setup_stack - 在新用户栈最高页写入 Linux ABI 初始栈
   @pgd:       用户页表(内核虚拟地址)
   @stack_top: 用户栈顶(USER_STACK_TOP)
   @entry:     ELF 入口点
   @pathname:  程序路径名
   @argv:      来自 execve 的 argv(可为 NULL)
   @envp:      来自 execve 的 envp(可为 NULL)
   @out_sp:    输出：新的用户栈指针
   @phdr_uaddr: 程序头的用户虚拟地址(AT_PHDR auxv)
   @phnum:     e_phnum
   @phent:     e_phentsize

   构建布局(从高地址向低)：
     strings (argv[0..n], envp[0..m])
     8-byte aligned
     auxv: AT_PHDR, AT_PHENT, AT_PHNUM, AT_ENTRY, AT_PAGESZ, AT_RANDOM, AT_NULL
     envp ptrs + NULL
     argv ptrs + NULL
     argc
     <- new sp
*/
static int elf_setup_stack(void *pgd, uint64_t stack_top, uint64_t entry, const char *pathname, char **argv,
                           char **envp, uint64_t *out_sp, uint64_t phdr_uaddr, uint16_t phnum, uint16_t phent,
                           uint64_t at_base)
{
    uint64_t top_page_uvaddr = stack_top - PAGE_SIZE; /* 0x6ffff000 */
    uint64_t top_page_paddr = mm_vm_get_paddr(pgd, top_page_uvaddr);
    if (top_page_paddr == 0) {
        KLOG_ERROR("[elf] elf_setup_stack: cannot get stack page paddr\n");
        return -1;
    }

    uint8_t *page = (uint8_t *)phys_to_virt(top_page_paddr);
    uint8_t *ptr = page + PAGE_SIZE;

#define UADDR(p) (top_page_uvaddr + (uint64_t)((p) - page))

    const char *default_arg0 = pathname ? pathname : "/init";
    const char *default_argv[] = { NULL, NULL };
    static const char *default_envp[] = { "PATH=/bin:/usr/bin:/sbin:/usr/sbin", "HOME=/", "TERM=vt100", NULL };

    default_argv[0] = default_arg0;

    const char **av = (const char **)(argv ? (void *)argv : (void *)default_argv);
    const char **ev = (const char **)(envp ? (void *)envp : (void *)default_envp);

    int argc = 0;
    while (av[argc])
        argc++;
    int envc = 0;
    while (ev[envc])
        envc++;

#define MAX_ARGS 128
    uint64_t av_uaddr[MAX_ARGS];
    uint64_t ev_uaddr[MAX_ARGS];

    if (argc > MAX_ARGS - 1)
        argc = MAX_ARGS - 1;
    if (envc > MAX_ARGS - 1)
        envc = MAX_ARGS - 1;

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

    /* AT_RANDOM 需要的 16 字节随机区(glibc/musl 启动必需) */
    ptr -= 16;
    {
        uint64_t seed0 = 0x6d5a56da5a6d1234ULL ^ entry;
        uint64_t seed1 = 0xa55aa55a33cc77eeULL ^ stack_top;
        *(uint64_t *)(ptr + 0) = seed0;
        *(uint64_t *)(ptr + 8) = seed1;
    }
    uint64_t at_random_uaddr = UADDR(ptr);

    ptr = (uint8_t *)((uint64_t)ptr & ~7ULL);

#define PUSH64(v) \
    do { \
        ptr -= 8; \
        *(uint64_t *)ptr = (uint64_t)(v); \
    } while (0)
    PUSH64(0);
    PUSH64(0);
    PUSH64(at_random_uaddr);
    PUSH64(25); /* AT_RANDOM */
    PUSH64(4096);
    PUSH64(6); /* AT_PAGESZ */
    PUSH64(entry);
    PUSH64(9); /* AT_ENTRY */
    if (at_base) {
        PUSH64(at_base);
        PUSH64(7); /* AT_BASE: interpreter 加载基址(静态连接时为 0) */
    }
    if (phdr_uaddr) {
        PUSH64(phnum);
        PUSH64(5); /* AT_PHNUM */
        PUSH64(phent);
        PUSH64(4); /* AT_PHENT */
        PUSH64(phdr_uaddr);
        PUSH64(3); /* AT_PHDR */
    }

    PUSH64(0);
    for (int i = envc - 1; i >= 0; i--)
        PUSH64(ev_uaddr[i]);

    PUSH64(0);
    for (int i = argc - 1; i >= 0; i--)
        PUSH64(av_uaddr[i]);

    PUSH64(argc);

    *out_sp = UADDR(ptr);

#undef UADDR
#undef PUSH64
#undef MAX_ARGS
    return 0;
}

/* ── 原地 exec 的进程状态迁移 ─────────────────────────────────── */

static void exec_free_loader_buffer(const void *data, uint64_t size)
{
    if (!data || size == 0)
        return;

    uint32_t pages = (uint32_t)(ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE);
    pmm_free_pages(&g_pmm, virt_to_phys((uint64_t)data), pages);
}

/*
   exec_close_cloexec_fds - 关闭带 FD_CLOEXEC 的 fd(其余保留)

   旧实现是"新任务继承 fd 后把 wrapper 的全部 fd 关掉"; 原地 exec 保留
   当前 fd 表, 只按 Linux 语义清理 CLOEXEC 项。
*/
static void exec_close_cloexec_fds(task_t *t)
{
    for (int fd = 0; fd < (int)TASK_MAX_FD; fd++) {
        if (((t->fd_cloexec[fd / 8] >> (fd % 8)) & 1) == 0)
            continue;

        int idx = task_take_fd(t, fd);
        if (idx < 0)
            continue;
        fd_obj_close(idx);
        fd_pool_free(idx);
    }
    memset(t->fd_cloexec, 0, sizeof(t->fd_cloexec));
}

/*
   exec_reset_signal_state - Linux exec 的信号语义

     - 被 caught 的信号处理函数重置为 SIG_DFL
     - SIG_IGN 原样保留(这是 glibc/musl 在 exec 后仍能忽略 SIGCHLD 等的前提)
     - 信号掩码 blocked_sigs 保留(POSIX: 跨 exec 不变)
     - 正在执行的 handler 栈帧作废(从 handler 里 execve 的极端场景)

   pending_sigs 不清：Linux 也没有在 execve 里清 pending 信号。
*/
static void exec_reset_signal_state(task_t *t)
{
    for (int i = 0; i < NSIG; i++) {
        if (t->sig_actions[i].sa_handler == SIG_IGN)
            continue;
        t->sig_actions[i].sa_handler = SIG_DFL;
        t->sig_actions[i].sa_flags = 0;
        t->sig_actions[i].sa_restorer = 0;
        t->sig_actions[i].sa_mask = 0;
    }
    t->sig_frame_sp = 0;
    t->sig_saved_blocked = 0;
}

/*
   exec_set_name - 更新 comm(/proc/<pid>/stat 的 Name)和 /proc/self/exe

   Linux 的 comm 是可执行文件的 basename(15 字节), /proc/self/exe 是
   解析过符号链接的绝对路径。以前两者都是加载时的原始入参。
*/
static void exec_set_name(task_t *t, const char *pathname)
{
    const char *base = pathname;
    for (const char *p = pathname; *p; p++) {
        if (*p == '/')
            base = p + 1;
    }
    if (*base == '\0')
        base = pathname;

    uint32_t i = 0;
    while (base[i] && i < TASK_NAME_LEN - 1) {
        t->name[i] = base[i];
        i++;
    }
    t->name[i] = '\0';

    i = 0;
    while (pathname[i] && i < TASK_EXE_LEN - 1) {
        t->exe_path[i] = pathname[i];
        i++;
    }
    t->exe_path[i] = '\0';
}

/* ── execve 主逻辑 ───────────────────────────────────────────────── */

/*
   task_execve - 把 ELF 映像原地装进当前进程并执行

   @frame: 用户进程从 syscall 进来时传入 trap_frame(返回时改它);
           内核线程(开机 busybox_loader)传 NULL, 走 arch_enter_user。

   成功时：用户进程返回 0(syscall 返回路径随后把控制权交给新程序);
           内核线程不返回, 直接进入用户态。
   失败时：返回负错误码, 当前进程的旧映像/fd/信号状态完全不变。
*/
int task_execve(const char *pathname, uint8_t *file_data, uint64_t file_size, uint8_t *interp_data,
                uint64_t interp_size, char **argv, char **envp, trap_frame_t *frame)
{
    elf_image_info_t info;
    uint64_t pgd_phys;
    void *pgd;
    int rc;
    task_t *current = task_current();

    if (current == NULL) {
        exec_free_loader_buffer(interp_data, interp_size);
        exec_free_loader_buffer(file_data, file_size);
        return -1;
    }

    /* 1. 分配并清零新用户页表 —— 旧映像此时完全不受影响 */
    pgd_phys = pmm_alloc_pages(&g_pmm, 1);
    if (pgd_phys == 0) {
        KLOG_ERROR("[exec] Failed to allocate PGD\n");
        exec_free_loader_buffer(interp_data, interp_size);
        exec_free_loader_buffer(file_data, file_size);
        return -1;
    }
    pgd = phys_to_virt(pgd_phys);
    memset(pgd, 0, PAGE_SIZE);

    /* 2. 注入架构要求的内核映射(AArch64 为空操作) */
    mm_vm_init_user_pgd(pgd, pgd_phys);

    /* 3. 加载主程序 ELF 段 + RELA 重定位 */
    rc = elf_image_load(file_data, file_size, pgd, &info);
    if (rc < 0) {
        KLOG_ERROR("[exec] elf_image_load failed: %d\n", rc);
        goto fail;
    }

    /* 3b. 加载动态连接器(如果有)——加载到同一 PGD 的独立地址区 */
    uint64_t exec_entry = info.entry_point; /* 静态连接时：直接跳入主程序 */
    uint64_t at_base = 0;
    uint64_t loaded_end = info.max_vaddr;
    /*
       interp 区的末端, 记到 task->interp_end 供 fork 复制用。
       0 = 本进程是静态链接的(绝大多数程序), fork 时不必扫这一带。
    */
    uint64_t interp_end = 0;
    if (interp_data && interp_size > 0) {
        elf_image_info_t interp_info;
        rc = elf_image_load_at(interp_data, interp_size, pgd, USER_INTERP_BASE, &interp_info);
        if (rc < 0) {
            KLOG_ERROR("[exec] Failed to load interpreter: %d\n", rc);
            goto fail;
        }
        exec_entry = interp_info.entry_point; /* 动态连接：跳入 ld-musl */
        at_base = interp_info.min_vaddr;      /* AT_BASE = interpreter 实际加载基址 */
        if (interp_info.max_vaddr > loaded_end)
            loaded_end = interp_info.max_vaddr;
        interp_end = ALIGN_UP(interp_info.max_vaddr, PAGE_SIZE);
        KLOG_DEBUG("[exec] Interpreter loaded: entry=0x%llx base=0x%llx "
                   "end=0x%llx\n",
                   exec_entry, at_base, interp_end);
    }

    /* 4. 分配并映射用户栈 */
    uint64_t stack_bottom = ALIGN_DOWN(USER_STACK_TOP - USER_STACK_SIZE, PAGE_SIZE);
    uint64_t stack_pages = (USER_STACK_TOP - stack_bottom) / PAGE_SIZE;
    uint64_t stack_base_paddr = pmm_alloc_pages(&g_pmm, (uint32_t)stack_pages);
    if (stack_base_paddr == 0) {
        KLOG_ERROR("[exec] Failed to allocate %llu stack pages\n", stack_pages);
        rc = -2;
        goto fail;
    }

    memset(phys_to_virt(stack_base_paddr), 0, stack_pages * PAGE_SIZE);

    if (mm_vm_map_pages(pgd, stack_bottom, stack_base_paddr, (int32_t)stack_pages, VM_PERM_USER) != 0) {
        KLOG_ERROR("[exec] Failed to map user stack: vaddr=0x%llx pages=%llu\n", stack_bottom, stack_pages);
        pmm_free_pages(&g_pmm, stack_base_paddr, (uint32_t)stack_pages);
        rc = -3;
        goto fail;
    }

    /* 4b. 映射信号返回蹦床页(x86_64 用户栈 NX, 不能放 trampoline 在栈上) */
#if ARCH_X86_64
    {
        uint64_t sigret_pa = pmm_alloc_pages(&g_pmm, 1);
        if (sigret_pa) {
            uint8_t *p = (uint8_t *)phys_to_virt(sigret_pa);
            memset(p, 0xcc, PAGE_SIZE); /* int3 填充(安全兜底) */
            /* mov $15, %eax; syscall  — rt_sigreturn on x86_64 */
            p[0] = 0xb8;
            p[1] = 0x0f;
            p[2] = 0x00;
            p[3] = 0x00;
            p[4] = 0x00;
            p[5] = 0x0f;
            p[6] = 0x05;
            mm_vm_map_pages(pgd, USER_SIGRET_PAGE, sigret_pa, 1, VM_PERM_USER);
        }
    }
#endif

    /* 5. 构建 Linux ABI 初始栈 */
    uint64_t user_sp = USER_STACK_TOP;
    if (elf_setup_stack(pgd, USER_STACK_TOP, info.entry_point, pathname, argv, envp, &user_sp, info.phdr_uaddr,
                        info.phnum, info.phent, at_base) != 0) {
        KLOG_ERROR("[exec] Failed to setup initial stack\n");
        rc = -4;
        goto fail;
    }

    /* 6. 计算堆/ mmap 基址(与原实现一致) */
    uint64_t mmap_base = (info.min_vaddr == USER_CODE_BASE) ? USER_MMAP_BASE_PIE : USER_MMAP_BASE_EXEC;
    uint64_t loaded_end_aligned = ALIGN_UP(loaded_end, PAGE_SIZE);
    if (loaded_end_aligned > mmap_base)
        mmap_base = loaded_end_aligned;

    /*
       新映像已经完整可用, loader 缓冲不再需要。从这里开始进入提交阶段,
       没有失败路径。缓冲归本函数所有: elf_loader 在 task_execve 返回后
       不得再释放它们。
    */
    exec_free_loader_buffer(interp_data, interp_size);
    exec_free_loader_buffer(file_data, file_size);

    /*
       ── 提交点 ──────────────────────────────────────────────────
       关中断的原因：改 task->pgd 与真正切换 MMU 之间如果被抢占,
       aarch64 的 arch_task_switch 会把"当前 TTBR0"(还是旧页表)存回
       prev->pgd, 把刚写进去的新页表指针覆盖掉。
    */
    uint64_t irq_flags = arch_irq_save();

    uint64_t old_pgd_phys = 0;
    bool old_pgd_owned = false;
    if (current->is_user_process && current->pgd != NULL) {
        old_pgd_phys = (uint64_t)current->pgd;
        old_pgd_owned = !current->shares_pgd;
    }
#if ARCH_X86_64
    bool was_user = current->is_user_process && current->user_started;
#endif
    bool other_threads = task_group_has_other_threads(current);

    exec_close_cloexec_fds(current);
    exec_reset_signal_state(current);
    exec_set_name(current, pathname);

    /* 换映像。PID/tgid、parent_id、优先级、cwd、uid/gid、pgid/sid、
       blocked_sigs、累计 CPU 时间都保留(Linux 语义)。 */
    current->pgd = (uint64_t *)pgd_phys;
    current->is_user_process = true;
    current->user_started = true;
    current->is_thread = false;
    current->shares_pgd = false;
    current->tgid = current->id;
    current->ctid_ptr = 0;
    current->user_entry = exec_entry;
    current->user_sp = user_sp;
    current->user_stack_top = USER_STACK_TOP;
    current->user_stack_size = USER_STACK_SIZE;
    current->heap_end = ALIGN_UP(info.max_vaddr, PAGE_SIZE);
    current->mmap_base = mmap_base;
    current->mmap_next = mmap_base;
    current->interp_end = interp_end;
    current->fs_base = 0;
    /* exec 重置 robust_list（Linux 语义）；pdeathsig/umask/groups/rlimits 保留 */
    current->robust_list = 0;
    current->sleep_expire_ns = 0;
    current->dumpable = 1;

    /* 7. 把本 CPU 切到新页表; 必须早于销毁旧页表 */
    mm_vm_switch_user(pgd_phys);

#if ARCH_X86_64
    /*
       返回用户态不再经过调度器(sched.c 里的 CR3/TSS/FS 设置), 这里补上:
         - 内核任务 exec 时 TSS.RSP0 还指着别人/初始值
         - exec 不继承 TLS, FS_BASE 必须清零
    */
    if (!was_user)
        x86_tss_set_rsp0((uint64_t)current->stack_base + TASK_STACK_SIZE);
    x86_write_fs_base(0);
#endif

    KLOG_DEBUG("[exec] pid=%u '%s' entry=0x%llx sp=0x%llx pgd=0x%llx\n", current->id, current->name, exec_entry,
               user_sp, pgd_phys);

    /*
       用户进程：改写 trap frame, 按原路(syscall 返回)回到新程序的入口。
       内核任务：没有 trap frame, 直接进入用户态, 不返回。
    */
    if (frame != NULL) {
        syscall_abi_reset_frame_for_exec(frame, exec_entry, user_sp);
        arch_irq_restore(irq_flags);

        if (old_pgd_phys != 0 && old_pgd_owned) {
            if (!other_threads) {
                vm_destroy_user_process(old_pgd_phys);
            } else {
                /*
                   同一线程组还有别的活线程在共用旧页表, 不能销毁。
                   Linux 会先 de_thread 杀掉其它线程; 本内核暂不支持从
                   多线程进程 exec, 这里只把当前任务摘成独立进程, 旧页表
                   留给线程组(最后一个引用者退出时才有机会回收)。
                */
                KLOG_WARN_ONCE("[exec] pid=%u exec with live threads: old pgd=0x%llx kept\n", current->id,
                               old_pgd_phys);
            }
        }
        return 0;
    }

    /* 内核线程 exec：旧 pgd 为 NULL; 若将来有, 也只在线程组独占时销毁 */
    if (old_pgd_phys != 0 && old_pgd_owned && !other_threads)
        vm_destroy_user_process(old_pgd_phys);

    arch_enter_user(exec_entry, user_sp);

fail:
    vm_destroy_user_process(pgd_phys);
    exec_free_loader_buffer(interp_data, interp_size);
    exec_free_loader_buffer(file_data, file_size);
    return rc;
}
