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
    if (!g_klog_async_ready || !buf || len == 0)
        return false;

    uint64_t flags;
    spin_lock_irqsave(&g_klog_ring_lock, &flags);

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
    if (g_klog_async_ready)
        return;

    /*
       在此之前（含本行自身的日志）仍然同步写，保证启动期不丢日志。
       klogd 用最低 RT 优先级（FIFO 1）：普通优先级下被唤醒要等 tick，
       日志输出和 wake_max 都会出现 ~10ms 的假延迟；FIFO 1 高于 NORMAL、
       低于所有真正的 RT worker，只在有日志时短暂抢占。
    */
    task_t *t = task_create_kernel_rt("klogd", klogd_fn, NULL, 1, CPU_AFFINITY_ANY);
    if (t)
        g_klog_async_ready = true;
}
