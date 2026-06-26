/*
 * boot/riscv64/exception.c — RISC-V 64-bit S-mode 异常/中断 C 分发器
 *
 * 提供：
 *   exception_init()   — 设置 stvec，配置异常入口
 *   irq_install()      — 注册 scause 对应的中断处理函数
 *   handle_exception() — 从 exception.S 调用，分发陷阱
 */

#include "exception.h"
#include "klog.h"
#include "riscv64/sysreg.h"
#include "syscall/syscall.h"
#include "platform_ops.h"
#include "task/cpu.h"
#include "task/task.h"

/* 中断处理函数表，索引 = scause 低位（去掉 bit63 后的中断编号）*/
#define MAX_IRQ_CAUSES  16
static irq_handler_t interrupt_handlers[MAX_IRQ_CAUSES];

/* ── 公共接口：注册中断处理函数 ────────────────────────────────── */

void irq_install(int cause, irq_handler_t h)
{
    if (cause >= 0 && cause < MAX_IRQ_CAUSES)
        interrupt_handlers[cause] = h;
}

/* ── exception_init ─────────────────────────────────────────────── */

/* Debug counters */
volatile uint64_t g_exception_entry_count = 0;
volatile uint64_t g_exception_code = 0;
volatile uint64_t g_exception_sepc = 0;
volatile uint64_t g_rv_irq_from_kernel = 0;
volatile uint64_t g_rv_irq_from_user = 0;
static volatile uint64_t g_rv_non_timer_irq_count = 0;

static bool rv_irq_log_sample(uint64_t n)
{
    return n <= 4 || (n <= 4096 && (n & (n - 1)) == 0);
}

void riscv_kernel_interrupt_enable(void)
{
    CSR_SET(sstatus, SSTATUS_SIE);
}

void riscv_kernel_interrupt_disable(void)
{
    CSR_CLEAR(sstatus, SSTATUS_SIE);
}

void exception_init(void)
{
    /* trap_vector 在 exception.S 中定义（.align 4，直接模式） */
    extern void trap_vector(void);

    /* 写入 stvec，bits[1:0] = 00 表示 Direct 模式 */
    WRITE_STVEC((uint64_t)trap_vector);

    /*
     * 注意：不在此处使能 sstatus.SIE（内核态中断开关）。
     * trap_vector 已能保存 S->S 的完整现场；这里仍保持 SIE=0，
     * 由调度器和 timer 初始化完成后的显式开关决定何时允许内核态收中断。
     */

    /*
     * 使能 sstatus.SUM（Supervisor User Memory access）。
     * 置 1 后 S 模式可直接读写带 PTE_U 的用户页面，
     * 使 syscall 处理函数能直接解引用用户空间指针（如 write buf）。
     */
    CSR_SET(sstatus, 1UL << 18);   /* bit 18 = SUM */

    KLOG_INFO("RISC-V exception init: stvec=0x%lx, SUM enabled (SIE initially 0)\n",
              (uint64_t)trap_vector);
}

/* ── handle_exception ───────────────────────────────────────────── *
 * 由 exception.S 的 trap_vector 调用，参数为 trap_frame_t *
 */
