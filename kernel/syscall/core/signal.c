/*
 * signal.c - 内核信号子系统（投递 + 三个 syscall 处理器）
 *
 * 从 kernel/syscall/syscall.c 抽出。包含：
 *   - deliver_pending_signals(): syscall 返回用户态前选取并投递一个信号
 *   - sigaction_handler():       rt_sigaction (Linux 13)
 *   - sigprocmask_handler():     rt_sigprocmask (Linux 14)
 *   - sigreturn_handler():       rt_sigreturn (Linux 15)
 *
 * 入口字符（Ctrl+C / 普通 stdin）的 UART → ringbuf 投递路径仍位于
 * syscall.c 中（signal_check_uart()），因其耦合 g_termios 全局状态。
 */
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "klog.h"
#include "task/task.h"
#include "string.h"
#include "arch.h"
#include "exception.h"
#include "user_layout.h"

/* ── deliver_pending_signals ────────────────────────────────────
 * 在 syscall 即将返回用户态前调用（且 syscall_abi_set_ret 已执行）。
 * 选取最低编号的待投递信号，执行 SIG_DFL / SIG_IGN / 用户 handler。
 * 用户 handler 通过在用户栈上构建 sigframe 实现。
 * ─────────────────────────────────────────────────────────────── */
void
deliver_pending_signals(task_t *t, trap_frame_t *frame)
{
    uint64_t unblocked = t->pending_sigs & ~t->blocked_sigs;
    if (!unblocked) return;

    /* 取最低编号的待投递信号 */
    int sig = 0;
    for (int i = 0; i < NSIG; i++) {
        if (unblocked & (1ULL << i)) { sig = i + 1; break; }
    }
    if (!sig) return;

    /* 清除待投递位 */
    t->pending_sigs &= ~(1ULL << (sig - 1));

    uint64_t sa_handler  = t->sig_actions[sig - 1].sa_handler;
    uint64_t sa_restorer = t->sig_actions[sig - 1].sa_restorer;
    uint64_t sa_mask     = t->sig_actions[sig - 1].sa_mask;

    if (sa_handler == SIG_IGN) return;   /* 忽略 */

    if (sa_handler == SIG_DFL) {
        /* 默认动作：SIGCHLD/SIGCONT/SIGURG/SIGWINCH/SIGTTIN/SIGTTOU/SIGTSTP → 忽略；其余 → 终止 */
        switch (sig) {
        case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH:
        case SIGTTIN: case SIGTTOU: case SIGTSTP: return;
        default:
            KLOG_INFO("[signal] pid=%u: SIG_DFL sig=%d → exit(%d)\n",
                      t->id, sig, 128 + sig);
            t->exit_signal = sig;
            sys_exit(128 + sig);
            return; /* unreachable */
        }
    }

    /* 在 handler 执行期间屏蔽本信号（+ sa_mask） */
    t->sig_saved_blocked = t->blocked_sigs;
    t->blocked_sigs |= (1ULL << (sig - 1)) | (sa_mask & ~((1ULL<<(SIGKILL-1))|(1ULL<<(SIGSTOP-1))));

    /* 在用户栈上压入当前 trap_frame（已含 syscall 返回值），建立 sigframe */
#if ARCH_X86_64
    {
        sa_restorer = USER_SIGRET_PAGE;
        uint64_t usp = frame->rsp & ~15ULL;          /* 16 字节对齐 */
        usp -= sizeof(trap_frame_t);
        memcpy((void *)usp, frame, sizeof(trap_frame_t));
        t->sig_frame_sp = usp;
        usp -= 8;
        *(uint64_t *)usp = sa_restorer;              /* 返回地址 = restorer */
        frame->rsp = usp;
        frame->rip = sa_handler;
        frame->rdi = (uint64_t)(uint32_t)sig;        /* 第一个参数 */
        frame->rflags &= ~(1ULL << 10);              /* 清 DF */
    }
#elif ARCH_AARCH64
    {
        uint64_t usp = frame->usp & ~15ULL;
        /* 如果 sa_restorer == 0，在栈上放一个 rt_sigreturn 蹦床 */
        if (sa_restorer == 0) {
            usp -= 8;
            uint32_t *tramp = (uint32_t *)usp;
            tramp[0] = 0xd2801168u;  /* mov x8, #139 */
            tramp[1] = 0xd4000001u;  /* svc #0       */
            sa_restorer = usp;
        }
        usp -= sizeof(trap_frame_t);
        memcpy((void *)usp, frame, sizeof(trap_frame_t));
        t->sig_frame_sp = usp;
        frame->usp    = usp;
        frame->r[0]   = (uint64_t)(uint32_t)sig;    /* x0 = signum */
        frame->r[30]  = sa_restorer;                /* lr  = restorer */
        frame->elr    = sa_handler;
    }
#elif ARCH_RISCV64
    {
        uint64_t usp = frame->x[2] & ~15ULL;
        /* 如果 sa_restorer == 0，在栈上放一个 rt_sigreturn 蹦床 */
        if (sa_restorer == 0) {
            usp -= 8;
            uint32_t *tramp = (uint32_t *)usp;
            tramp[0] = 0x08b00893u;  /* li a7, 139   */
            tramp[1] = 0x00000073u;  /* ecall         */
            sa_restorer = usp;
        }
        usp -= sizeof(trap_frame_t);
        memcpy((void *)usp, frame, sizeof(trap_frame_t));
        t->sig_frame_sp = usp;
        frame->x[2]  = usp;                         /* sp */
        frame->x[10] = (uint64_t)(uint32_t)sig;     /* a0 = signum */
        frame->x[1]  = sa_restorer;                 /* ra = restorer */
        frame->sepc  = sa_handler;
    }
#endif

    KLOG_DEBUG("[signal] pid=%u: deliver sig=%d handler=0x%llx restorer=0x%llx\n",
              t->id, sig, sa_handler, sa_restorer);
}

