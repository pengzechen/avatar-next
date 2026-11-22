/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/debug/klog_async.c — 异步内核日志输出

   背景：klog_write() 原来整段持 spin_lock_irqsave 逐字符写 UART。R3 的
   IRQ-off trace 显示这是 x86/riscv 上最大的关中断区间（毫秒级）。

   现在：启动 klogd 内核线程后，klog_write 只把消息 memcpy 进环形缓冲
   （持 IRQ-off 几微秒），UART 输出由 klogd 在中断打开的状态下完成。
   panic 路径和环形缓冲满时仍退回同步输出，保证崩溃现场不丢。
*/

#include "klog.h"
#include "task/task.h"
#include "task/sched.h"
#include "spinlock.h"
#include "string.h"

extern void uart_putchar(char c);

#define KLOG_RING_SIZE 65536u /* 必须是 2 的幂 */
#define KLOG_RING_MASK (KLOG_RING_SIZE - 1u)

static uint8_t g_klog_ring[KLOG_RING_SIZE];
static volatile uint32_t g_klog_head; /* 生产者 */
static volatile uint32_t g_klog_tail; /* 消费者 */
static spinlock_t g_klog_ring_lock = SPINLOCK_INIT;
static bool g_klog_async_ready;
static task_t *g_klog_sleeper;

bool klog_async_enqueue(const char *buf, size_t len)
{
    if (!__atomic_load_n(&g_klog_async_ready, __ATOMIC_ACQUIRE) || !buf || len == 0)
        return false;

    uint64_t flags;
    spin_lock_irqsave(&g_klog_ring_lock, &flags);

    /*
       拿到锁后复查：panic flush 会在置位"异步关闭"之后再来抢这把锁。
       不复查的话，已经在等锁的生产者会在 flush 认领完范围之后又写进环形
       缓冲，那批字节就永远没人消费了。
    */
    if (!__atomic_load_n(&g_klog_async_ready, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&g_klog_ring_lock, flags);
        return false;
    }

    uint32_t used = g_klog_head - g_klog_tail;
    if (len > KLOG_RING_SIZE - used) {
        spin_unlock_irqrestore(&g_klog_ring_lock, flags);
        return false; /* 满：退回同步输出 */
    }

    for (size_t i = 0; i < len; i++)
        g_klog_ring[(g_klog_head + (uint32_t)i) & KLOG_RING_MASK] = (uint8_t)buf[i];
    g_klog_head += (uint32_t)len;

    task_t *sleeper = g_klog_sleeper;
    g_klog_sleeper = NULL;

    spin_unlock_irqrestore(&g_klog_ring_lock, flags);

    if (sleeper)
        task_unblock(sleeper);
    return true;
}

/*
   klog_async_panic_flush - panic 时把已入环的日志同步刷出

   背景：klogd 是普通内核线程，进入 panic 后不会再被调度。如果崩溃前最后
   几条日志还躺在环形缓冲里，它们永远不会被打印出来 —— 而那段恰恰是最想
   看到的现场。这里在 klog_panic_begin() 里主动消费一次。

   并发处理：
     - 先关异步入口(ready=false)，并在 enqueue 拿锁后复查，保证之后不会再
       有生产者往环里写；
     - 尽力拿环形锁：拿到就独占认领 tail..head；拿不到说明有核正卡在临界区，
       直接按快照裸读。panic 下宁可重复输出，也不能丢日志；
     - 认领(把 tail 推到 head)之后释放锁再写 UART，避免持锁做几十 KB 的
       慢速串口输出。此时生产者已被 ready=false 挡住，klogd 看到 head==tail
       也不会再消费，环内容不会被覆盖。
*/
void klog_async_panic_flush(void)
{
    if (!__atomic_load_n(&g_klog_async_ready, __ATOMIC_ACQUIRE))
        return;

    __atomic_store_n(&g_klog_async_ready, false, __ATOMIC_RELEASE);

    uint64_t flags;
    bool locked = (spin_trylock_irqsave(&g_klog_ring_lock, &flags) == 0);

    uint32_t tail = g_klog_tail;
    uint32_t head = g_klog_head;
    g_klog_tail = head; /* 认领整个已入队范围 */

    if (locked)
        spin_unlock_irqrestore(&g_klog_ring_lock, flags);

    for (uint32_t pos = tail; pos != head; pos++)
        uart_putchar((char)g_klog_ring[pos & KLOG_RING_MASK]);
}

static void klogd_fn(void *arg)
{
    (void)arg;
    char chunk[128];

    for (;;) {
        uint64_t flags;
        uint32_t n;

        spin_lock_irqsave(&g_klog_ring_lock, &flags);

        while (g_klog_head == g_klog_tail) {
            task_t *cur = task_current();
            g_klog_sleeper = cur;
            cur->state = TASK_BLOCKED;
            spin_unlock_irqrestore(&g_klog_ring_lock, flags);

            sched_schedule();

            spin_lock_irqsave(&g_klog_ring_lock, &flags);
        }

        uint32_t used = g_klog_head - g_klog_tail;
        n = (used > sizeof(chunk)) ? (uint32_t)sizeof(chunk) : used;
        for (uint32_t i = 0; i < n; i++)
            chunk[i] = (char)g_klog_ring[(g_klog_tail + i) & KLOG_RING_MASK];
        g_klog_tail += n;

        spin_unlock_irqrestore(&g_klog_ring_lock, flags);

        for (uint32_t i = 0; i < n; i++)
            uart_putchar(chunk[i]);
    }
}

void klog_async_start(void)
{
    if (__atomic_load_n(&g_klog_async_ready, __ATOMIC_ACQUIRE))
        return;

    /*
       在此之前（含本行自身的日志）仍然同步写，保证启动期不丢日志。
       klogd 用最低 RT 优先级（FIFO 1）：普通优先级下被唤醒要等 tick，
       日志输出和 wake_max 都会出现 ~10ms 的假延迟；FIFO 1 高于 NORMAL、
       低于所有真正的 RT worker，只在有日志时短暂抢占。
    */
    task_t *t = task_create_kernel_rt("klogd", klogd_fn, NULL, 1, CPU_AFFINITY_ANY);
    if (t)
        __atomic_store_n(&g_klog_async_ready, true, __ATOMIC_RELEASE);
}
