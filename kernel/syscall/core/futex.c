/*
 * futex.c - 简易 futex 实现（fixed-size 表，无哈希）
 *
 * 仅支持 FUTEX_WAIT / FUTEX_WAKE 基本语义，足以让 musl pthread mutex / cond 工作。
 * 派生自 kernel/syscall/syscall.c，作为大重构的第一步抽出。
 */
#include "syscall/core/futex.h"
#include "syscall/syscall_internal.h"
#include "task/task.h"
#include "task/sched.h"
#include "task/switch.h"
#include "spinlock.h"
#include "arch.h"

#define FUTEX_TABLE_SIZE 64

static struct {
    uintptr_t uaddr;    /* 用户虚拟地址 (0 = 空闲) */
    task_t   *waiter;   /* 等待该地址的任务 */
} g_futex_table[FUTEX_TABLE_SIZE];

/*
 * 保护上面这张**全局**表。
 * 以前只用 arch_irq_save()（仅关本核中断）——两颗 CPU 同时进来就会把表写坏，
 * 而 futex syscall 在任何 CPU 上都能并发进入。改用真正的自旋锁。
 */
static spinlock_t g_futex_lock = SPINLOCK_INIT;

int
futex_do_wake(uintptr_t uaddr, int count)
{
    int woken = 0;
    uint64_t flags;
    spin_lock_irqsave(&g_futex_lock, &flags);
    for (int i = 0; i < FUTEX_TABLE_SIZE && woken < count; i++) {
        if (g_futex_table[i].uaddr == uaddr && g_futex_table[i].waiter != NULL) {
            task_t *t = g_futex_table[i].waiter;
            g_futex_table[i].uaddr  = 0;
            g_futex_table[i].waiter = NULL;
            t->state = TASK_READY;
            sched_enqueue(t);
            woken++;
        }
    }
    spin_unlock_irqrestore(&g_futex_lock, flags);
    return woken;
}

int
sys_futex_wait(uint32_t *uaddr, uint32_t val)
{
    uint32_t cur_val;
    uint64_t flags;

    /*
     * 用户指针一律经 copy_from_user_bytes 读：这是来自 syscall 参数的地址，
     * 裸解引用（原来的 *(volatile uint32_t *)uaddr）遇到坏地址就是内核态
     * 数据中止 → 整机挂死。见 CLAUDE.md 的用户指针规则。
     */
    if (copy_from_user_bytes(uaddr, &cur_val, sizeof(cur_val)) < 0)
        return -EFAULT;

    spin_lock_irqsave(&g_futex_lock, &flags);

    /* 原子检查：若 *uaddr != val，立即返回 EAGAIN */
    if (cur_val != val) {
        spin_unlock_irqrestore(&g_futex_lock, flags);
        return -EAGAIN;
    }

    /* 找空闲槽 */
    int slot = -1;
    for (int i = 0; i < FUTEX_TABLE_SIZE; i++) {
        if (g_futex_table[i].waiter == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&g_futex_lock, flags);
        return -ENOMEM;
    }

    task_t *cur = task_current();
    g_futex_table[slot].uaddr  = (uintptr_t)uaddr;
    g_futex_table[slot].waiter = cur;
    cur->state = TASK_BLOCKED;

    spin_unlock_irqrestore(&g_futex_lock, flags);

    /* 让出 CPU；被 futex_do_wake 设回 TASK_READY 后继续 */
    sched_schedule();
    return 0;
}

/* ────────────────────────────────────────────────────────────
 * futex syscall 派发：仅支持 FUTEX_WAIT / FUTEX_WAKE 基本语义
 * ──────────────────────────────────────────────────────────── */
void futex_handler(uint64_t regs[6])
{
    uint32_t *uaddr = (uint32_t *)regs[0];
    int op  = (int)regs[1] & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    uint32_t val = (uint32_t)regs[2];
    /* FUTEX_WAKE: regs[2] 是唤醒数量 */
    if (op == FUTEX_WAIT)
        regs[0] = (uint64_t)(int64_t)sys_futex_wait(uaddr, val);
    else if (op == FUTEX_WAKE)
        regs[0] = (uint64_t)(int64_t)futex_do_wake((uintptr_t)uaddr, (int)val);
    else
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
}
