/*
 * core/proc_lifecycle.c - 进程/线程生命周期 syscall
 *
 * 从 kernel/syscall/syscall.c 抽出，覆盖：
 *   exit / exit_group        → sys_exit() + exit_handler()
 *   clone (fork + thread)    → clone_handler()
 *   execve                   → sys_execve() + execve_handler()
 *   wait4 / waitid           → wait_handler()
 *
 * sys_exit / sys_execve 在 syscall.h 中已有 public 声明。
 */
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/core/futex.h"
#include "loader/elf_loader.h"
#include "task/task.h"
#include "task/sched.h"
#include "task/switch.h"
#include "user_layout.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "klog.h"
#include "arch.h"
#include "syscall_abi.h"
#include "list.h"
#include <ext4.h>
#if ARCH_RISCV64
#include "riscv64/satp_utils.h"
#endif
#if ARCH_X86_64
#include "x86_64/mmu.h"
#endif

/* 任务池（定义在 kernel/task/task.c） */
extern task_t  g_task_pool[TASK_MAX];
extern uint8_t g_stack_used[TASK_MAX];
extern uint8_t g_task_stacks[TASK_MAX][TASK_STACK_SIZE];
extern uint32_t g_task_id_cnt;

/* ───────────────────────────────────────────────────────────────
 *  sys_exit
 * ─────────────────────────────────────────────────────────────── */
