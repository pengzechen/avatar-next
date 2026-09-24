#ifndef DEBUG_BACKTRACE_H
#define DEBUG_BACKTRACE_H

/*
 * kernel/debug/backtrace.h — 内核调用栈回溯
 *
 * panic / 内核态异常时打印带函数名的调用栈，回答"是谁调过来的"。
 * 实现在 kernel/debug/backtrace.c，设计说明见 docs/basic/BACKTRACE.md。
 *
 * 两条腿：
 *   1. 帧指针链 —— 前提是构建时 FP=1（Makefile 加 -fno-omit-frame-pointer）。
 *      准确、开销小，是主力路径。
 *   2. 栈扫描 —— 帧指针链断了（汇编存根、FP=0 的构建、栈被踩烂）时的兜底。
 *      会有误报也可能漏帧，但至少能给出线索。
 */

#include "types.h"

/* 一次回溯最多输出的帧数。
 * 上界是必须的：栈一旦被踩烂，帧指针链可能是个环，不设上限就转不出来。 */
#define BT_MAX_DEPTH 32

/* 一帧。name 指向 kallsyms 表里的名字（静态存储，不需要释放）；
 * 表里查不到时 name 为 NULL —— 此时只报地址。 */
typedef struct {
    uintptr_t   addr;    /* 调用点地址（返回地址，指向 call 的**下一条**指令）*/
    const char *name;    /* 所属函数名，NULL = 没查到 */
    uintptr_t   offset;  /* addr - 函数入口 */
} bt_entry_t;

/* ── 收集 ─────────────────────────────────────────────────────── */

/*
 * backtrace_collect - 收集**当前**调用栈
 *
 * out[0] 是调用者的那一帧（不含 backtrace_collect 自己）。
 * 返回写入的帧数（可能为 0 —— 栈不可信时宁可不报）。
 */
int backtrace_collect(bt_entry_t *out, int max);

/*
 * backtrace_collect_from - 从给定的点开始收集
 *
 * 给异常/panic 路径用：这几个值直接取自 trap_frame_t（见
 * boot/<arch>/exception.c），此时"当前栈"已经换了，不能靠 __builtin_*。
 *
 *   x86_64  : pc=frame->rip,  fp=frame->rbp,  sp=frame->rsp,  ra=0
 *   aarch64 : pc=frame->elr,  fp=frame->r[29],
 *             sp=(uintptr_t)frame + TRAP_FRAME_SIZE,           ra=0
 *   riscv64 : pc=frame->sepc, fp=frame->x[8], sp=frame->x[2], ra=frame->x[1]
 *
 * pc 非 0 时会被当作第 0 帧记下来（它是出故障的那条指令）。
 *
 * ra 是**给 riscv64 的补充信息**，x86_64/aarch64 传 0：
 * 只有 RISC-V 的叶子函数不把返回地址存到栈上（它没调用过谁，ra 一直活着），
 * 于是那一条调用者的 PC 栈上没有、只躺在 ra 寄存器里。异常路径可以从 trap
 * frame 的 x[1] 把它捞回来，否则这一帧就只能空着。
 * 详见 backtrace.c 里 bt_read_frame 的说明。
 */
int backtrace_collect_from(uintptr_t pc, uintptr_t fp, uintptr_t sp, uintptr_t ra,
                           bt_entry_t *out, int max);

/* ── 打印 ─────────────────────────────────────────────────────── */

/*
 * backtrace_print / backtrace_print_from - 把调用栈打到控制台
 *
 * 输出走 kprintf 而不是 KLOG_* 宏，理由和 include/assert.h 里写的一样：
 * LOG=none 下 KLOG_ERROR 会变成空语句，而崩溃信息必须任何构建配置下都
 * 看得见。一旦 backtrace_panic_enter() 置了 panic 模式，输出还会绕开
 * klog 的全局锁（见下）。
 */
void backtrace_print(void);
void backtrace_print_from(uintptr_t pc, uintptr_t fp, uintptr_t sp, uintptr_t ra);

/*
 * backtrace_dump_fault - 内核态异常处理里的标准三步
 *
 *   klog 摘锁 → 进入 panic 模式 → 用异常帧打调用栈
 *
 * 参数含义同 backtrace_collect_from（ra 只有 riscv64 用得上，其余传 0）。
 *
 * 之后调用 platform_panic() 停机即可：它会看到"这次 panic 已经打过调用栈"
 * 而跳过自己那次，所以不会出现两份栈。
 *
 * ⚠️ 顺序别自己展开重排。如果先调 platform_panic()，它打的是**异常处理
 * 程序自己**的栈（此时早已不在出错的那条路径上），看到的调用栈是错的。
 */
void backtrace_dump_fault(uintptr_t pc, uintptr_t fp, uintptr_t sp, uintptr_t ra);

/*
 * backtrace_render - 渲染进调用者给的缓冲区（/proc/backtrace 用）
 * 返回写入的字节数（不含结尾 NUL），oformat 同 backtrace_print。
 */
int backtrace_render(char *buf, int size);

/* ── panic 支持 ───────────────────────────────────────────────── */

/*
 * backtrace_panic_enter - 宣告"已经进入 panic 路径"
 *
 * 之后 backtrace_print* 会**完全绕开 klog 的全局锁**直写 UART，原因：
 * kvprintf() 会取 g_klog_lock（lib/klog.c），而 panic 完全可能发生在
 * 另一个 CPU 正持锁的时候 —— 那样就是在锁上自旋到天荒地老，调用栈一个字
 * 都打不出来。uart_putchar/uart_putstr 是无锁的（TX FIFO 忙等），是崩溃
 * 路径上唯一可靠的出口。
 *
 * 同时它把输出降到"只打一次"，避免 panic 里再 panic 刷屏。
 * 幂等，可以重复调用。
 */
void backtrace_panic_enter(void);

/* ── 符号查找 ─────────────────────────────────────────────────── */

/*
 * backtrace_lookup - 查 addr 落在哪个函数里
 *
 * 命中返回函数名并把 *offset 设为 addr 相对函数入口的偏移；
 * 没命中（地址不在 .text 里、或表里没有更小的入口）返回 NULL。
 * offset 允许传 NULL。
 */
const char *backtrace_lookup(uintptr_t addr, uintptr_t *offset);

/* ── 自检 ─────────────────────────────────────────────────────── */

/*
 * backtrace_selftest - 开机自检
 *
 * 造一条已知的调用链，验证"展开 + 符号化"整条路是通的。**正常时静默**，
 * 只有检查失败才用 kprintf 报错（同样不受 LOG= 影响）。
 * 失败意味着 kallsyms 表错位或帧指针没生效 —— 那时候 panic 输出的调用栈
 * 是假的，比打不出来更危险，所以要当场喊出来。
 */
void backtrace_selftest(void);

#endif /* DEBUG_BACKTRACE_H */
