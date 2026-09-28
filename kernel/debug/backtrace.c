/*
 * kernel/debug/backtrace.c — 内核调用栈回溯
 *
 * 实现在这里，用法和设计取舍见 debug/backtrace.h 与 docs/basic/BACKTRACE.md。
 * 下面只补三个实现上的选择：
 *
 * 1. **帧指针链优先，栈扫描兜底**。帧指针链准确、代价低，但只要有一环不
 *    可靠（汇编写的异常存根不建帧、FP=0 的构建、栈被踩烂）就整条断掉。
 *    所以链走不通时改用栈扫描 —— 会有误报，但比"什么都没有"强。
 *
 * 2. **每一步都验证，而不是走到底再验证**。栈被踩烂时帧指针可能指向任意
 *    内存，照着读就是读野地址。所以每一步都要求：地址是内核地址、8 字节
 *    对齐、落在当前栈区间内、返回地址落在 .text 里、且帧指针严格单调递增
 *    （栈向下增长）。任何一条不满足就停，宁可少报几帧。
 *
 * 3. **panic 路径绕开 klog 的锁**。kvprintf() 取 g_klog_lock，而 panic 完全
 *    可能发生在别的 CPU 正持锁的时候 —— 那样就在锁上自旋到死，一个字都打
 *    不出来。见 backtrace_panic_enter()。
 */

#include "debug/backtrace.h"
#include "arch.h"
#include "arg.h"
#include "klog.h"
#include "task/task.h"
#include "task/cpu.h"

/* ── 外部符号 ──────────────────────────────────────────────────── */

/* 函数符号表：由 tools/gen_kallsyms.py 从 stage-1 的 ELF 抽出后生成汇编，
 * 链接进 .kallsyms 段（见 Makefile §11g 与各架构的 boot/<arch>/link.ld）。
 *
 * 注意上面这句只能写成 <arch> 不能写成通配符形式 —— "星号+斜杠"会提前
 * 结束注释，后面的中文会变成代码。klog.h 里也踩过同一个坑。
 *
 * ⚠️ 链接分两趟：stage-1 生成符号表、stage-2 才把真表编进来。两趟都要有
 * **真实定义**的这四个符号 —— stage-1 链的是 gen_kallsyms.py --stub 产出的
 * 空表（count=0），不是"不链接"，更不是弱符号。原因：弱符号在 stage-1 里
 * 是未定义的（地址按 0 解析），本文件里对它的取地址比较会被编成"构造常量
 * 0"，需要的指令序列比 stage-2 的 PC 相对寻址长几个字节 —— 于是 .text
 * 两趟不一样长，后面的函数地址整体偏移，符号表全错位（实测差 16 字节）。
 *
 * 表空不空只看 count：空表的 addrs 数组不参与比较（下面短路了）。 */
extern const uint32_t __kallsyms_count;
extern const uintptr_t __kallsyms_addrs[];
extern const uint32_t __kallsyms_name_off[];
extern const char __kallsyms_names[];

/* 代码区间，三个 link.ld 都导出。用来判断"这个值像不像返回地址"。 */
extern const char _stext[];
extern const char _etext[];

/* 启动栈的上下界，由各架构的 boot.S 导出。
 * 拿不到当前任务的栈时用它 —— 见 bt_stack_bounds()。
 * 名字三个架构不一致（历史原因），只能分开声明。 */
#if ARCH_X86_64
extern const char boot_stack_bottom[];
extern const char boot_stack_top[];
#define BT_BOOT_STACK_LO ((uintptr_t)boot_stack_bottom)
#define BT_BOOT_STACK_HI ((uintptr_t)boot_stack_top)
#elif ARCH_AARCH64
extern const char stack_bottom[];
extern const char stack_top[];
#define BT_BOOT_STACK_LO ((uintptr_t)stack_bottom)
#define BT_BOOT_STACK_HI ((uintptr_t)stack_top)
#elif ARCH_RISCV64
/*
 * ⚠️ 用 stack_bottom/stack_top 而不是 boot_stack_bottom/boot_stack_top。
 * boot.S 里两对都有，但 boot_stack_* 在**低地址**的 .bss.boot 段，而内核
 * 代码跑在高 VMA（0xffffffc0...）—— 两者相距远超 PC 相对寻址的 ±2GB，
 * 引用它直接报 R_RISCV_PCREL_HI20 截断，链接都过不去。
 * 语义上也是 stack_* 才对：panic 时 SP 一般已经落在高半区别名上。
 */
