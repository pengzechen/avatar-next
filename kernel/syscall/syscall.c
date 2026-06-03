#include "kernel_stat.h"
#include "pseudofs.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "syscall/core/futex.h"
#include "loader/bin_loader.h"
#include "loader/elf_loader.h"
#include "klog.h"
#include "task/task.h"
#include "user_layout.h"
#include "task/sched.h"
#include "task/switch.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "arch.h"
#include "exception.h"
#include "syscall_abi.h"
#include "uart/uart.h"
#include <ext4.h>
#include <ext4_errno.h>
#if ARCH_RISCV64
#include "riscv64/satp_utils.h"
#endif

#include "syscall/fs/fd_pool.h"

/* ─────────────────────────────────────────────────────────────────
 * Unblock parent waiting for a specific child (or any child)
 * ───────────────────────────────────────────────────────────────── */
/* g_task_pool / g_stack_used defined as non-static in task.c */
extern task_t  g_task_pool[TASK_MAX];
extern uint8_t g_stack_used[TASK_MAX];

static void notify_parent_wait(task_t *child)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i]) continue;
        task_t *t = &g_task_pool[i];
        if (!t->is_waiting) continue;
        if (t->id != child->parent_id) continue;
        if (t->wait_pid != (uint32_t)-1 && t->wait_pid != child->id) continue;
        t->is_waiting = false;
        task_unblock(t);
        return;
    }
}

/* Called from task.c's task_exit() to notify waiting parent */
void notify_parent_wait_from_task(task_t *child)
{
    notify_parent_wait(child);
}

/* syscall entry counter（brk.c 等的调试日志会引用） */
volatile uint32_t g_syscall_entry_count = 0;

#if ARCH_X86_64
/* ─────────────────────────────────────────────────────────────────
 * Linux x86_64 → 内部统一系统调用号翻译
 *
 * busybox-x86_64 使用 Linux x86_64 syscall 号，而下面的 switch 分支
 * 使用 AArch64/RISC-V 的通用 Linux syscall 号（两者已在 AARCH64/RISC-V
 * 上共享）。这个翻译层把 x86_64 号映射到对应分支，同时对参数布局不同
 * 的 syscall（open/stat/access/readlink 等）原地调整 regs[]。
 * ───────────────────────────────────────────────────────────────── */
