/*
 * core/proc_ids.c - 进程身份 / 进程组 / 会话相关 syscall
 *
 * 从 kernel/syscall/syscall.c 抽出。覆盖：
 *   getpid / getppid / gettid / set_tid_address
 *   getuid / geteuid / getgid / getegid / setuid / setgid
 *   getpgid / setpgid / getsid / setsid
 *   getgroups / setgroups
 *
 * 当前模型：单用户（uid/gid = 0），sid 简化为 pgid。
 */
#include "syscall/syscall_internal.h"
#include "task/task.h"

void proc_ids_handler(uint64_t syscall_num, uint64_t regs[6], task_t *current)
{
    switch (syscall_num) {
    case LINUX_SYS_SET_TID_ADDR:
        /* musl 线程初始化时设置线程 'clear-child-tid' 地址 */
        current->ctid_ptr = regs[0];
        /* fall through */
    case LINUX_SYS_GETTID:
    case LINUX_SYS_GETPID:
        regs[0] = (uint64_t)current->id;
        break;

    case LINUX_SYS_GETPPID:
        regs[0] = (uint64_t)current->parent_id;
        break;

    case LINUX_SYS_GETUID:
    case LINUX_SYS_GETEUID:
    case LINUX_SYS_GETGID:
    case LINUX_SYS_GETEGID:
        regs[0] = 0;  /* root */
        break;

    case LINUX_SYS_SETUID:
    case LINUX_SYS_SETGID:
        regs[0] = 0;
        break;

    case LINUX_SYS_SETPGID: {
        int pid  = (int)(int32_t)regs[0];
        int pgid = (int)(int32_t)regs[1];
        task_t *tgt = (pid == 0) ? current : task_find_by_id((uint32_t)pid);
        if (!tgt) { regs[0] = (uint64_t)(int64_t)-ESRCH; break; }
        if (pgid != 0)
            tgt->pgid = (uint32_t)pgid;
        else if (pid != 0)
            tgt->pgid = (uint32_t)pid;
        else
            tgt->pgid = current->id;
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_GETPGID: {
        int pid = (int)(int32_t)regs[0];
        task_t *tgt = (pid == 0) ? current : task_find_by_id((uint32_t)pid);
        regs[0] = tgt ? (uint64_t)tgt->pgid : (uint64_t)(int64_t)-ESRCH;
        break;
    }

    case LINUX_SYS_GETSID:
    case LINUX_SYS_SETSID:
        /* 简化：以 pgid 代替 sid */
        regs[0] = (uint64_t)current->pgid;
        break;

    case LINUX_SYS_GETGROUPS:
    case LINUX_SYS_SETGROUPS:
        regs[0] = 0;
        break;

    default:
        /* 不应到达：调用方负责派发已知号码 */
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
        break;
    }
}