extern const char stack_bottom[];
extern const char stack_top[];
#define BT_BOOT_STACK_LO ((uintptr_t)stack_bottom)
#define BT_BOOT_STACK_HI ((uintptr_t)stack_top)
#endif

/* 平台提供的无锁 UART 输出（也就是 klog 自己用的那个出口）
 * 与 klog 的多行缓冲格式化。和 lib/klog.c 的用法保持一致。 */
extern void uart_putstr(const char *str);
extern int my_vsnprintf(char *buf, int size, const char *fmt, va_list va);

/*
 * bt_get_sp - 读当前栈指针
 *
 * 为什么不直接用 __builtin_frame_address(0)：那是**帧**指针，不是栈指针。
 * FP=1 时它比真正的 sp 高出一整个帧；FP=0 时它压根不是帧指针（rbp 已经被
 * 当通用寄存器用了），拿去当扫描起点会偏出去。
 *
 * 读 sp 没有现成的 wrapper（exception_impl.h 里那些是管中断屏蔽的），
 * 就地内联汇编 —— 和 boot/x86_64/exception.c 读 CR2/CR3 是同一个做法。
 */
static uintptr_t bt_get_sp(void)
{
    uintptr_t sp;

#if ARCH_X86_64
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
#elif ARCH_AARCH64
    __asm__ volatile("mov %0, sp" : "=r"(sp));
#elif ARCH_RISCV64
    __asm__ volatile("mv %0, sp" : "=r"(sp));
#endif

    return sp;
}

/* 内核态地址的下界。
 * 三个架构的 KERNEL_VMA_OFF / PHYS_OFFSET 都不低于这个值
 * （x86_64 0xffff8000_00000000、aarch64 0xffff0000_00000000、
 *  riscv64 0xffffffc0_00000000），所以一个常量对三者都成立。 */
#define BT_KERNEL_ADDR_MIN 0xffff000000000000UL

/* ── 输出通道 ──────────────────────────────────────────────────── */

static bool g_bt_panic;      /* panic 模式：绕开 klog 锁，直写 UART */
static bool g_bt_panic_done; /* panic 模式下已经打过一次，不再刷屏 */
static bool g_bt_busy;       /* 防重入：dump 过程中又崩了 */

void backtrace_panic_enter(void)
{
    g_bt_panic = true;
}

/* 整段调用栈拼成一条消息再发：走 kprintf 时是一条消息一次加锁，多核下
 * 这段输出不会和别的日志交错。 */
static void bt_puts(const char *s)
{
    if (g_bt_panic) {
        uart_putstr(s);
    } else {
        kprintf("%s", s);
    }
}

/* ── 符号查询 ──────────────────────────────────────────────────── */

const char *backtrace_lookup(uintptr_t addr, uintptr_t *offset)
{
    uint32_t n = __kallsyms_count;

    if (offset)
        *offset = 0;

    /* 空表（stage-1 的桩）直接落空；短路保证不会读 addrs[0] */
    if (n == 0 || addr < __kallsyms_addrs[0])
        return NULL;

    /* 二分：找满足 addrs[i] <= addr 的最大 i */
    uint32_t lo = 0, hi = n - 1, best = 0;

    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (__kallsyms_addrs[mid] <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            if (mid == 0)
                break;
            hi = mid - 1;
        }
    }

    if (offset)
        *offset = addr - __kallsyms_addrs[best];

    return __kallsyms_names + __kallsyms_name_off[best];
}