void sys_exit(int status)
{
    task_t *current = task_current();

    KLOG_DEBUG("[syscall] process '%s' (id=%u) exiting with status %d\n",
               current->name, current->id, status);

    /* CLONE_CHILD_CLEARTID: 清零 tid 并唤醒 pthread_join 等待者 */
    if (current->ctid_ptr) {
        *(volatile uint32_t *)current->ctid_ptr = 0;
        futex_do_wake(current->ctid_ptr, 0x7fffffff);
    }

    /* CLONE_VM/CLONE_FILES threads share fd objects with the process. */
    if (!current->is_thread) {
        for (int _fd = 0; _fd < (int)TASK_MAX_FD; _fd++) {
            int _idx = (int)(int8_t)current->fd_table[_fd];
            if (_idx < 0 || _idx >= FD_POOL_SIZE) {
                current->fd_table[_fd] = -1;
                continue;
            }
            fd_obj_t *_obj = &g_fd_pool[_idx];
            if (_obj->type == FDT_FILE)
                ext4_fclose(&_obj->file);
            else if (_obj->type == FDT_DIR)
                ext4_dir_close(&_obj->dir);
            fd_pool_free(_idx);
            current->fd_table[_fd] = -1;
        }
    }

    /* 在退出前计算 utime： wall_time − stime */
    if (current->is_user_process) {
        uint64_t _now = kernel_get_ns();
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

    /* 不应该到达这里 */
    while (1)
        ;
}

void exit_handler(uint64_t regs[6])
{
    sys_exit((int)regs[0]);
}

/* ───────────────────────────────────────────────────────────────
 *  sys_execve
 * ─────────────────────────────────────────────────────────────── */
int64_t sys_execve(const char *pathname, char **argv, char **envp)
{
    (void)envp;
    if (pathname == NULL)
        return -1;

    KLOG_DEBUG("[syscall] execve called\n");

    task_t *current = task_current();
    /* 记录可执行文件路径（/proc/self/exe 使用） */
    {
        int k = 0;
        while (pathname[k] && k < (int)TASK_EXE_LEN - 1) {
            current->exe_path[k] = pathname[k]; k++;
        }
        current->exe_path[k] = '\0';
    }

    /* execve 关闭所有 fd > 2 */
    KLOG_DEBUG("[execve] pid=%u closing all fds > 2\n", current->id);
    for (int fd = 3; fd < (int)TASK_MAX_FD; fd++) {
        int idx = (int)current->fd_table[fd];
        if (idx < 0 || idx >= FD_POOL_SIZE) {
            current->fd_table[fd] = (int8_t)-1;
            continue;
        }
        fd_obj_t *obj = &g_fd_pool[idx];
        if (obj->type == FDT_FREE) {
            current->fd_table[fd] = (int8_t)-1;
            continue;
        }
        KLOG_TRACE("[execve] closing fd=%d pool_idx=%d type=%d\n",
                  fd, idx, obj->type);
        if (obj->type == FDT_FILE)
            ext4_fclose(&obj->file);
        else if (obj->type == FDT_DIR)
            ext4_dir_close(&obj->dir);
        fd_pool_free(idx);
        current->fd_table[fd] = (int8_t)-1;
    }

    /* 从 userspace 复制 argv */
#define EXEC_MAX_ARGC   32
#define EXEC_MAX_ARGLEN 128
    char  arg_store[EXEC_MAX_ARGC][EXEC_MAX_ARGLEN];
    char *argv_ptrs[EXEC_MAX_ARGC + 1];
    char **kern_argv = NULL;

    if (argv) {
        char **uav = argv;
        int n = 0;
        while (n < EXEC_MAX_ARGC) {
            char *uarg = uav[n];
            if (!uarg) break;
            copy_string_from_user(uarg, arg_store[n], EXEC_MAX_ARGLEN);
            argv_ptrs[n] = arg_store[n];
            n++;
        }
        argv_ptrs[n] = NULL;
        kern_argv = argv_ptrs;
    }

    /* 将相对路径转为绝对路径 */
    char abs_path[256];
    resolve_path(current->cwd, pathname, abs_path, (int)sizeof(abs_path));

    int rc = elf_loader_load_from_file(abs_path, kern_argv, NULL);
    /* 成功则不应返回到这里 */
    return rc;
}

void execve_handler(uint64_t regs[6])
{
    const char *pathname = (const char *)regs[0];
    char **argv = (char **)regs[1];
    char **envp = (char **)regs[2];
    regs[0] = sys_execve(pathname, argv, envp);
}

/* ───────────────────────────────────────────────────────────────
 *  clone (fork + thread)
 * ─────────────────────────────────────────────────────────────── */
void clone_handler(uint64_t regs[6], task_t *parent, trap_frame_t *frame)
{
    uint64_t flags          = regs[0];
    uint64_t child_stack    = regs[1];
    uint32_t *parent_tidptr = (uint32_t *)regs[2];
    uint64_t tls            = regs[3];
    uint32_t *child_tidptr  = (uint32_t *)regs[4];

    /* 分配子任务槽 */
    task_t *child = NULL;
    {
        /* 先回收已死亡的槽 */
        for (uint32_t i = 0; i < TASK_MAX; i++) {
            if (g_stack_used[i] && g_task_pool[i].state == TASK_DEAD) {
                task_reap_dead(&g_task_pool[i]);
                break;
            }
        }
        for (uint32_t i = 0; i < TASK_MAX; i++) {
            if (!g_stack_used[i]) {
                g_stack_used[i] = 1;
                g_task_pool[i].stack_base = g_task_stacks[i];
                child = &g_task_pool[i];
                break;
            }
        }
    }

    if (!child) {
        KLOG_ERROR("[clone] no free task slots\n");
        regs[0] = (uint64_t)(int64_t)-ENOMEM;
        return;
    }

    child->id              = g_task_id_cnt++;
    child->state           = TASK_READY;
    child->priority        = parent->priority;
    child->is_user_process = true;
    child->user_started    = true;
    child->exit_status     = 0;
    child->is_waiting      = false;
    child->wait_pid        = (uint32_t)-1;
    child->ctid_ptr        = 0;
    child->is_thread       = false;
    child->utime_ns        = 0;
    child->stime_ns        = 0;
    child->sc_entry_ns     = 0;
    child->create_ns       = 0;

    if (flags & CLONE_VM) {
        /* ── 线程路径 ── */
        child->is_thread       = true;
        child->pgd             = parent->pgd;
        child->user_entry      = parent->user_entry;
        child->user_sp         = child_stack;
        child->user_stack_top  = child_stack;
        child->user_stack_size = parent->user_stack_size;
        child->heap_end        = parent->heap_end;
        child->mmap_next       = parent->mmap_next;
        child->fs_base         = (flags & CLONE_SETTLS) ? tls : parent->fs_base;
        child->parent_id       = parent->id;

        if ((flags & CLONE_PARENT_SETTID) && parent_tidptr)
            *parent_tidptr = child->id;
        if ((flags & CLONE_CHILD_CLEARTID) && child_tidptr)
            child->ctid_ptr = (uint64_t)child_tidptr;

        {
            int k = 0;
            while (parent->cwd[k] && k < (int)TASK_CWD_LEN - 1) {
                child->cwd[k] = parent->cwd[k]; k++;
            }
            child->cwd[k] = '\0';
        }
        for (uint32_t k = 0; k < TASK_MAX_FD; k++)
            child->fd_table[k] = parent->fd_table[k];
        {
            int k = 0;
            while (parent->name[k] && k < (int)TASK_NAME_LEN - 1) {
                child->name[k] = parent->name[k]; k++;
            }
            child->name[k] = '\0';
        }

        list_node_init(&child->run_node);
        list_node_init(&child->wait_node);

        child->sp = arch_init_fork_child_stack(
            child->stack_base, TASK_STACK_SIZE,
            frame, child_stack,
            (flags & CLONE_SETTLS) ? tls : 0
        );

        KLOG_DEBUG("[clone/thread] parent=%u child=%u flags=0x%llx tls=0x%llx usp=0x%llx ctid=%p\n",
               parent->id, child->id, flags, tls, child_stack, child_tidptr);

    } else {
        /* ── fork 路径 ── */
        uint64_t child_pgd_phys = pmm_alloc_pages(g_pmm, 1);
        if (child_pgd_phys == 0) {
            KLOG_ERROR("[clone] no memory for child pgd\n");
            g_stack_used[child - g_task_pool] = 0;
            regs[0] = (uint64_t)(int64_t)-ENOMEM;
            return;
        }
        void *child_pgd_virt  = phys_to_virt(child_pgd_phys);
        void *parent_pgd_virt = phys_to_virt((uint64_t)parent->pgd);
        memset(child_pgd_virt, 0, PAGE_SIZE);

#if ARCH_X86_64
        x86_copy_kernel_mappings((uint64_t *)child_pgd_virt,
                                 (uint64_t *)parent_pgd_virt);
#endif
#if ARCH_RISCV64
        {
            uint64_t *child_l1  = (uint64_t *)child_pgd_virt;
            uint64_t *kernel_l1 = (uint64_t *)phys_to_virt(satp_read_pgd_phys());
            riscv64_copy_kernel_mappings(child_l1, kernel_l1);
        }
#endif

        bool clone_copy_ok = true;

        #define CLONE_COPY_RANGE(start, end)                                              \
            do {                                                                           \
                uint64_t __s = ALIGN_DOWN((start), PAGE_SIZE);                            \
                uint64_t __e = ALIGN_UP((end), PAGE_SIZE);                                \
                for (uint64_t va = __s; clone_copy_ok && va < __e; va += PAGE_SIZE) {     \
                    uint64_t src_pa = mm_vm_get_paddr(parent_pgd_virt, va);               \
                    if (src_pa == 0)                                                       \
                        continue;                                                          \
                    uint64_t dst_pa = pmm_alloc_pages(g_pmm, 1);                          \
                    if (dst_pa == 0) {                                                     \
                        clone_copy_ok = false;                                             \
                        break;                                                             \
                    }                                                                      \
                    memcpy(phys_to_virt(dst_pa), phys_to_virt(src_pa), PAGE_SIZE);        \
                    if (mm_vm_map_pages(child_pgd_virt, va, dst_pa, 1, 0) != 0) {         \
                        pmm_free_pages(g_pmm, dst_pa, 1);                                  \
                        clone_copy_ok = false;                                             \
                        break;                                                             \
                    }                                                                      \
                }                                                                          \
            } while (0)

        CLONE_COPY_RANGE(0x0, parent->heap_end);
        if (clone_copy_ok && parent->mmap_next > USER_MMAP_BASE_EXEC)
            CLONE_COPY_RANGE(USER_MMAP_BASE_EXEC, parent->mmap_next);
        if (clone_copy_ok)
            CLONE_COPY_RANGE(parent->user_stack_top - parent->user_stack_size,
                             parent->user_stack_top);
        #undef CLONE_COPY_RANGE

        if (!clone_copy_ok) {
            KLOG_ERROR("[clone] failed to copy user address space\n");
            g_stack_used[child - g_task_pool] = 0;
            pmm_free_pages(g_pmm, child_pgd_phys, 1);
            regs[0] = (uint64_t)(int64_t)-ENOMEM;
            return;
        }

        child->pgd             = (uint64_t *)child_pgd_phys;
        child->user_entry      = parent->user_entry;
        child->user_sp         = parent->user_sp;
        child->user_stack_top  = parent->user_stack_top;
        child->user_stack_size = parent->user_stack_size;
        child->heap_end        = parent->heap_end;
        child->mmap_next       = parent->mmap_next;
        child->fs_base         = parent->fs_base;
        child->parent_id       = parent->id;

        {
            int k = 0;
            while (parent->cwd[k] && k < (int)TASK_CWD_LEN - 1) {
                child->cwd[k] = parent->cwd[k]; k++;
            }
            child->cwd[k] = '\0';
        }
        /* 深拷贝 fd_table */
        for (uint32_t k = 0; k < TASK_MAX_FD; k++) {
            int pidx = (int)(int8_t)parent->fd_table[k];
            if (pidx < 0 || pidx >= FD_POOL_SIZE) {
                child->fd_table[k] = -1;
                continue;
            }
            fd_obj_t *src = &g_fd_pool[pidx];
            if (src->type == FDT_FREE) {
                child->fd_table[k] = -1;
                continue;
            }
            int new_idx = fd_pool_alloc();
            if (new_idx < 0) {
                KLOG_ERROR("[clone/fork] fd pool full, fd=%u dropped\n", k);
                child->fd_table[k] = -1;
                continue;
            }
            g_fd_pool[new_idx] = *src;
            child->fd_table[k] = (int8_t)new_idx;
        }
        {
            int k = 0;
            while (parent->name[k] && k < (int)TASK_NAME_LEN - 1) {
                child->name[k] = parent->name[k]; k++;
            }
            child->name[k] = '\0';
        }

        list_node_init(&child->run_node);
        list_node_init(&child->wait_node);

        child->sp = arch_init_fork_child_stack(
            child->stack_base, TASK_STACK_SIZE,
            frame, 0, 0
        );

        KLOG_DEBUG("[clone/fork] parent=%u child=%u elr=0x%llx usp=0x%llx\n",
               parent->id, child->id, syscall_abi_ip(frame), syscall_abi_user_sp(frame));
    }

    /* 信号继承 */
    child->pending_sigs      = 0;
    child->sig_frame_sp      = 0;
    child->sig_saved_blocked = 0;
    child->blocked_sigs      = parent->blocked_sigs;
    child->pgid              = parent->pgid;
    for (int _si = 0; _si < NSIG; _si++)
        child->sig_actions[_si] = parent->sig_actions[_si];

    child->create_ns = kernel_get_ns();
    sched_enqueue(child);

    /* 父进程返回子进程 PID */
    regs[0] = (uint64_t)child->id;
}

/* ───────────────────────────────────────────────────────────────
 *  wait4 / waitid
 * ─────────────────────────────────────────────────────────────── */
void wait_handler(uint64_t regs[6], task_t *me)
{
    int wait_pid = (int)(int32_t)regs[0];
    int *wstatus = (int *)regs[1];
    int options  = (int)regs[2];

    const int WNOHANG = 1;

    /* 先判断是否存在匹配的子进程 */
    bool has_matching_child = false;
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i]) continue;
        task_t *t = &g_task_pool[i];
        if (t->parent_id != me->id) continue;
        if (wait_pid > 0 && (int)t->id != wait_pid) continue;
        has_matching_child = true;
        break;
    }

    if (!has_matching_child) {
        regs[0] = (uint64_t)(int64_t)-ECHILD;
        return;
    }

    /* 查找已退出的子进程 */
    task_t *found = NULL;
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i]) continue;
        task_t *t = &g_task_pool[i];
        if (t->parent_id != me->id) continue;
        if (wait_pid > 0 && (int)t->id != wait_pid) continue;
        if (t->state == TASK_DEAD) { found = t; break; }
    }

    if (found) {
        if (wstatus)
            *wstatus = (found->exit_status & 0xFF) << 8;
        if (regs[3]) {
            memset((void *)regs[3], 0, 144);
            uint64_t *ru = (uint64_t *)regs[3];
            ru[0] = found->utime_ns / 1000000000ULL;
            ru[1] = (found->utime_ns % 1000000000ULL) / 1000ULL;
            ru[2] = found->stime_ns / 1000000000ULL;
            ru[3] = (found->stime_ns % 1000000000ULL) / 1000ULL;
        }
        regs[0] = (uint64_t)found->id;
        task_reap_dead(found);
        return;
    }

    /* 无已退出的子进程 */
    if (options & WNOHANG) {
        regs[0] = 0;
        return;
    }

    /* 阻塞等待 */
    me->is_waiting = true;
    me->wait_pid   = (wait_pid > 0) ? (uint32_t)wait_pid : (uint32_t)-1;
    task_block(NULL);

    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i]) continue;
        task_t *t = &g_task_pool[i];
        if (t->parent_id != me->id) continue;
        if (wait_pid > 0 && (int)t->id != wait_pid) continue;
        if (t->state == TASK_DEAD) { found = t; break; }
    }
    if (found) {
        if (wstatus)
            *wstatus = (found->exit_status & 0xFF) << 8;
        if (regs[3]) {
            memset((void *)regs[3], 0, 144);
            uint64_t *ru = (uint64_t *)regs[3];
            ru[0] = found->utime_ns / 1000000000ULL;
            ru[1] = (found->utime_ns % 1000000000ULL) / 1000ULL;
            ru[2] = found->stime_ns / 1000000000ULL;
            ru[3] = (found->stime_ns % 1000000000ULL) / 1000ULL;
        }
        regs[0] = (uint64_t)found->id;
        task_reap_dead(found);
    } else {
        regs[0] = (uint64_t)(int64_t)-ECHILD;
    }
}