static void x86_translate_syscall(uint64_t *nr, uint64_t regs[9])
{
    switch (*nr) {
    /* ── 基础 I/O ── */
    case 0:   *nr = LINUX_SYS_READ;  break;   /* read */
    case 1:   *nr = LINUX_SYS_WRITE; break;   /* write */
    case 2:   /* open(path,flags,mode) → openat(AT_FDCWD,path,flags,mode) */
        regs[3] = regs[2]; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_OPENAT; break;
    case 3:   *nr = LINUX_SYS_CLOSE; break;   /* close */
    case 4:   /* stat(path,buf) → newfstatat(AT_FDCWD,path,buf,0) */
        regs[3] = 0; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_NEWFSTATAT; break;
    case 5:   *nr = LINUX_SYS_FSTAT; break;   /* fstat */
    case 6:   /* lstat(path,buf) → newfstatat(AT_FDCWD,path,buf,0) */
        regs[3] = 0; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_NEWFSTATAT; break;
    case 7:   *nr = X86_SYS_POLL;          break; /* poll */
    case 8:   *nr = LINUX_SYS_LSEEK;       break; /* lseek */
    case 9:   *nr = LINUX_SYS_MMAP;        break; /* mmap */
    case 10:  *nr = LINUX_SYS_MPROTECT;    break; /* mprotect */
    case 11:  *nr = LINUX_SYS_MUNMAP;      break; /* munmap */
    case 12:  *nr = LINUX_SYS_BRK;         break; /* brk */
    case 13:  *nr = LINUX_SYS_RT_SIGACTION;  break; /* rt_sigaction */
    case 14:  *nr = LINUX_SYS_RT_SIGPROCMASK; break; /* rt_sigprocmask */
    case 15:  *nr = LINUX_SYS_RT_SIGRETURN;  break; /* rt_sigreturn */
    case 16:  *nr = LINUX_SYS_IOCTL;       break; /* ioctl */
    case 17:  *nr = LINUX_SYS_READ;        break; /* pread64 → read(stub) */
    case 20:  *nr = LINUX_SYS_WRITEV;      break; /* writev */
    case 21:  /* access(path,mode) → faccessat(AT_FDCWD,path,mode,0) */
        regs[3] = 0; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_FACCESSAT; break;
    case 22:  *nr = LINUX_SYS_PIPE2;       break; /* pipe → pipe2(flags=0) */
    case 23:  *nr = X86_SYS_SELECT;        break; /* select(nfds,r,w,e,timeval*) */
    case 24:  *nr = LINUX_SYS_SCHED_YIELD; break; /* sched_yield */
    case 32:  /* dup(oldfd): x86_64. 不直接支持，返回 ENOSYS */
        /* busybox sh 很少用裸 dup()，ENOSYS 不影响启动 */
        break; /* *nr stays 32, hits default: → ENOSYS */
    case 33:  /* dup2(oldfd,newfd) → dup3(oldfd,newfd,0) */
        regs[2] = 0;
        *nr = LINUX_SYS_DUP3; break;
    case 35:  *nr = LINUX_SYS_NANOSLEEP;   break; /* nanosleep */
    case 39:  *nr = LINUX_SYS_GETPID;      break; /* getpid */
    case 40:  *nr = LINUX_SYS_SENDFILE;    break; /* sendfile / sendfile64 (x86_64) */
    case 187: *nr = LINUX_SYS_SENDFILE;    break; /* sendfile64 (x86_64 compat) */
    case 41:  *nr = LINUX_SYS_SOCKET;      break; /* socket */
    case 56:  /* clone: x86_64 ABI 顺序 flags,stack,ptid,ctid,tls
                        内部 ABI 顺序 flags,stack,ptid,tls,ctid
                        需要交换 arg3↔arg4 */
        { uint64_t tmp = regs[3]; regs[3] = regs[4]; regs[4] = tmp; }
        *nr = LINUX_SYS_CLONE; break;
    case 57:  /* fork(): musl 直接 syscall，不传参数 → rdi/rsi/... 是垃圾。
                        必须显式构造 flags=SIGCHLD (17)，其余清零。 */
        regs[0] = 17; /* SIGCHLD */
        regs[1] = 0; regs[2] = 0; regs[3] = 0; regs[4] = 0; regs[5] = 0;
        *nr = LINUX_SYS_CLONE; break;
    case 58:  /* vfork(): 同理，构造 CLONE_VM|CLONE_VFORK|SIGCHLD。
                        但当前内核把 CLONE_VM 当线程走，会出错。
                        为兼容 busybox sh 的 fork+exec，这里退化为普通 fork。 */
        regs[0] = 17; /* SIGCHLD: 走 fork 路径（拷贝地址空间） */
        regs[1] = 0; regs[2] = 0; regs[3] = 0; regs[4] = 0; regs[5] = 0;
        *nr = LINUX_SYS_CLONE; break;
    case 59:  *nr = LINUX_SYS_EXECVE;      break; /* execve */
    case 60:  *nr = LINUX_SYS_EXIT;        break; /* exit */
    case 61:  *nr = LINUX_SYS_WAIT4;       break; /* wait4 */
    case 62:  *nr = LINUX_SYS_KILL;        break; /* kill */
    case 63:  *nr = LINUX_SYS_UNAME;       break; /* uname */
    case 72:  *nr = LINUX_SYS_FCNTL;       break; /* fcntl */
    case 74:  *nr = LINUX_SYS_FSYNC;       break; /* fsync */
    case 75:  *nr = LINUX_SYS_FDATASYNC;   break; /* fdatasync */
    case 79:  *nr = LINUX_SYS_GETCWD;      break; /* getcwd */
    case 80:  *nr = LINUX_SYS_CHDIR;       break; /* chdir */
    case 82:  *nr = 0x7FFFFFFEULL; break;      /* fchmod → stub 0 */
    case 83:  *nr = 0x7FFFFFFEULL; break;      /* fchown → stub 0 */
    case 89:  /* readlink(path,buf,bufsiz) → readlinkat(AT_FDCWD,path,buf,bufsiz) */
        regs[3] = regs[2]; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_READLINKAT; break;
    case 95:  *nr = LINUX_SYS_UMASK;       break; /* umask */
    case 97:  *nr = LINUX_SYS_GETRLIMIT;   break; /* getrlimit */
    case 98:  *nr = LINUX_SYS_GETRUSAGE;   break; /* getrusage */
    case 99:  *nr = 0x7FFFFFFEULL; break;      /* sysinfo → stub 0 */
    case 100: *nr = 0x7FFFFFFEULL; break;      /* times → stub 0 */
    case 102: *nr = LINUX_SYS_GETUID;      break; /* getuid */
    case 104: *nr = LINUX_SYS_GETGID;      break; /* getgid */
    case 105: *nr = LINUX_SYS_SETUID;      break; /* setuid */
    case 106: *nr = LINUX_SYS_SETGID;      break; /* setgid */
    case 107: *nr = LINUX_SYS_GETEUID;     break; /* geteuid */
    case 108: *nr = LINUX_SYS_GETEGID;     break; /* getegid */
    case 109: *nr = LINUX_SYS_SETPGID;     break; /* setpgid */
    case 110: *nr = LINUX_SYS_GETPPID;     break; /* getppid */
    case 111: /* getpgrp() → getpgid(0) */
        regs[0] = 0; *nr = LINUX_SYS_GETPGID; break;
    case 112: *nr = LINUX_SYS_SETSID;      break; /* setsid */
    case 115: *nr = LINUX_SYS_GETGROUPS;   break; /* getgroups */
    case 116: *nr = LINUX_SYS_SETGROUPS;   break; /* setgroups */
    case 121: *nr = LINUX_SYS_GETPGID;     break; /* getpgid */
    case 124: *nr = LINUX_SYS_GETSID;      break; /* getsid */
    case 131: *nr = 0x7FFFFFFEULL;         break; /* sigaltstack → stub 0 */
    case 157: *nr = LINUX_SYS_PRCTL;       break; /* prctl */
    case 158: *nr = X86_SYS_ARCH_PRCTL;    break; /* arch_prctl */
    case 160: *nr = LINUX_SYS_SETRLIMIT;   break; /* setrlimit */
    case 165: *nr = LINUX_SYS_GETRUSAGE;   break; /* getrusage(again) */
    case 186: *nr = LINUX_SYS_GETTID;      break; /* gettid */
    case 202: *nr = LINUX_SYS_FUTEX;        break; /* futex */
    case 217: *nr = LINUX_SYS_GETDENTS64;  break; /* getdents64 */
    case 218: *nr = LINUX_SYS_SET_TID_ADDR; break; /* set_tid_address */
    case 228: *nr = LINUX_SYS_CLOCK_GETTIME; break; /* clock_gettime */
    case 231: *nr = LINUX_SYS_EXIT_GROUP;  break; /* exit_group */
    case 234: *nr = LINUX_SYS_TGKILL;      break; /* tgkill */
    case 247: *nr = LINUX_SYS_WAITID;      break; /* waitid */
    case 257: *nr = LINUX_SYS_OPENAT;      break; /* openat */
    case 258: *nr = 0x7FFFFFFEULL; break;      /* mkdirat → stub 0 */
    case 262: *nr = LINUX_SYS_NEWFSTATAT;  break; /* newfstatat */
    case 263: *nr = LINUX_SYS_UNLINKAT;    break; /* unlinkat */
    case 264: *nr = LINUX_SYS_RENAMEAT;    break; /* renameat */
    case 267: *nr = LINUX_SYS_READLINKAT;  break; /* readlinkat */
    case 268: *nr = 0x7FFFFFFEULL; break;      /* fchmodat → stub 0 */
    case 269: *nr = LINUX_SYS_FACCESSAT;   break; /* faccessat */
    case 270: *nr = LINUX_SYS_PSELECT6;    break; /* pselect6 */
    case 271: *nr = LINUX_SYS_PPOLL;       break; /* ppoll */
    case 273: *nr = LINUX_SYS_SET_ROBUST_LIST; break; /* set_robust_list */
    case 292: *nr = LINUX_SYS_DUP3;        break; /* dup3 */
    case 293: *nr = LINUX_SYS_PIPE2;       break; /* pipe2 */
    case 302: *nr = LINUX_SYS_PRLIMIT64;   break; /* prlimit64 */
    case 318: *nr = LINUX_SYS_GETRANDOM;   break; /* getrandom */
    case 334: *nr = X86_SYS_RSEQ;          break; /* rseq */
    /* 其他：保持原号（会落到 default: 返回 ENOSYS） */
    default: break;
    }
}
#endif /* ARCH_X86_64 */