static bool bt_is_text(uintptr_t a)
{
    return a >= (uintptr_t)_stext && a < (uintptr_t)_etext;
}

static void bt_fill(bt_entry_t *e, uintptr_t addr)
{
    e->addr = addr;
    e->name = backtrace_lookup(addr, &e->offset);
}

/* ── 展开 ──────────────────────────────────────────────────────── */

/*
 * bt_read_frame - 从帧记录 fp 取出「上一帧的 fp」和「返回地址」
 *
 * 返回 true 表示 ra 有效；false 表示这是个**叶子帧**，本帧没把返回地址
 * 存到栈上（只有 riscv64 会这样，见下）。
 *
 * ⚠️ 三个架构的帧布局不一样，这是本文件最容易写错的一处。实测 GCC
 * -O2 -fno-omit-frame-pointer 生成：
 *
 *   x86_64 rbp            aarch64 x29            riscv64 s0(x8)
 *   push %rbp             stp x29,x30,[sp,-16]!  addi sp,sp,-N
 *   mov  %rsp,%rbp        mov x29,sp             sd   ra,(N-8)(sp)
 *                                                sd   s0,(N-16)(sp)
 *                                                addi s0,sp,N
 *   [fp+0] = 上帧 fp      [x29+0] = 上帧 x29     [s0-16] = 上帧 s0
 *   [fp+8] = 返回地址     [x29+8] = 返回地址     [s0-8]  = 返回地址
 *
 * RISC-V 的偏移是**负的** —— s0 指向帧顶而不是帧底。照抄 x86 的
 * [fp]/[fp+8] 会读到帧内的局部变量，症状是调用栈里冒出一堆随机函数。
 *
 * RISC-V 还有个额外的坑：**叶子函数不保存 ra**（压根没调用过谁，ra 一直
 * 活着，没必要存），于是 GCC 把它省下来的槽位给了 s0：
 *
 *   bt_temp_b（非叶子）: addi sp,sp,-16; sd s0,0(sp); sd ra,8(sp); addi s0,sp,16
 *   bt_temp_c（叶子）  : addi sp,sp,-16; sd s0,8(sp);               addi s0,sp,16
 *                                            ^^^^^^^ 只有 s0，没有 ra
 *
 * 也就是说 [s0-8] 里装的到底是 ra 还是 s0，得看这个函数是不是叶子。
 * 判据是"它是不是代码地址"。叶子帧的返回地址栈上根本没有 —— 只活在 ra
 * 寄存器里，异常路径可以从 trap frame 的 x[1] 捞回来（见
 * backtrace_dump_fault 的 ra 参数）。
 */
static bool bt_read_frame(uintptr_t fp, uintptr_t *prev_fp, uintptr_t *ra)
{
#if ARCH_RISCV64
    uintptr_t top = *(const uintptr_t *)(fp - 8);

    if (bt_is_text(top)) {
        *ra = top;
        *prev_fp = *(const uintptr_t *)(fp - 16);
        return true;
    }

    *prev_fp = top; /* 叶子帧：[s0-8] 里是上一帧的 s0 */
    *ra = 0;
    return false;
#else
    *prev_fp = *(const uintptr_t *)fp;
    *ra = *(const uintptr_t *)(fp + 8);
    return true;
#endif
}

/* 读一个帧记录要在 fp 上下各留多少余量 —— 必须和 bt_read_frame 的偏移一致，
 * 否则边界检查会放进一个读起来越界的 fp。 */
#if ARCH_RISCV64
#define BT_FRAME_PAD_LO 16
#define BT_FRAME_PAD_HI 0
#else
#define BT_FRAME_PAD_LO 0
#define BT_FRAME_PAD_HI 16
#endif