void handle_exception(void *frame_ptr)
{
    trap_frame_t *frame = (trap_frame_t *)frame_ptr;
    uint64_t cause     = frame->scause;
    uint64_t code = cause & ~SCAUSE_INTERRUPT_BIT;
    
    /* 更新调试计数器 */
    g_exception_entry_count++;
    g_exception_code = code;
    g_exception_sepc = frame->sepc;
    
    /* 调试：打印异常基本信息 */
    if (!(cause & SCAUSE_INTERRUPT_BIT)) {
        // KLOG_ERROR("[exception] sync: code=%llu pc=0x%lx stval=0x%lx sstatus=0x%lx\n",
        //            code, frame->sepc, frame->stval, frame->sstatus);
    }

    if (cause & SCAUSE_INTERRUPT_BIT) {
        /* ── 中断路径 ──────────────────────────────────────────── */
        uint64_t irq = cause & ~SCAUSE_INTERRUPT_BIT;
        cpu_t *cpu = cpu_current();

        cpu->irq_depth++;

        if (irq != CAUSE_SUPERVISOR_TIMER) {
            uint64_t n = ++g_rv_non_timer_irq_count;
            if (rv_irq_log_sample(n)) {
                KLOG_WARN("[riscv irq] non-timer IRQ #%llu cause=%llu mode=%c sepc=0x%lx sstatus=0x%lx stval=0x%lx\n",
                          n, irq,
                          (frame->sstatus & SSTATUS_SPP) ? 'S' : 'U',
                          frame->sepc, frame->sstatus, frame->stval);
            }
        }

        if (frame->sstatus & SSTATUS_SPP) {
            uint64_t n = ++g_rv_irq_from_kernel;
            if (rv_irq_log_sample(n)) {
                // 打开可以确认内核态是否收到中断
                // KLOG_INFO("[riscv irq] S-mode IRQ #%llu cause=%llu sepc=0x%lx sstatus=0x%lx need_resched=%u preempt=%u\n",
                //           n, irq, frame->sepc, frame->sstatus,
                //           cpu->need_resched ? 1U : 0U,
                //           cpu->current_task ? cpu->current_task->preempt_count : 0U);
            }
        } else {
            g_rv_irq_from_user++;
        }

        if (irq < MAX_IRQ_CAUSES && interrupt_handlers[irq]) {
            interrupt_handlers[irq](frame_ptr);
        }
        cpu->irq_depth--;
    } else {
        /* ── 同步异常路径 ────────────────────────────────────── */
        if (code == CAUSE_USER_ECALL) {
            /* RISC-V ecall: sepc 指向 ecall 本身，需手动前进到下一条 */
            frame->sepc += 4;
            syscall_handler(frame);
            return;
        }

        KLOG_ERROR("[exception] sync: code=%llu pc=0x%lx stval=0x%lx sstatus=0x%lx satp=0x%lx\n",
                   code, frame->sepc, frame->stval, frame->sstatus, CSR_READ(satp));

        /* 其他异常：打印简单信息后挂起 */
        if (code == CAUSE_INSN_PAGE_FAULT  || code == CAUSE_LOAD_PAGE_FAULT  ||
            code == CAUSE_STORE_PAGE_FAULT ||
            code == CAUSE_INSN_GUEST_PAGE_FAULT ||
            code == CAUSE_LOAD_GUEST_PAGE_FAULT ||
            code == CAUSE_STORE_GUEST_PAGE_FAULT) {
            const char *fault_type =
                (code == CAUSE_INSN_PAGE_FAULT)        ? "Inst" :
                (code == CAUSE_LOAD_PAGE_FAULT)        ? "Load" :
                (code == CAUSE_STORE_PAGE_FAULT)       ? "Store" :
                (code == CAUSE_INSN_GUEST_PAGE_FAULT)  ? "Inst-G" :
                (code == CAUSE_LOAD_GUEST_PAGE_FAULT)  ? "Load-G" : "Store-G";
            uint64_t sstatus_val = frame->sstatus;
            uint64_t satp_val    = CSR_READ(satp);
#if defined(PLATFORM_SG2002)
            KLOG_ERROR("%s PF: pc=0x%lx va=0x%lx sstatus=0x%lx(SPP=%u) satp=0x%lx(PPN=0x%lx)\n",
                       fault_type, frame->sepc, frame->stval,
                       sstatus_val, (unsigned)((sstatus_val >> 8) & 1),
                       satp_val, satp_val & 0xfffffffffffULL);
#else
            uint64_t hstatus_val = CSR_READ(hstatus);
            KLOG_ERROR("%s PF: pc=0x%lx va=0x%lx sstatus=0x%lx(SPP=%u) hstatus=0x%lx(SPV=%u SPVP=%u) satp=0x%lx(PPN=0x%lx)\n",
                       fault_type, frame->sepc, frame->stval,
                       sstatus_val, (unsigned)((sstatus_val >> 8) & 1),
                       hstatus_val,
                       (unsigned)((hstatus_val >> 7) & 1),
                       (unsigned)((hstatus_val >> 8) & 1),
                       satp_val, satp_val & 0xfffffffffffULL);
#endif
            static const char * const reg_names[] = {
                "zero","ra","sp","gp","tp","t0","t1","t2",
                "s0","s1","a0","a1","a2","a3","a4","a5",
                "a6","a7","s2","s3","s4","s5","s6","s7",
                "s8","s9","s10","s11","t3","t4","t5","t6",
            };
            for (int i = 1; i < 32; i += 4) {
                int end = i + 4 > 32 ? 32 : i + 4;
                if (end - i == 4)
                    KLOG_ERROR("  %s=0x%lx %s=0x%lx %s=0x%lx %s=0x%lx\n",
                               reg_names[i],   frame->x[i],
                               reg_names[i+1], frame->x[i+1],
                               reg_names[i+2], frame->x[i+2],
                               reg_names[i+3], frame->x[i+3]);
                else
                    for (int j = i; j < end; j++)
                        KLOG_ERROR("  %s=0x%lx\n", reg_names[j], frame->x[j]);
            }
        }
        
        platform_shutdown();
    }
}
