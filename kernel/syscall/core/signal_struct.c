/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/syscall/core/signal_struct.c — 进程级信号处置（signal_struct）

   把 sig_action_t[NSIG] 从 task_t 里拆出来，原因见
   docs/task_t_refactor.md：
     - 2 KiB 的数组占了 TCB 的 58%，但典型进程只装 2~5 个 handler；
     - Linux 中信号处置属于线程组（CLONE_THREAD 共享），不是每线程一份。

   约定：
     - task_t::sig == NULL 表示全部 SIG_DFL，不为没装 handler 的任务
       浪费 2 KiB；第一次 rt_sigaction 或创建线程组时才分配。
     - 静态池 g_signal_pool[TASK_MAX]：同时存在的 signal_struct 不会
       超过活任务数，所以不需要动态分配，也不会在早期启动/IRQ 路径失败。
     - refcount 的所有修改都在 g_signal_lock 下；task_t::sig 的读写用
       acquire/release 原子操作，避免发布未初始化完的 actions。
*/

#include "signal.h"
#include "task/task.h"
#include "spinlock.h"
#include "klog.h"
#include "string.h"

static signal_struct_t g_signal_pool[TASK_MAX];
static spinlock_t g_signal_lock = SPINLOCK_INIT;

/* 持锁调用：取一个空闲槽位并初始化为全 SIG_DFL（refcount=1）。 */
static signal_struct_t *sig_alloc_locked(void)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        signal_struct_t *s = &g_signal_pool[i];
        if (s->refcount != 0)
            continue;
        memset(s->actions, 0, sizeof(s->actions)); /* 0 = SIG_DFL */
        s->refcount = 1;
        return s;
    }
    return NULL;
}

bool task_sigaction_get(task_t *t, int sig, sig_action_t *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->sa_handler = SIG_DFL;
    if (!t || sig < 1 || sig > NSIG)
        return false;

    signal_struct_t *s = __atomic_load_n(&t->sig, __ATOMIC_ACQUIRE);
    if (!s)
        return false;
    *out = s->actions[sig - 1];
    return true;
}

int task_sigaction_set(task_t *t, int sig, const sig_action_t *act)
{
    if (!t || !act || sig < 1 || sig > NSIG)
        return -1;

    uint64_t flags;
    spin_lock_irqsave(&g_signal_lock, &flags);

    signal_struct_t *s = __atomic_load_n(&t->sig, __ATOMIC_RELAXED);
    /*
       纯 SIG_DFL 且 mask/flags 全 0 时无需实例化（NULL 本身就是这个
       语义）；但 SIG_DFL 带非零 mask 或 SIG_IGN 都要存下来，否则
       rt_sigaction 的 oldact 读回值会和 Linux 不一致。
    */
    bool needs_store = act->sa_handler != SIG_DFL || act->sa_flags != 0 || act->sa_restorer != 0 || act->sa_mask != 0;
    if (!s && needs_store) {
        s = sig_alloc_locked();
        if (!s) {
            spin_unlock_irqrestore(&g_signal_lock, flags);
            return -1;
        }
        __atomic_store_n(&t->sig, s, __ATOMIC_RELEASE);
    }
    if (s)
        s->actions[sig - 1] = *act;

    spin_unlock_irqrestore(&g_signal_lock, flags);
    return 0;
}

/*
   task_signal_share - CLONE_THREAD：子线程与父任务共享同一份处置

   如果父任务还是 NULL（线程组里没人装过 handler），先分配一份全
   SIG_DFL 的实例。否则线程 A 装 handler 时只会写自己的 t->sig，
   线程 B 仍看到 NULL，进程级语义就断了。
*/
signal_struct_t *task_signal_share(task_t *parent)
{
    if (!parent)
        return NULL;

    uint64_t flags;
    spin_lock_irqsave(&g_signal_lock, &flags);

    signal_struct_t *s = __atomic_load_n(&parent->sig, __ATOMIC_RELAXED);
    if (!s) {
        s = sig_alloc_locked();
        if (!s) {
            spin_unlock_irqrestore(&g_signal_lock, flags);
            KLOG_ERROR("[signal] signal_struct pool exhausted (share)\n");
            return NULL;
        }
        __atomic_store_n(&parent->sig, s, __ATOMIC_RELEASE);
    }
    s->refcount++;

    spin_unlock_irqrestore(&g_signal_lock, flags);
    return s;
}