/*
 * bt_stack_bounds - 当前内核栈的 [lo, hi)
 *
 * 拿到栈区间才能判断一个帧指针是不是"落在栈里"，这是防读野地址的
 * 主要手段。
 *
 * ⚠️ 用任务栈之前必须先用 sp 验一下。task_init() 里 sched_init() 一执行，
 * task_current() 就返回 idle 了，但 CPU 此时还跑在 boot 栈上，要等
 * task_switch_to_idle_stack() 才真正切过去。那段窗口里如果照用 idle 的
 * 栈边界，当前栈上的所有帧都会被判成"不在栈里"，一个都走不出来。
 *
 * 三个分支依次是：任务的栈（且 sp 确实在里面）→ 启动栈 → 都不认识时以 sp
 * 为中心开一个窗口（至少让首帧自身能过检查，剩下的靠单调性和 .text 检查兜）。
 */
static void bt_stack_bounds(uintptr_t sp, uintptr_t *lo, uintptr_t *hi)
{
    task_t *cur = task_current();

    if (cur != NULL && cur->stack_base != NULL) {
        uintptr_t l = (uintptr_t)cur->stack_base;
        uintptr_t h = l + TASK_STACK_SIZE;

        if (sp >= l && sp < h) {
            *lo = l;
            *hi = h;
            return;
        }
    }

    if (sp >= BT_BOOT_STACK_LO && sp < BT_BOOT_STACK_HI) {
        *lo = BT_BOOT_STACK_LO;
        *hi = BT_BOOT_STACK_HI;
        return;
    }

    *lo = sp - TASK_STACK_SIZE;
    *hi = sp + TASK_STACK_SIZE;
}

/*
 * bt_walk_fp - 顺着帧指针链向上走
 *
 * fp 是起始帧的帧指针，返回的第一帧是 fp 所属函数的**调用者**里的位置
 * （[fp+8]/[fp-8] 存的是返回地址，也就是 call 的下一条指令）。
 */
static int bt_walk_fp(uintptr_t fp, uintptr_t sp, uintptr_t seed_ra,
                      bt_entry_t *out, int max)
{
    uintptr_t lo, hi;
    int n = 0;
    bool first = true;

    bt_stack_bounds(sp, &lo, &hi);

    while (n < max) {
        /*
         * 每一步都验证，不信任链上的任何一环：
         * 栈被踩烂时 fp 可能指向任意内存，照着读就是读野地址。
         *
         * ⚠️ 边界余量按架构给（BT_FRAME_PAD_*），不能一律写成 fp < lo+16 ——
         * 首帧的 fp 往往就等于调用者给的 sp，多减 16 会让**第一帧直接被
         * 拒掉**，整条帧指针链一次都走不通（实测踩过：所有调用栈都退化成
         * 栈扫描）。
         */
        if (fp < BT_KERNEL_ADDR_MIN || (fp & (sizeof(uintptr_t) - 1)) != 0)
            break;
        if (fp < lo + BT_FRAME_PAD_LO || fp + BT_FRAME_PAD_HI > hi)
            break;

        uintptr_t prev_fp, ra;
        bool ra_ok = bt_read_frame(fp, &prev_fp, &ra);

        if (ra_ok) {
            if (!bt_is_text(ra)) /* 不是返回地址：链到头了，或者这帧是假的 */
                break;
        } else {
            /*
             * 叶子帧（只有 riscv64 会这样，见 bt_read_frame）：本帧没把返回
             * 地址存到栈上，它只活在 ra 寄存器里。第一帧还能靠异常路径递进来
             * 的 ra 补上；再往上就没有来源了 —— 跳过这一层的 PC，但**继续
             * 沿帧链走**，不能因为少一格就把后面的调用者全丢掉。
             */
            ra = (first && bt_is_text(seed_ra)) ? seed_ra : 0;
        }

        if (prev_fp <= fp) /* 栈向下增长，往上走必须严格递增 */
            break;

        if (ra != 0)
            bt_fill(&out[n++], ra);
        fp = prev_fp;
        first = false;
    }

    return n;
}

