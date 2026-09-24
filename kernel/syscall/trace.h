/*
 * kernel/syscall/trace.h — 系统调用环形缓冲（strace 式事后回看）
 *
 * 解决的问题：用户态程序崩了，想按 syscall 调用顺序定位，但逐条打日志
 * （KLOG_SYSCALL）有几个致命短板 ——
 *   1. 要 LOG=debug 全量重编才看得到；
 *   2. 直写 UART，几百行刷过去就没了，而崩溃现场恰恰在最后几条；
 *   3. 崩在 syscall 中间时，只剩一个孤立的入口行，配不出调用对。
 *
 * 这里改成**常开记录 + 按需/自动 dump**：
 *   - 每次 syscall 只往每 CPU 的环形缓冲里写一条（入口写、出口回填返回值），
 *     不碰 UART、不加锁，开销是几次结构体写；
 *   - 进程异常死亡（SIGSEGV 等）时自动把该 pid 最近的记录打出来；
 *   - 也可以随时 cat /proc/syscalls 主动看。
 *
 * 为什么必须常开：崩溃是事后才知道的，没法"补录"。关掉省下来的那点开销，
 * 换来的是崩的时候什么都没有。
 *
 * 为什么每 CPU 一个环而不是一个全局环：syscall 路径本来就关中断、且不会
 * 迁移，所以同一个 CPU 只有它自己在写自己的环 —— 不需要任何锁。
 * 全局环就得在每次 syscall 上取自旋锁，那是热路径上不该有的东西。
 */

#ifndef SYSCALL_TRACE_H
#define SYSCALL_TRACE_H

#include "types.h"

/*
 * 每 CPU 环形缓冲深度（条）。512 条 × 88 字节 × 8 CPU ≈ 360KB 的 .bss。
 * 嵌入式目标（sg2002 等）内存紧张时可以在平台配置里调小。
 */
#ifndef SYSCALL_TRACE_DEPTH
#define SYSCALL_TRACE_DEPTH 512
#endif

/* 单条记录内联保存的路径串长度（含结尾 NUL）*/
#ifndef SYSCALL_TRACE_PATH_MAX
#define SYSCALL_TRACE_PATH_MAX 32
#endif

/*
 * 崩溃时自动 dump 的行数上限。512 条全打会把串口占住好几秒 ——
 * 而且 dump 是在异常上下文里、关中断走的 UART，每条约 1ms，
 * 这里是 32 条 ≈ 30ms。定位问题一般只需要最后几十条，够用。
 * 需要更多上下文时，崩溃后 cat /proc/syscalls 是另一条不受此限的路。
 */
#ifndef SYSCALL_TRACE_DUMP_MAX
#define SYSCALL_TRACE_DUMP_MAX 32
#endif

/* ret 字段的"入口已写、还没返回"哨兵。execve/exit 这类不返回的 syscall
 * 会一直保持这个值 —— dump 时显示成 "= ?"，本身就是有用信息。 */
#define SYSCALL_TRACE_PENDING  (0x5A5A5A5A5A5A5A5AULL)

typedef struct {
    uint64_t seq;                            /* 每 CPU 单调序号，用于排序 */
    uint64_t a0, a1, a2;                     /* 前三个参数 */
    int64_t  ret;                            /* 返回值，或 SYSCALL_TRACE_PENDING */
    uint32_t at_ms;                          /* 相对启动的毫秒数 */
    uint16_t pid;
    uint16_t nr;
    uint8_t  flags;                          /* bit0 = 已返回；bit1 = path[] 有效 */
    uint8_t  _pad[3];
    char     path[SYSCALL_TRACE_PATH_MAX];
} syscall_trace_rec_t;

#define SYSCALL_TRACE_F_RETURNED  0x01U
#define SYSCALL_TRACE_F_PATH      0x02U

/* syscall 号 → 名字（未知返回 "?"）。syscall.c 的流式追踪也用这个。 */
const char *syscall_trace_name(uint32_t nr);

/*
 * 入口记一条。入口只填参数，返回值留哨兵，出口由 syscall_trace_exit() 回填
 * —— 一条 syscall 只占一个槽位，dump 出来是一行一条，和 strace 一致。
 */
void syscall_trace_enter(uint16_t pid, uint32_t nr,
                         uint64_t a0, uint64_t a1, uint64_t a2);

/* 出口回填返回值。没有待回填记录时是空操作。 */
void syscall_trace_exit(int64_t ret);

/* 把最近 max 条打到 klog（WARN 级）。pid_filter==0 表示不过滤 pid。 */
void syscall_trace_dump(uint16_t pid_filter, uint32_t max);

/* 同上，但渲染进 out（给 /proc/syscalls 用）。返回写入长度。 */
int syscall_trace_render(uint16_t pid_filter, uint32_t max, char *out, int len);

/* 已记录的条数（调试/自检用）*/
uint32_t syscall_trace_count(void);

#endif /* SYSCALL_TRACE_H */