/* fork：复制父进程的处置（不是共享）。父进程为 NULL 时子进程也为 NULL。 */
bool task_signal_copy(task_t *parent, task_t *child)
{
    if (!parent || !child)
        return false;

    uint64_t flags;
    spin_lock_irqsave(&g_signal_lock, &flags);

    signal_struct_t *ps = __atomic_load_n(&parent->sig, __ATOMIC_ACQUIRE);
    if (!ps) {
        __atomic_store_n(&child->sig, NULL, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&g_signal_lock, flags);
        return true;
    }

    signal_struct_t *cs = sig_alloc_locked();
    if (!cs) {
        spin_unlock_irqrestore(&g_signal_lock, flags);
        KLOG_ERROR("[signal] signal_struct pool exhausted (fork)\n");
        return false;
    }
    memcpy(cs->actions, ps->actions, sizeof(cs->actions));
    __atomic_store_n(&child->sig, cs, __ATOMIC_RELEASE);

    spin_unlock_irqrestore(&g_signal_lock, flags);
    return true;
}

/* TCB 回收时调用：最后一个引用释放后才把池槽位还回去。 */
void task_signal_release(task_t *t)
{
    if (!t)
        return;

    signal_struct_t *s = __atomic_load_n(&t->sig, __ATOMIC_ACQUIRE);
    if (!s)
        return;
    __atomic_store_n(&t->sig, NULL, __ATOMIC_RELEASE);

    uint64_t flags;
    spin_lock_irqsave(&g_signal_lock, &flags);
    if (s->refcount > 0)
        s->refcount--;
    spin_unlock_irqrestore(&g_signal_lock, flags);
}

/*
   exec：被 caught 的 handler 重置为 SIG_DFL，SIG_IGN 保留。

   与旧实现一样保留 SIG_IGN（glibc/musl 依赖它跨 exec 忽略 SIGCHLD 等）。
   和线程共享时先 unshare：exec 只应改变当前任务的处置，不能把还在
   跑的其他线程的 handler 清掉。没有 SIG_IGN 可保留时直接把引用还回去，
   让 t->sig 回到 NULL（省掉一份全默认的 2 KiB）。
*/
void task_signal_exec_reset(task_t *t)
{
    if (!t)
        return;

    uint64_t flags;

    spin_lock_irqsave(&g_signal_lock, &flags);

    signal_struct_t *s = __atomic_load_n(&t->sig, __ATOMIC_ACQUIRE);
    if (!s) {
        spin_unlock_irqrestore(&g_signal_lock, flags);
        return;
    }

    bool has_ign = false;
    for (int i = 0; i < NSIG; i++) {
        if (s->actions[i].sa_handler == SIG_IGN) {
            has_ign = true;
            break;
        }
    }

    if (s->refcount == 1) {
        for (int i = 0; i < NSIG; i++) {
            if (s->actions[i].sa_handler != SIG_IGN)
                memset(&s->actions[i], 0, sizeof(s->actions[i]));
        }
        if (!has_ign) {
            __atomic_store_n(&t->sig, NULL, __ATOMIC_RELEASE);
            s->refcount = 0;
        }
        spin_unlock_irqrestore(&g_signal_lock, flags);
        return;
    }

    /* 共享：没有要保留的 SIG_IGN 就只是解除共享。 */
    if (!has_ign) {
        __atomic_store_n(&t->sig, NULL, __ATOMIC_RELEASE);
        s->refcount--;
        spin_unlock_irqrestore(&g_signal_lock, flags);
        return;
    }

    signal_struct_t *ns = sig_alloc_locked();
    if (!ns) {
        /*
           池满（理论上是 64 个任务全部分配了 signal_struct，又恰好在
           exec）。退化为原地重置：当前任务语义正确，但会连带清掉共享
           线程的 handler。exec 在多线程进程里本来就是罕见路径。
        */
        for (int i = 0; i < NSIG; i++) {
            if (s->actions[i].sa_handler != SIG_IGN)
                memset(&s->actions[i], 0, sizeof(s->actions[i]));
        }
        spin_unlock_irqrestore(&g_signal_lock, flags);
        KLOG_WARN("[signal] signal_struct pool exhausted (exec); reset shared actions in place\n");
        return;
    }

    for (int i = 0; i < NSIG; i++) {
        if (s->actions[i].sa_handler == SIG_IGN)
            ns->actions[i] = s->actions[i];
    }
    __atomic_store_n(&t->sig, ns, __ATOMIC_RELEASE);
    s->refcount--;

    spin_unlock_irqrestore(&g_signal_lock, flags);
}