/*
 * bt_scan_stack - 栈扫描兜底
 *
 * 从 sp 往栈顶逐字扫描，把落在 .text 里的值当作返回地址。帧指针链走不通
 * 时才用（汇编存根、FP=0 的构建、栈被踩烂）。
 *
 * 代价是**会误报**：局部变量、被调用者保存的旧 PC、任何碰巧等于代码地址
 * 的常量都会被当成一帧。所以它只是补充，不替代帧指针链。
 */
static int bt_scan_stack(uintptr_t sp, uintptr_t lo, uintptr_t hi,
                         bt_entry_t *out, int n, int max)
{
    uintptr_t p = sp & ~(uintptr_t)(sizeof(uintptr_t) - 1);

    if (p < lo)
        p = lo;

    for (; p + sizeof(uintptr_t) <= hi && n < max; p += sizeof(uintptr_t)) {
        uintptr_t v = *(const uintptr_t *)p;

        if (!bt_is_text(v))
            continue;
        if (n > 0 && out[n - 1].addr == v) /* 同一个值的多份副本，压掉 */
            continue;

        bt_fill(&out[n++], v);
    }

    return n;
}

/*
 * 最近一次收集用的是哪条路：true = 帧指针链没走通、退化成栈扫描。
 * 渲染和自检都靠它说明"这份调用栈可不可信"。
 */
static bool g_bt_last_used_scan;

/*
 * 收集缓冲：**必须放 BSS，不能放栈上。**
 *
 * 栈扫描是"从 sp 往上逐字找像返回地址的值"，而调用者传进来的 out[] 往往
 * 就落在**当前这个栈**上（backtrace_selftest 的局部数组、bt_render_common
 * 的 e[]）。边扫边往 out[] 里写，等于一边扫一边往被扫的区域里写 .text
 * 地址 —— 扫到那一段就把自己刚写进去的值再认一遍，越写越多。
 * 实测症状：32 帧全是 bt_selftest_c/b/a/selftest 四个地址循环重复，
 * 直到撞上 BT_MAX_DEPTH。
 *
 * 放 BSS 就不可能落在任何内核栈区间里（内核栈都是 BSS 里的独立数组，
 * 互不相交），这个自反馈路径就断了。代价是不可重入 —— 但整个模块本来
 * 就用 g_bt_busy 串行化了。
 */
static bt_entry_t g_bt_scratch[BT_MAX_DEPTH];

static int bt_collect(uintptr_t pc, uintptr_t fp, uintptr_t sp, uintptr_t ra,
                      int max)
{
    bt_entry_t *out = g_bt_scratch;
    int n = 0;

    g_bt_last_used_scan = false;

    if (max <= 0)
        return 0;
    if (max > BT_MAX_DEPTH)
        max = BT_MAX_DEPTH;

    /* 出故障的那条指令算第 0 帧。不在 .text 里也照报 —— 跳到野地址正是
     * 最需要看清的情况，此时 name 为 NULL，只显示地址。 */
    if (pc != 0)
        bt_fill(&out[n++], pc);

    if (fp != 0)
        n += bt_walk_fp(fp, sp, ra, out + n, max - n);

    /*
     * 帧指针链只走出一帧（通常只有 pc 那一帧）说明它没起作用 —— 换成栈
     * 扫描。走通了就不用扫描：扫描的误报会污染一份本来干净的调用栈。
     *
     * ⚠️ 扫描要从 keep 开始**追加**，不能从 0 开始重写。异常路径下 out[0]
     * 是那条出错的指令（pc），是整份调用栈里最该看见的一帧；从 0 重写会
     * 把它吃掉，症状是调用栈看着挺正常、唯独少了出错的那一条
     * （riscv64 实测：sepc 明明打出来了，backtrace 的 #0 却是它的调用者）。
     */
    if (n < 2 && sp != 0) {
        uintptr_t lo, hi;
        int keep = n;

        bt_stack_bounds(sp, &lo, &hi);
        n = bt_scan_stack(sp, lo, hi, out, keep, max);

        g_bt_last_used_scan = true;
    }

    return n;
}

