/*
 * boot/riscv64/exception.c — RISC-V 64-bit S-mode 异常/中断 C 分发器
 *
 * 提供：
 *   exception_init()   — 设置 stvec，使能 sstatus.SIE
 *   irq_install()      — 注册 scause 对应的中断处理函数
 *   handle_exception() — 从 exception.S 调用，分发陷阱
 */

#include "exception.h"
#include "klog.h"
#include "riscv64/sysreg.h"
#include "syscall/syscall.h"

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

void exception_init(void)
{
    /* trap_vector 在 exception.S 中定义（.align 4，直接模式） */
    extern void trap_vector(void);

    /* 写入 stvec，bits[1:0] = 00 表示 Direct 模式 */
    WRITE_STVEC((uint64_t)trap_vector);

    /*
     * 注意：不在此处使能 sstatus.SIE（内核态中断开关）。
     * 内核态（S-mode）始终保持 SIE=0，防止定时器中断在内核代码中随机触发，
     * 破坏 t0/t1 等临时寄存器（对齐 AArch64：daifclr 只在 task_trampoline_user 中调用）。
     * 定时器中断只在用户态（U-mode）生效：sret 通过 SPIE→SIE 自动使能。
     */

    /*
     * 使能 sstatus.SUM（Supervisor User Memory access）。
     * 置 1 后 S 模式可直接读写带 PTE_U 的用户页面，
     * 使 syscall 处理函数能直接解引用用户空间指针（如 write buf）。
     */
    CSR_SET(sstatus, 1UL << 18);   /* bit 18 = SUM */

    KLOG_INFO("RISC-V exception init: stvec=0x%lx, SUM enabled (SIE kept 0 in kernel)\n",
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

        if (irq < MAX_IRQ_CAUSES && interrupt_handlers[irq]) {
            interrupt_handlers[irq](frame_ptr);
        }
    } else {
        /* ── 同步异常路径 ────────────────────────────────────── */
        if (code == CAUSE_USER_ECALL) {
            /* RISC-V ecall: sepc 指向 ecall 本身，需手动前进到下一条 */
            frame->sepc += 4;
            syscall_handler(frame);
            return;
        }

        /* 其他异常：打印简单信息后挂起 */
        if (code == 12 || code == 13 || code == 15 ||
            code == 20 || code == 21 || code == 23) {
            const char *fault_type = (code == 12) ? "Inst" :
                                     (code == 13) ? "Load" :
                                     (code == 15) ? "Store" :
                                     (code == 20) ? "Inst-G" :
                                     (code == 21) ? "Load-G" : "Store-G";
            uint64_t hstatus_val = CSR_READ(hstatus);
            uint64_t sstatus_val = frame->sstatus;
            uint64_t satp_val    = CSR_READ(satp);
            KLOG_ERROR("%s PF: pc=0x%lx va=0x%lx sstatus=0x%lx(SPP=%u) hstatus=0x%lx(SPV=%u SPVP=%u) satp=0x%lx(PPN=0x%lx)\n",
                       fault_type, frame->sepc, frame->stval,
                       sstatus_val, (unsigned)((sstatus_val >> 8) & 1),
                       hstatus_val,
                       (unsigned)((hstatus_val >> 7) & 1),
                       (unsigned)((hstatus_val >> 8) & 1),
                       satp_val, satp_val & 0xfffffffffffULL);
        }
        
        do_platform_shutdown();
    }
}