/* ── rt_sigaction ─────────────────────────────────────────────── */
void sigaction_handler(uint64_t regs[6], task_t *current)
{
    int sig = (int)regs[0];
    const struct kernel_sigaction *act = (const struct kernel_sigaction *)regs[1];
    struct kernel_sigaction       *old = (struct kernel_sigaction *)regs[2];
    size_t sigsetsize = (size_t)regs[3];

    if (sigsetsize != sizeof(uint64_t)) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    if (sig < 1 || sig > NSIG || sig == SIGKILL || sig == SIGSTOP) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }
    if (old) {
        old->sa_handler  = current->sig_actions[sig-1].sa_handler;
        old->sa_flags    = current->sig_actions[sig-1].sa_flags;
        old->sa_restorer = current->sig_actions[sig-1].sa_restorer;
        old->sa_mask     = current->sig_actions[sig-1].sa_mask;
    }
    if (act) {
        current->sig_actions[sig-1].sa_handler  = act->sa_handler;
        current->sig_actions[sig-1].sa_flags    = act->sa_flags;
        current->sig_actions[sig-1].sa_restorer = act->sa_restorer;
        current->sig_actions[sig-1].sa_mask     = act->sa_mask;
    }
    regs[0] = 0;
}

/* ── rt_sigprocmask ───────────────────────────────────────────── */
void sigprocmask_handler(uint64_t regs[6], task_t *current)
{
    int            how   = (int)regs[0];
    const uint64_t *nset = (const uint64_t *)regs[1];
    uint64_t       *oset = (uint64_t *)regs[2];

    if (oset) *oset = current->blocked_sigs;
    if (nset) {
        /* SIGKILL / SIGSTOP 不可屏蔽 */
        uint64_t m = *nset & ~((1ULL<<(SIGKILL-1))|(1ULL<<(SIGSTOP-1)));
        if      (how == SIG_BLOCK)   current->blocked_sigs |= m;
        else if (how == SIG_UNBLOCK) current->blocked_sigs &= ~m;
        else if (how == SIG_SETMASK) current->blocked_sigs  = m;
        else { regs[0] = (uint64_t)(int64_t)-EINVAL; return; }
    }
    regs[0] = 0;
}

