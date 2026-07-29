/*
 * core/proc_ids.c - 进程身份 / 进程组 / 会话相关 syscall
 *
 * 从 kernel/syscall/syscall.c 抽出。覆盖：
 *   getpid / getppid / gettid / set_tid_address
 *   getuid / geteuid / getgid / getegid / setuid / setgid
 *   getpgid / setpgid / getsid / setsid
 *   getgroups / setgroups
 *
 * 当前模型：root 可任意 setuid/setgid；不做权限检查。
 */
#include "syscall/syscall_internal.h"
#include "syscall/syscall.h"
#include "task/task.h"

extern task_t  g_task_pool[TASK_MAX];
extern uint8_t g_stack_used[TASK_MAX];

static bool process_group_exists(uint32_t pgid)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i])
            continue;
        if (g_task_pool[i].state == TASK_DEAD ||
            g_task_pool[i].state == TASK_ALLOCATING)
            continue;
        if (g_task_pool[i].pgid == pgid)
            return true;
    }
    return false;
}

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
        regs[0] = current->uid;
        break;

    case LINUX_SYS_GETEUID:
        regs[0] = current->euid;
        break;

    case LINUX_SYS_GETGID:
        regs[0] = current->gid;
        break;

    case LINUX_SYS_GETEGID:
        regs[0] = current->egid;
        break;

    case LINUX_SYS_SETUID: {
        uint32_t uid = (uint32_t)regs[0];
        current->uid = uid;
        current->euid = uid;
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_SETGID: {
        uint32_t gid = (uint32_t)regs[0];
        current->gid = gid;
        current->egid = gid;
        regs[0] = 0;
        break;
    }

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

    case LINUX_SYS_GETSID: {
        int pid = (int)(int32_t)regs[0];
        task_t *tgt = (pid == 0) ? current : task_find_by_id((uint32_t)pid);
        regs[0] = tgt ? (uint64_t)tgt->sid : (uint64_t)(int64_t)-ESRCH;
        break;
    }

    case LINUX_SYS_SETSID:
        if (process_group_exists(current->id)) {
            regs[0] = (uint64_t)(int64_t)-EPERM;
            break;
        }
        current->sid  = current->id;
        current->pgid = current->id;
        regs[0] = (uint64_t)current->id;
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