/* 把 g_bt_scratch 里的结果交给调用者。
 * 只在 bt_collect **返回之后**才写 out —— 见 g_bt_scratch 的说明：
 * 扫描期间往栈上的 out 写会自反馈。 */
static int bt_copy_out(bt_entry_t *out, int max, int n)
{
    if (out == NULL)
        return n;
    if (n > max)
        n = max;

    for (int i = 0; i < n; i++)
        out[i] = g_bt_scratch[i];

    return n;
}

__attribute__((noinline)) int backtrace_collect(bt_entry_t *out, int max)
{
    /*
     * 帧指针必须在这里取（而不是被内联出去的某个 helper 里）：取到的是
     * **本函数**的帧记录，往上走一步正好落到调用者那一帧，于是 out[0] 是
     * 调用者的位置 —— 不会把 backtrace 自己的内部帧混进调用者的栈里。
     * noinline 是给"有人哪天开了 LTO"兜底的。
     */
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);

    /* ra 传 0：从"当前栈"开始时，起始帧一定是 backtrace 自己（它调用过
     * 东西，不是叶子），帧记录里必然存着返回地址，用不上这个提示。 */
    return bt_copy_out(out, max, bt_collect(0, fp, bt_get_sp(), 0, max));
}

int backtrace_collect_from(uintptr_t pc, uintptr_t fp, uintptr_t sp,
                           uintptr_t ra, bt_entry_t *out, int max)
{
    return bt_copy_out(out, max, bt_collect(pc, fp, sp, ra, max));
}

/* ── 渲染 ──────────────────────────────────────────────────────── */

/* 追加一段格式化文本，返回新的 pos。
 * my_vsnprintf 是 C99 语义（截断时返回"本该多长"），所以必须自己 clamp ——
 * 否则 pos 会一路越界，最后写穿缓冲区。 */
static int bt_appendf(char *buf, int size, int pos, const char *fmt, ...)
{
    va_list va;
    int r;

    if (pos >= size - 1)
        return size - 1;

    va_start(va, fmt);
    r = my_vsnprintf(buf + pos, size - pos, fmt, va);
    va_end(va);

    if (r < 0)
        return pos;
    if (pos + r > size - 1)
        return size - 1;

    return pos + r;
}

static int bt_render_entry(char *buf, int size, int pos, int idx,
                           const bt_entry_t *e)
{
    if (e->name != NULL) {
        return bt_appendf(buf, size, pos, "  #%-2d 0x%lx  %s+0x%lx\n", idx,
                          (unsigned long)e->addr, e->name,
                          (unsigned long)e->offset);
    }
    return bt_appendf(buf, size, pos, "  #%-2d 0x%lx  <no symbol>\n", idx,
                      (unsigned long)e->addr);
}

static int bt_render_common(char *buf, int size, uintptr_t pc, uintptr_t fp,
                            uintptr_t sp, uintptr_t ra, const char *banner)
{
    int n, pos;

    /* 下限 32 是给下面"截断标记"直接写尾部留余量（size-24 不能下溢） */
    if (buf == NULL || size < 32)
        return 0;

    /* 直接读 g_bt_scratch，不再往本函数栈上开一份 e[] —— 那正是扫描
     * 自反馈的源头，见 g_bt_scratch 的说明。 */
    n = bt_collect(pc, fp, sp, ra, BT_MAX_DEPTH);

    pos = bt_appendf(buf, size, 0, "%s\n", banner);

    if (g_bt_last_used_scan) {
        pos = bt_appendf(
            buf, size, pos,
            "  [bt] 帧指针链不可用，以下是栈扫描结果（可能有误报）\n");
    }

    for (int i = 0; i < n; i++)
        pos = bt_render_entry(buf, size, pos, i, &g_bt_scratch[i]);

    if (n == 0)
        pos = bt_appendf(buf, size, pos, "  <空的调用栈>\n");
    else if (pos >= size - 1)
        /* 截断必须明确标出来 —— 否则看起来像"调用栈只有这么深"。
         * 直接覆盖尾部 24 字节（size>=32 已保证不下溢）。 */
        bt_appendf(buf, size, size - 24, "  ... (truncated)\n");

    return pos;
}