/* ─────────────────────────────────────────────────────────────────
 * Main syscall dispatcher
 * ───────────────────────────────────────────────────────────────── */
void syscall_handler(trap_frame_t *frame)
{
    g_syscall_entry_count++;
    uint64_t syscall_num = syscall_abi_nr(frame);
    uint64_t raw_syscall_num = syscall_num;
    uint64_t regs[9] = {0};
    for (int i = 0; i < 6; i++) {
        regs[i] = syscall_abi_arg(frame, i);
    }

#if ARCH_X86_64
    /* 将 Linux x86_64 syscall 号翻译为下方 switch 使用的内部编号 */
    x86_translate_syscall(&syscall_num, regs);
    /* 0x7FFFFFFEULL 是 stub 标记：直接返回 0 */
    if (syscall_num == 0x7FFFFFFEULL) {
        if (g_syscall_entry_count <= 16) {
            KLOG_DEBUG("[syscall:x86] raw=%llu -> stub0 args=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]\n",
                       raw_syscall_num, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]);
        }
        syscall_abi_set_ret(frame, 0);
        return;
    }
#endif

    task_t *current = task_current();

    /* 每次 syscall 入口 poll UART：弥补关中断期间 timer 无法触发的窗口 */
    signal_check_uart();

    /* 开始 syscall stime 计时 */
    if (current && current->is_user_process)
        current->sc_entry_ns = kernel_get_ns();

    switch (syscall_num) {

    /* ── 自定义系统调用（向后兼容）────────────────────────── */
    case SYS_EXIT:
        sys_exit((int)regs[0]);
        break;
    case SYS_YIELD:
        regs[0] = sys_yield();
        break;
    case SYS_GETPID:
        regs[0] = sys_getpid();
        break;
    case SYS_SLEEP:
        regs[0] = sys_sleep(regs[0]);
        break;
    case SYS_EXECVE: {
        const char *pathname = (const char *)regs[0];
        char **argv = (char **)regs[1];
        char **envp = (char **)regs[2];
        regs[0] = sys_execve(pathname, argv, envp);
        break;
    }
    case SYS_BRK: {
        void *addr = (void *)regs[0];
        regs[0] = (uint64_t)sys_brk(addr);
        break;
    }
    case SYS_SBRK: {
        int64_t inc = (int64_t)regs[0];
        regs[0] = (uint64_t)sys_sbrk(inc);
        break;
    }
    case SYS_WRITE: {
        regs[0] = sys_write((const char *)regs[0], regs[1]);
        break;
    }
    case SYS_READ: {
        regs[0] = sys_read((char *)regs[0], regs[1]);
        break;
    }
    case SYS_OPEN: {
        regs[0] = sys_open((const char *)regs[0], (int)regs[1], (int)regs[2]);
        break;
    }
    case SYS_CLOSE: {
        regs[0] = sys_close((int)regs[0]);
        break;
    }
    case SYS_GETTIMEOFDAY: {
        regs[0] = sys_gettimeofday((struct timeval *)regs[0], (void *)regs[1]);
        break;
    }

    /* ── Linux AArch64 标准系统调用 ─────────────────────────────── */

    /* --- 进程管理 --- */
    case LINUX_SYS_EXIT:
    case LINUX_SYS_EXIT_GROUP:
        exit_handler(regs);
        break;

    case LINUX_SYS_CLONE:
        clone_handler(regs, task_current(), frame);
        break;

    case LINUX_SYS_EXECVE:
        execve_handler(regs);
        break;

    case LINUX_SYS_WAIT4:
    case LINUX_SYS_WAITID:
        wait_handler(regs, task_current());
        break;

    case LINUX_SYS_SET_TID_ADDR:
    case LINUX_SYS_GETTID:
    case LINUX_SYS_GETPID:
    case LINUX_SYS_GETPPID:
    case LINUX_SYS_GETUID:
    case LINUX_SYS_GETEUID:
    case LINUX_SYS_GETGID:
    case LINUX_SYS_GETEGID:
    case LINUX_SYS_SETUID:
    case LINUX_SYS_SETGID:
    case LINUX_SYS_SETPGID:
    case LINUX_SYS_GETPGID:
    case LINUX_SYS_GETSID:
    case LINUX_SYS_SETSID:
    case LINUX_SYS_GETGROUPS:
    case LINUX_SYS_SETGROUPS:
        proc_ids_handler(syscall_num, regs, current);
        break;

    case LINUX_SYS_FUTEX:
        futex_handler(regs);
        break;

    case X86_SYS_ARCH_PRCTL: {
#if ARCH_X86_64
        arch_prctl_handler(regs, current);
#else
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
#endif
        break;
    }

    case X86_SYS_RSEQ:
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
        break;

    case LINUX_SYS_KILL:
        kill_handler(regs, current);
        break;

    case LINUX_SYS_TKILL:
        tkill_handler(regs);
        break;

    case LINUX_SYS_TGKILL:
        tgkill_handler(regs);
        break;

    case LINUX_SYS_SCHED_YIELD:
        sched_yield_handler(regs);
        break;

    /* --- 文件描述符 --- */
    case LINUX_SYS_READ:
        read_handler(regs, current);
        break;

    case LINUX_SYS_READV:
        readv_handler(regs, current);
        break;

    case LINUX_SYS_WRITE:
        write_handler(regs, current);
        break;

    case LINUX_SYS_WRITEV:
        writev_handler(regs, current);
        break;

    case LINUX_SYS_LSEEK:
        lseek_handler(regs, current);
        break;

    case LINUX_SYS_FACCESSAT:
        faccessat_handler(regs, current);
        break;

    case LINUX_SYS_RENAMEAT:
        renameat_handler(regs, current);
        break;

    case LINUX_SYS_UNLINKAT:
        unlinkat_handler(regs, current);
        break;

    case LINUX_SYS_OPENAT:
        openat_handler(regs, current);
        break;

    case LINUX_SYS_CLOSE:
        close_handler(regs, current);
        break;

    case LINUX_SYS_DUP3:
        dup3_handler(regs, current);
        break;

    case LINUX_SYS_FCNTL:
        fcntl_handler(regs);
        break;

    case LINUX_SYS_PIPE2:
        pipe2_handler(regs);
        break;

    case LINUX_SYS_GETDENTS64:
        getdents64_handler(regs, current);
        break;

    case LINUX_SYS_FSTAT:
        fstat_handler(regs, current);
        break;

    case LINUX_SYS_NEWFSTATAT:
        newfstatat_handler(regs, current);
        break;

    case LINUX_SYS_READLINKAT:
        readlinkat_handler(regs, current);
        break;

    case LINUX_SYS_FSYNC:
    case LINUX_SYS_FDATASYNC:
        regs[0] = 0;
        break;

    case LINUX_SYS_SENDFILE:
        sendfile_handler(regs, current);
        break;

    /* --- 目录操作 --- */
    case LINUX_SYS_GETCWD:
        getcwd_handler(regs, current);
        break;

    case LINUX_SYS_CHDIR:
        chdir_handler(regs, current);
        break;

    /* --- ioctl --- */
    case LINUX_SYS_IOCTL:
        ioctl_handler(regs, current);
        break;

    case X86_SYS_POLL:
    case LINUX_SYS_PPOLL:
        poll_handler(regs, syscall_num, current);
        break;

    case X86_SYS_SELECT:
    case LINUX_SYS_PSELECT6:
        select_handler(regs, syscall_num, current);
        break;

    /* --- 信号系统（实现位于 kernel/syscall/signal.c）--- */
    case LINUX_SYS_RT_SIGACTION:
        sigaction_handler(regs, current);
        break;

    case LINUX_SYS_RT_SIGPROCMASK:
        sigprocmask_handler(regs, current);
        break;

    case LINUX_SYS_RT_SIGRETURN:
        sigreturn_handler(regs, current, frame);
        break;

    /* --- 系统信息 --- */
    case LINUX_SYS_UNAME: {
        struct kernel_utsname *u = (struct kernel_utsname *)regs[0];
        if (!u) { regs[0] = (uint64_t)(int64_t)-EFAULT; break; }
        copy_string_to_user("Linux",      u->sysname,    sizeof(u->sysname));
        copy_string_to_user("avatar",     u->nodename,   sizeof(u->nodename));
        copy_string_to_user("5.15.0",     u->release,    sizeof(u->release));
        copy_string_to_user("#1 SMP",     u->version,    sizeof(u->version));
#if ARCH_RISCV64
        copy_string_to_user("riscv64",    u->machine,    sizeof(u->machine));
#elif ARCH_AARCH64
        copy_string_to_user("aarch64",    u->machine,    sizeof(u->machine));
#else
        copy_string_to_user("x86_64",     u->machine,    sizeof(u->machine));
#endif
        copy_string_to_user("",           u->domainname, sizeof(u->domainname));
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_CLOCK_GETTIME: {
        struct kernel_timespec *ts = (struct kernel_timespec *)regs[1];
        if (ts) {
            uint64_t ns  = kernel_get_ns();
            ts->tv_sec   = (int64_t)(ns / 1000000000ULL);
            ts->tv_nsec  = (int64_t)(ns % 1000000000ULL);
        }
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_NANOSLEEP:
        nanosleep_handler(regs);
        break;

    case LINUX_SYS_GETRLIMIT:
    case LINUX_SYS_SETRLIMIT:
    case LINUX_SYS_PRLIMIT64: {
        /* Return unlimited for most resources */
        if (syscall_num == LINUX_SYS_GETRLIMIT || syscall_num == LINUX_SYS_PRLIMIT64) {
            /*
             * getrlimit(resource, rlim):                 rlim 在 arg1
             * prlimit64(pid, resource, new, old):        old  在 arg3
             */
            struct kernel_rlimit *rl = (struct kernel_rlimit *)regs[
                (syscall_num == LINUX_SYS_PRLIMIT64) ? 3 : 1];
            if (rl) {
                rl->rlim_cur = (uint64_t)-1;
                rl->rlim_max = (uint64_t)-1;
            }
        }
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_GETRUSAGE:
        if (regs[1]) {
            memset((void *)regs[1], 0, 144);
            if (current && current->is_user_process) {
                uint64_t *ru = (uint64_t *)regs[1];
                ru[0] = current->utime_ns / 1000000000ULL;
                ru[1] = (current->utime_ns % 1000000000ULL) / 1000ULL;
                ru[2] = current->stime_ns / 1000000000ULL;
                ru[3] = (current->stime_ns % 1000000000ULL) / 1000ULL;
            }
        }
        regs[0] = 0;
        break;

    case LINUX_SYS_UMASK:
        regs[0] = 0022;
        break;

    case LINUX_SYS_PRCTL:
    case LINUX_SYS_SET_ROBUST_LIST:
    case LINUX_SYS_MPROTECT:
        regs[0] = 0;
        break;

    case LINUX_SYS_MUNMAP:
        regs[0] = sys_munmap(regs[0], regs[1]);
        break;

    case LINUX_SYS_SOCKET:
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
        break;

    case LINUX_SYS_GETRANDOM:
        getrandom_handler(regs);
        break;

    case LINUX_SYS_BRK:
        regs[0] = (uint64_t)sys_brk((void *)regs[0]);
        break;

    case LINUX_SYS_MMAP: {
        regs[0] = sys_mmap(regs[0], regs[1], (int)regs[2], (int)regs[3],
                           (int)regs[4], regs[5]);
        break;
    }

    /* membarrier (283 on riscv64/aarch64, 324 on x86_64):
     * musl calls MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED at pthread_create.
     * We run on a simple uniprocessor kernel with coherent shared memory,
     * so just acknowledge without doing anything. */
    case 283:   /* riscv64 / aarch64 __NR_membarrier */
    case 324:   /* x86_64  __NR_membarrier */
        regs[0] = 0;
        break;

    default:
        KLOG_ERROR("[syscall] Unknown syscall: %llu\n", syscall_num);
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
        break;
    }

    /* 结束 syscall stime 计时（EXIT 类 syscall 不会到达这里） */
    if (current && current->is_user_process && current->sc_entry_ns != 0) {
        current->stime_ns += kernel_get_ns() - current->sc_entry_ns;
        current->sc_entry_ns = 0;
    }

    syscall_abi_set_ret(frame, regs[0]);

    /* 在返回用户态前投递 pending 信号
     * 注意：必须在 syscall_abi_set_ret 之后调用，
     * 这样 sigframe 里保存的帧已含有正确的 syscall 返回值。 */
    if (current && current->is_user_process)
        deliver_pending_signals(current, frame);
}


/* ── 具体系统调用实现 ──────────────────────────────────────────── */

int64_t sys_write(const char *str, uint64_t len)
{
    if (str == NULL) {
        return -1;
    }

    /* 简单实现：直接输出到 UART（未验证用户指针，后续改进） */
    for (uint64_t i = 0; i < len; i++) {
        klog_putchar(str[i]);
    }

    return (int64_t)len;
}

int64_t sys_yield(void)
{
    task_yield();
    return 0;
}

int64_t sys_getpid(void)
{
    task_t *current = task_current();
    return (int64_t)current->id;
}

int64_t sys_sleep(uint64_t ms)
{
    /* 简单忙等待实现：TODO 改为基于定时器的睡眠并让出 CPU */
    if (ms == 0)
        return 0;

    /* 假设 CPU ~1-2 GHz，每次循环约几个纳秒 */
    volatile uint64_t count = ms * 100000;
    while (count--) {
        __asm__ volatile("nop");
    }

    return 0;
}

int64_t sys_read(char *buf, uint64_t len)
{
    if (buf == NULL || len == 0) {
        return -1;
    }
    return 0;
}

int64_t sys_open(const char *pathname, int flags, int mode)
{
    (void)flags;
    (void)mode;
    KLOG_WARN("[syscall] open('%s') not implemented\n", pathname);
    return -1;
}

int64_t sys_close(int fd)
{
    KLOG_WARN("[syscall] close(%d) not implemented\n", fd);
    return -1;
}

int64_t sys_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (!tv) return -1;
    uint64_t ns   = kernel_get_ns();
    tv->tv_sec    = (long)(ns / 1000000000ULL);
    tv->tv_usec   = (long)((ns % 1000000000ULL) / 1000ULL);
    return 0;
}