/* ── rt_sigreturn ─────────────────────────────────────────────── */
void sigreturn_handler(uint64_t regs[6], task_t *current, trap_frame_t *frame)
{
    /* 从信号 handler 返回：恢复进入 handler 前的 trap_frame */
    if (current->sig_frame_sp) {
        trap_frame_t *saved = (trap_frame_t *)current->sig_frame_sp;
        memcpy(frame, saved, sizeof(trap_frame_t));
        current->sig_frame_sp = 0;
        /* 恢复信号投递前的 blocked_sigs */
        current->blocked_sigs = current->sig_saved_blocked;
        /* 让 syscall_abi_set_ret 写入已恢复帧中的正确寄存器值（幂等）*/
#if ARCH_X86_64
        regs[0] = frame->rax;
#elif ARCH_AARCH64
        regs[0] = frame->r[0];
#elif ARCH_RISCV64
        regs[0] = frame->x[10];
#endif
    } else {
        regs[0] = 0;
    }
}

/* ── kill (Linux 129) ──────────────────────────────────────────
 * pid > 0 : 单进程; pid == 0 / -1 : 当前 pgid; pid < -1 : (-pid) pgid
 * sig == 0 : 仅做存在性检查
 * ────────────────────────────────────────────────────────────── */
void
kill_handler(uint64_t regs[6], task_t *current)
{
    int pid = (int)(int32_t)regs[0];
    int sig = (int)regs[1];

    if (sig < 0 || sig > NSIG) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    if (sig == 0) { regs[0] = 0; return; }

    if (pid > 0) {
        task_t *tgt = task_find_by_id((uint32_t)pid);
        if (!tgt) { regs[0] = (uint64_t)(int64_t)-ESRCH; return; }
        task_send_signal(tgt, sig);
    } else if (pid == 0) {
        if (!task_send_signal_to_pgid(current->pgid, sig)) {
            regs[0] = (uint64_t)(int64_t)-ESRCH; return;
        }
    } else if (pid == -1) {
        /* 简化：向 current 的 pgid 广播 */
        if (!task_send_signal_to_pgid(current->pgid, sig)) {
            regs[0] = (uint64_t)(int64_t)-ESRCH; return;
        }
    } else {
        /* pid < -1: send to process group (-pid) */
        uint32_t pgid = (uint32_t)(-(int64_t)pid);
        if (pgid == 0 || !task_send_signal_to_pgid(pgid, sig)) {
            regs[0] = (uint64_t)(int64_t)-ESRCH;
            return;
        }
    }
    regs[0] = 0;
}

/* ── tkill (Linux 130) ─────────────────────────────────────────
 * 旧接口：按 tid 向单个任务发送信号。
 * ────────────────────────────────────────────────────────────── */
void
tkill_handler(uint64_t regs[6])
{
    int tid = (int)(int32_t)regs[0];
    int sig = (int)regs[1];

    if (sig == 0) { regs[0] = 0; return; }

    task_t *tgt = task_find_by_id((uint32_t)tid);
    if (!tgt) {
        regs[0] = (uint64_t)(int64_t)-ESRCH;
        return;
    }
    task_send_signal(tgt, sig);
    regs[0] = 0;
}

/* ── tgkill (Linux 131) ────────────────────────────────────────
 * tgid 是线程组 leader 的 PID（不是 pgid）。
 * 单线程进程: tgid == tid；线程: tgid == parent_id（leader）。
 * ────────────────────────────────────────────────────────────── */
void
tgkill_handler(uint64_t regs[6])
{
    int tgid = (int)(int32_t)regs[0];
    int tid  = (int)(int32_t)regs[1];
    int sig  = (int)regs[2];

    if (sig == 0) { regs[0] = 0; return; }

    task_t *tgt = task_find_by_id((uint32_t)tid);
    if (!tgt ||
        (tgid > 0 && (uint32_t)tgid != tgt->id && (uint32_t)tgid != tgt->parent_id)) {
        regs[0] = (uint64_t)(int64_t)-ESRCH;
        return;
    }
    task_send_signal(tgt, sig);
    regs[0] = 0;
}
