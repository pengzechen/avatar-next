/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/syscall/core/resource.c — 身份/资源/时间类 Linux syscall

   覆盖：
     getgroups / setgroups
     getrlimit / setrlimit / prlimit64
     umask
     prctl（PDEATHSIG / DUMPABLE / NAME / NO_NEW_PRIVS）
     set_robust_list
     times

   设计要点：
     - 这些 syscall 以前是静默成功型桩（返回 0 但不改状态），调用方会
       误以为生效。这里把状态真正挂到 task_t 上，并在 fork/exec 按 Linux
       语义继承。
     - 用户指针一律经 copy_to/from_user_bytes（CLAUDE.md 的「用户指针」
       规则），坏地址返回 EFAULT 而不是把内核打挂。
     - 权限模型只有 euid == 0 这一条（与项目现有 setuid 模型一致），
       不实现 capability。
*/

#include "syscall/syscall_internal.h"
#include "task/task.h"
#include "driver/time.h"
#include "string.h"

/* ── rlimit ─────────────────────────────────────────────────────── */

/* Linux RLIMIT_* 编号在 64 位 ABI 上三架构一致 */
#define RLIMIT_CPU        0
#define RLIMIT_FSIZE      1
#define RLIMIT_DATA       2
#define RLIMIT_STACK      3
#define RLIMIT_CORE       4
#define RLIMIT_RSS        5
#define RLIMIT_NPROC      6
#define RLIMIT_NOFILE     7
#define RLIMIT_MEMLOCK    8
#define RLIMIT_AS         9
#define RLIMIT_LOCKS      10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE   12
#define RLIMIT_NICE       13
#define RLIMIT_RTPRIO     14
#define RLIMIT_RTTIME     15

#define RLIM_INFINITY (~0ULL)

/*
   没有 /proc/sys/fs/nr_open 时的硬上界。LTP setrlimit03 用
   "nr_open + 1" 检查 NOFILE 的硬上界；本环境没有该 proc 文件，测试
   回落到 1024*1024，这里取同一个值。
*/
#define RLIMIT_NOFILE_HARD_MAX (1024ULL * 1024ULL)

static bool rlimit_res_valid(int res)
{
    return res >= 0 && res < (int)TASK_RLIMIT_COUNT;
}

/*
   setrlimit 的公共校验/写入。返回 0 或负 errno。
   newlim 已经是从用户空间拷进来的内核副本。
*/
static int rlimit_apply(task_t *t, int res, const struct kernel_rlimit *newlim)
{
    if (!rlimit_res_valid(res))
        return -EINVAL;
    if (newlim->rlim_cur > newlim->rlim_max)
        return -EINVAL;

    /*
       NOFILE 的硬上界：即使 root 也不能超过 nr_open。Linux 返回 EPERM
       （不是 EINVAL），LTP setrlimit03 就是按这个判的。
    */
    if (res == RLIMIT_NOFILE && newlim->rlim_max > RLIMIT_NOFILE_HARD_MAX)
        return -EPERM;

    /* 非 root 不得提高硬限制 */
    if (t->euid != 0 && newlim->rlim_max > t->rlimit_max[res])
        return -EPERM;

    t->rlimit_cur[res] = newlim->rlim_cur;
    t->rlimit_max[res] = newlim->rlim_max;
    return 0;
}

static void rlimit_get(task_t *t, int res, struct kernel_rlimit *out)
{
    out->rlim_cur = t->rlimit_cur[res];
    out->rlim_max = t->rlimit_max[res];
}

/* ── getgroups / setgroups ──────────────────────────────────────── */