static void bt_banner(char *buf, int size, const char *what)
{
    task_t *cur = task_current();

    if (cur != NULL) {
        bt_appendf(buf, size, 0, "=== BACKTRACE%s [C%u '%s' id=%u] ===", what,
                   klog_cpu_id(), cur->name, cur->id);
    } else {
        /* task_init() 之前的 boot 上下文没有任务可报 */
        bt_appendf(buf, size, 0, "=== BACKTRACE%s [C%u boot] ===", what,
                   klog_cpu_id());
    }
}

/*
 * 供 /proc/backtrace 用：渲染"读这个文件的那一刻"当前任务的调用栈。
 *
 * 最上面几帧必然是 backtrace_read → vfs → syscall 入口那一串 —— 那条路径
 * **就是**这个任务此刻的内核栈，不是噪声。Linux 的 /proc/PID/stack 也一样。
 */
int backtrace_render(char *buf, int size)
{
    char banner[128];

    bt_banner(banner, (int)sizeof(banner), "");
    return bt_render_common(buf, size, 0, (uintptr_t)__builtin_frame_address(0),
                            bt_get_sp(), 0, banner);
}

void backtrace_print(void)
{
    static char buf[2048];
    char banner[128];

    /*
     * 这两道闸和 backtrace_print_from 里的一样，缺一不可：
     *   g_bt_busy       —— 防重入
     *   g_bt_panic_done —— 异常路径已经用**异常帧**打过一份正确的栈了，
     *                      这里再来一份只会是错的：平台 panic 是由异常处理
     *                      程序调用的，从 platform_panic 往上走要穿过没有帧
     *                      指针的异常存根，捞到的是栈上残留的旧返回地址
     *                      （实测打出了一个跟现场毫无关系的 ramblk_init）。
     *                      两份栈里错的那份更容易把人带偏，所以宁可不打。
     */
    if (g_bt_busy)
        return;
    if (g_bt_panic && g_bt_panic_done)
        return;

    g_bt_busy = true;

    bt_banner(banner, (int)sizeof(banner), "");
    bt_render_common(buf, (int)sizeof(buf), 0,
                     (uintptr_t)__builtin_frame_address(0), bt_get_sp(), 0,
                     banner);
    bt_puts(buf);

    g_bt_busy = false;
}

void backtrace_print_from(uintptr_t pc, uintptr_t fp, uintptr_t sp,
                          uintptr_t ra)
{
    static char buf[2048];
    char banner[128];

    /*
     * 重入和"只打一次"两道闸：
     *   g_bt_busy       —— dump 本身又崩了（符号表坏了、栈彻底烂了），
     *                      别无限递归。
     *   g_bt_panic_done —— panic 路径只打一次；panic 之后内核已经不可信，
     *                      第二次的栈基本都是噪声，还会把第一次刷出屏幕。
     */
    if (g_bt_busy)
        return;
    if (g_bt_panic && g_bt_panic_done)
        return;

    g_bt_busy = true;

    bt_banner(banner, (int)sizeof(banner), " (fault)");
    bt_render_common(buf, (int)sizeof(buf), pc, fp, sp, ra, banner);
    bt_puts(buf);

    if (g_bt_panic)
        g_bt_panic_done = true;

    g_bt_busy = false;
}

void backtrace_dump_fault(uintptr_t pc, uintptr_t fp, uintptr_t sp,
                          uintptr_t ra)
{
    klog_panic_begin();      /* 先把 klog 的全局锁摘掉，否则可能卡在锁上 */
    backtrace_panic_enter(); /* 改走无锁 UART，并只打一次 */
    backtrace_print_from(pc, fp, sp, ra);
}

/* ── 自检 ──────────────────────────────────────────────────────── */

/* 让 GCC 不能把"调用后直接返回"做成尾调用 —— 尾调用会把帧吃掉，
 * 那样这条测试链就只剩一层，测不出展开对不对。 */
static volatile int bt_selftest_sink;

static __attribute__((noinline)) int bt_selftest_c(bt_entry_t *out, int max)
{
    int n = backtrace_collect(out, max);

    bt_selftest_sink = n;
    return n;
}

static __attribute__((noinline)) int bt_selftest_b(bt_entry_t *out, int max)
{
    int n = bt_selftest_c(out, max);

    bt_selftest_sink = n;
    return n;
}

static __attribute__((noinline)) int bt_selftest_a(bt_entry_t *out, int max)
{
    int n = bt_selftest_b(out, max);

    bt_selftest_sink = n;
    return n;
}

/* freestanding 下不依赖 strstr 是否可用（string.h 里没有它） */
static bool bt_name_contains(const char *hay, const char *needle)
{
    if (hay == NULL)
        return false;

    for (; *hay; hay++) {
        const char *h = hay, *n = needle;

        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (*n == '\0')
            return true;
    }
    return false;
}

void backtrace_selftest(void)
{
    /*
     * 期望的链路：backtrace_collect 的调用者开始往上是
     *   out[0] = bt_selftest_c, out[1] = bt_selftest_b, out[2] = bt_selftest_a
     * 用"包含"而不是全等匹配：GCC 可能给 static 函数加上 .constprop.0 /
     * .isra.0 后缀。
     */
    static const char *const want[] = {
        "bt_selftest_c",
        "bt_selftest_b",
        "bt_selftest_a",
    };
    bt_entry_t e[BT_MAX_DEPTH];
    int n, bad = -1;

    n = bt_selftest_a(e, BT_MAX_DEPTH);

    for (int i = 0; i < 3; i++) {
        if (i >= n || !bt_name_contains(e[i].name, want[i])) {
            bad = i;
            break;
        }
    }

    if (bad < 0) {
        /*
         * 正常时 LOG=info 下完全静默，只在 LOG=debug 下留一份完整的
         * 调用栈 —— 它同时是"展开对不对"和"符号化对不对"的现场证据，
         * 比一句 ok 有用得多。
         */
        KLOG_DEBUG("[bt] selftest ok: %d frames, 走的是%s\n", n,
                   g_bt_last_used_scan ? "栈扫描（帧指针链没走通）"
                                       : "帧指针链");
        for (int i = 0; i < n; i++) {
            if (e[i].name != NULL) {
                KLOG_DEBUG("[bt]   #%-2d 0x%lx  %s+0x%lx\n", i,
                           (unsigned long)e[i].addr, e[i].name,
                           (unsigned long)e[i].offset);
            } else {
                KLOG_DEBUG("[bt]   #%-2d 0x%lx  <no symbol>\n", i,
                           (unsigned long)e[i].addr);
            }
        }
        return;
    }

    /*
     * 这里必须用 kprintf 而不是 KLOG_ERROR：LOG=none 会把 KLOG_ERROR 编成
     * 空语句，而"调用栈是假的"这件事任何构建配置下都得看得见 —— 假的栈比
     * 打不出来更危险，它会把人带到完全错误的方向去。理由同 include/assert.h。
     */
    kprintf(
        KLOG_COLOR_RED
        "[bt] SELFTEST FAILED: 第 %d 帧应是 '%s'，实际是 '%s' (共 %d 帧)\n" KLOG_COLOR_RESET,
        bad, want[bad],
        (bad < n && e[bad].name != NULL) ? e[bad].name : "<no symbol>", n);
    kprintf("[bt] panic 时打出的调用栈不可信。常见原因：\n");
    kprintf(
        "[bt]   1. kallsyms 表错位 —— link.ld 里 .kallsyms 段被挪到了 .text 之前\n");
    kprintf("[bt]   2. FP=0 构建 —— 帧指针链不可用，退化成栈扫描\n");
    kprintf("[bt] 详见 docs/basic/BACKTRACE.md\n");
}