void syscall_groups_handler(uint64_t syscall_num, uint64_t regs[6], task_t *t)
{
    if (syscall_num == LINUX_SYS_GETGROUPS) {
        int size = (int)(int32_t)regs[0];
        uint32_t *list = (uint32_t *)regs[1];

        if (size < 0) {
            regs[0] = (uint64_t)(int64_t)-EINVAL;
            return;
        }
        /* getgroups(0, ...)：只返回数量，不写用户缓冲(POSIX 特例) */
        if (size == 0) {
            regs[0] = (uint64_t)t->ngroups;
            return;
        }
        if (size < t->ngroups) {
            regs[0] = (uint64_t)(int64_t)-EINVAL;
            return;
        }
        if (t->ngroups > 0) {
            if (copy_to_user_bytes(t->groups, list, (uint64_t)t->ngroups * sizeof(uint32_t)) < 0) {
                regs[0] = (uint64_t)(int64_t)-EFAULT;
                return;
            }
        }
        regs[0] = (uint64_t)t->ngroups;
        return;
    }

    /* setgroups(size, list) */
    int size = (int)(int32_t)regs[0];
    const uint32_t *list = (const uint32_t *)regs[1];

    if (size < 0 || size > (int)TASK_GROUPS_MAX) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }
    /* CAP_SETGID：本内核只认 root */
    if (t->euid != 0) {
        regs[0] = (uint64_t)(int64_t)-EPERM;
        return;
    }

    uint32_t tmp[TASK_GROUPS_MAX];
    if (size > 0) {
        if (copy_from_user_bytes(list, tmp, (uint64_t)size * sizeof(uint32_t)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
    }

    t->ngroups = size;
    for (int i = 0; i < size; i++)
        t->groups[i] = tmp[i];
    regs[0] = 0;
}

/* ── getrlimit / setrlimit / prlimit64 ─────────────────────────── */

void syscall_rlimit_handler(uint64_t syscall_num, uint64_t regs[6], task_t *t)
{
    if (syscall_num == LINUX_SYS_GETRLIMIT) {
        int res = (int)(int32_t)regs[0];
        uint64_t uptr = regs[1];

        if (!rlimit_res_valid(res)) {
            regs[0] = (uint64_t)(int64_t)-EINVAL;
            return;
        }
        if (!uptr) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        struct kernel_rlimit rl;
        rlimit_get(t, res, &rl);
        if (copy_to_user_bytes(&rl, (void *)uptr, sizeof(rl)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        regs[0] = 0;
        return;
    }

    if (syscall_num == LINUX_SYS_SETRLIMIT) {
        int res = (int)(int32_t)regs[0];
        uint64_t uptr = regs[1];

        if (!rlimit_res_valid(res)) {
            regs[0] = (uint64_t)(int64_t)-EINVAL;
            return;
        }
        if (!uptr) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        struct kernel_rlimit nl;
        if (copy_from_user_bytes((const void *)uptr, &nl, sizeof(nl)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        int rc = rlimit_apply(t, res, &nl);
        regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
        return;
    }

    /* prlimit64(pid, resource, new_limit, old_limit) */
    int pid = (int)(int32_t)regs[0];
    int res = (int)(int32_t)regs[1];
    uint64_t newp = regs[2];
    uint64_t oldp = regs[3];

    task_t *target = t;
    if (pid != 0 && (uint32_t)pid != t->id) {
        target = task_find_by_id((uint32_t)pid);
        if (!target) {
            regs[0] = (uint64_t)(int64_t)-ESRCH;
            return;
        }
        /* 非 root 不能改/看别人的限制 */
        if (t->euid != 0 && t->euid != target->uid && t->euid != target->euid) {
            regs[0] = (uint64_t)(int64_t)-EPERM;
            return;
        }
    }

    if (!rlimit_res_valid(res)) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    if (newp) {
        struct kernel_rlimit nl;
        if (copy_from_user_bytes((const void *)newp, &nl, sizeof(nl)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        int rc = rlimit_apply(target, res, &nl);
        if (rc < 0) {
            regs[0] = (uint64_t)(int64_t)rc;
            return;
        }
    }

    if (oldp) {
        struct kernel_rlimit rl;
        rlimit_get(target, res, &rl);
        if (copy_to_user_bytes(&rl, (void *)oldp, sizeof(rl)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
    }

    regs[0] = 0;
}

/* ── umask ──────────────────────────────────────────────────────── */

void syscall_umask_handler(uint64_t regs[6], task_t *t)
{
    uint32_t old = t->umask;
    t->umask = (uint32_t)regs[0] & 0777u;
    regs[0] = old;
}

/* ── prctl ──────────────────────────────────────────────────────── */

#define PR_SET_PDEATHSIG    1
#define PR_GET_PDEATHSIG    2
#define PR_GET_DUMPABLE     3
#define PR_SET_DUMPABLE     4
#define PR_SET_NAME         15
#define PR_GET_NAME         16
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39

void syscall_prctl_handler(uint64_t regs[6], task_t *t)
{
    int option = (int)(int32_t)regs[0];
    uint64_t arg2 = regs[1];
    uint64_t arg3 = regs[2];

    (void)arg3;

    switch (option) {
    case PR_SET_PDEATHSIG: {
        int sig = (int)(int32_t)arg2;
        if (sig < 0 || sig > NSIG) {
            regs[0] = (uint64_t)(int64_t)-EINVAL;
            return;
        }
        t->pdeathsig = (uint64_t)sig;
        regs[0] = 0;
        return;
    }
    case PR_GET_PDEATHSIG: {
        if (!arg2) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        int sig = (int)t->pdeathsig;
        if (copy_to_user_bytes(&sig, (void *)arg2, sizeof(sig)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        regs[0] = 0;
        return;
    }
    case PR_GET_DUMPABLE:
        regs[0] = t->dumpable ? 1 : 0;
        return;
    case PR_SET_DUMPABLE: {
        uint64_t v = arg2;
        if (v > 1) {
            regs[0] = (uint64_t)(int64_t)-EINVAL;
            return;
        }
        t->dumpable = (uint8_t)v;
        regs[0] = 0;
        return;
    }
    case PR_SET_NAME: {
        char name[TASK_NAME_LEN];
        memset(name, 0, sizeof(name));
        if (copy_string_from_user((const char *)arg2, name, (int)sizeof(name)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        /* 内核 comm 最长 TASK_NAME_LEN-1，超长截断(Linux 也截断) */
        memcpy(t->name, name, TASK_NAME_LEN);
        t->name[TASK_NAME_LEN - 1] = '\0';
        regs[0] = 0;
        return;
    }
    case PR_GET_NAME: {
        if (!arg2) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        char name[TASK_NAME_LEN];
        memset(name, 0, sizeof(name));
        memcpy(name, t->name, TASK_NAME_LEN);
        name[TASK_NAME_LEN - 1] = '\0';
        if (copy_to_user_bytes(name, (void *)arg2, TASK_NAME_LEN) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        regs[0] = 0;
        return;
    }
    case PR_SET_NO_NEW_PRIVS:
        if (arg2 != 1) {
            regs[0] = (uint64_t)(int64_t)-EINVAL;
            return;
        }
        t->no_new_privs = 1;
        regs[0] = 0;
        return;
    case PR_GET_NO_NEW_PRIVS:
        regs[0] = t->no_new_privs ? 1 : 0;
        return;
    default:
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }
}

/* ── set_robust_list ────────────────────────────────────────────── */

/* struct robust_list_head 在 64 位 ABI 上是 3 个指针 = 24 字节 */
#define ROBUST_LIST_HEAD_SIZE 24u

void syscall_robust_list_handler(uint64_t regs[6], task_t *t)
{
    uint64_t head = regs[0];
    uint64_t len = regs[1];

    if (len != ROBUST_LIST_HEAD_SIZE) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }
    t->robust_list = head;
    regs[0] = 0;
}

/* ── times ──────────────────────────────────────────────────────── */

#define TIMES_TICK_HZ     100ULL /* 与 LAPIC/PIT tick 一致 */
#define TIMES_NS_PER_TICK (1000000000ULL / TIMES_TICK_HZ)

/* ── personality ────────────────────────────────────────────────── */

void syscall_personality_handler(uint64_t regs[6], task_t *t)
{
    uint32_t persona = (uint32_t)regs[0];
    uint32_t old = t->personality;

    /* personality(0xffffffff) 是查询当前 persona(Linux 约定) */
    if (persona != 0xffffffffu)
        t->personality = persona;
    regs[0] = old;
}

/* ── times ──────────────────────────────────────────────────────── */

void syscall_times_handler(uint64_t regs[6], task_t *t)
{
    uint64_t now = timer_get_ns();
    uint64_t ticks = now / TIMES_NS_PER_TICK;

    if (regs[0]) {
        /*
           struct tms { clock_t tms_utime, stime, cutime, cstime; }
           clock_t 在 64 位 Linux 上是 long(8 字节) → 共 32 字节。
        */
        uint64_t tms[4];
        uint64_t stime = t->stime_ns;
        if (t->sc_entry_ns != 0 && now > t->sc_entry_ns)
            stime += now - t->sc_entry_ns;
        uint64_t wall = (t->create_ns != 0 && now > t->create_ns) ? (now - t->create_ns) : 0;
        uint64_t utime = (wall > stime) ? (wall - stime) : 0;

        tms[0] = utime / TIMES_NS_PER_TICK;
        tms[1] = stime / TIMES_NS_PER_TICK;
        tms[2] = 0; /* 子进程时间：本内核未累计 */
        tms[3] = 0;

        if (copy_to_user_bytes(tms, (void *)regs[0], sizeof(tms)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
    }
    regs[0] = ticks;
}
