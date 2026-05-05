/*
 * boot/x86_64/exception.c — x86_64 IDT 初始化与中断分发
 *
 * 提供：
 *   exception_init()    — 建立 IDT，加载 IDTR，初始化 LAPIC，sti
 *   irq_install()       — 注册向量对应的 C handler
 *   handle_exception()  — 由 exception.S 调用，分发到已注册的 handler
 *   idt_set_gate()      — 填写单个 IDT 门描述符
 */

#include "exception.h"
#include "klog.h"
#include "irq/lapic.h"
#include "x86_64/io.h"      /* outb — 用于屏蔽 8259A PIC */
#include "task/task.h"
#include "task/sched.h"
#include "mm_vm.h"

/* ── IDT 表 ─────────────────────────────────────────────────── */

static idt_entry_t idt[IDT_MAX_ENTRIES] __attribute__((aligned(16)));
static idtr_t      idtr;

/* ── 中断处理函数表（256 个向量）──────────────────────────────── */

static irq_handler_t irq_handlers[IDT_MAX_ENTRIES];

/* ── ISR 存根地址表（来自 exception.S）────────────────────────── */

extern void *isr_stub_table[];  /* 前 33 个 stub 地址 */

/* 各存根的 extern 声明 */
extern void isr_stub_32(void);   /* LAPIC Timer                    */
extern void isr_stub_255(void);  /* LAPIC Spurious                 */

/* ── 代码段选择子（来自 boot.S GDT）────────────────────────────
 * boot_gdt: 0x00=null, 0x08=code32, 0x10=code64, 0x18=data
 * 
 * 用户段选择子（需要在 GDT 中添加）：
 *   0x20: 用户数据段（基于 0x18 + RPL=3）
 *   0x28: 用户代码段（基于 0x10 + RPL=3）
 * 
 * 注意：SYSRET 指令要求用户段布局：
 *   用户代码段 = MSR_STAR[63:48] + 16
 *   用户数据段 = MSR_STAR[63:48] + 8
 */
#define KERNEL_CS_SEL  0x10u
#define KERNEL_DS_SEL  0x18u
#define USER_DS_SEL    0x20u  /* RPL=0，SYSRET 会自动加 3 */
#define USER_CS_SEL    0x28u  /* RPL=0，SYSRET 会自动加 3 */

/* ── MSR 寄存器地址 ───────────────────────────────────────────── */
#define MSR_STAR    0xC0000081  /* SYSCALL 段选择子 */
#define MSR_LSTAR   0xC0000082  /* SYSCALL 入口地址 */
#define MSR_CSTAR   0xC0000083  /* 兼容模式（32位）入口（未使用） */
#define MSR_SFMASK  0xC0000084  /* SYSCALL RFLAGS 掩码 */

/* ── MSR 读写辅助函数 ───────────────────────────────────────────── */

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/* ── idt_set_gate ───────────────────────────────────────────── */

void idt_set_gate(uint8_t vec, void (*handler)(void), uint16_t sel, uint8_t attr)
{
    uint64_t base = (uint64_t)handler;
    idt[vec].offset_low  = (uint16_t)(base & 0xFFFFu);
    idt[vec].selector    = sel;
    idt[vec].ist         = 0;
    idt[vec].type_attr   = attr;
    idt[vec].offset_mid  = (uint16_t)((base >> 16) & 0xFFFFu);
    idt[vec].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFFu);
    idt[vec].reserved    = 0;
}

/* ── irq_install ────────────────────────────────────────────── */

void irq_install(int vector, irq_handler_t h)
{
    if (vector >= 0 && vector < (int)IDT_MAX_ENTRIES)
        irq_handlers[vector] = h;
}

/* ── exception_init ─────────────────────────────────────────── */

void exception_init(void)
{
    /* 安装前 32 个 CPU 异常存根（来自 isr_stub_table）*/
    for (int i = 0; i < 32; i++) {
        void (*stub)(void) = (void (*)(void))isr_stub_table[i];
        idt_set_gate((uint8_t)i, stub, KERNEL_CS_SEL, IDT_ATTR_KERNEL_INT);
    }

    /* 安装向量 32（LAPIC Timer）*/
    idt_set_gate(IDT_LAPIC_TIMER_VEC, isr_stub_32,  KERNEL_CS_SEL, IDT_ATTR_KERNEL_INT);

    /* 安装向量 255（LAPIC Spurious）*/
    idt_set_gate(IDT_LAPIC_SPURIOUS,  isr_stub_255, KERNEL_CS_SEL, IDT_ATTR_KERNEL_INT);

    /* 加载 IDTR */
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt[0];
    __asm__ volatile("lidt %0" :: "m"(idtr));

    KLOG_INFO("IDT loaded: base=0x%lx limit=%u\n",
              idtr.base, (uint32_t)idtr.limit + 1u);

    /*
     * 配置 SYSCALL/SYSRET MSR 寄存器（用户态支持）
     */
    extern void syscall_entry(void);  /* syscall_wrapper.S */
    
    /* MSR_STAR: 配置段选择子
     * [31:0]   保留
     * [47:32]  SYSCALL 内核 CS/SS （CS=值, SS=值+8）
     * [63:48]  SYSRET 用户 CS/SS （CS=值+16, SS=值+8） */
    uint64_t star = ((uint64_t)KERNEL_CS_SEL << 32) | ((uint64_t)(USER_CS_SEL - 16) << 48);
    wrmsr(MSR_STAR, star);
    
    /* MSR_LSTAR: SYSCALL 入口地址 */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    
    /* MSR_SFMASK: SYSCALL 时清除的 RFLAGS 位（关中断：IF=0x200） */
    wrmsr(MSR_SFMASK, 0x200);  /* 清除 IF（中断标志） */
    
    KLOG_INFO("SYSCALL/SYSRET configured: entry=0x%lx\n", (uint64_t)syscall_entry);

    /*
     * 屏蔽 8259A PIC（legacy），我们使用 LAPIC。
     * 默认情况下 BIOS/QEMU 将 IRQ0 映射到 INT 8（Double Fault 向量），
     * 如果不屏蔽，sti 后 PIC timer 会触发 #DF。
     */
    outb(0x21, 0xFF);   /* master PIC — 屏蔽所有 IRQ */
    outb(0xA1, 0xFF);   /* slave  PIC — 屏蔽所有 IRQ */

    /* 初始化 LAPIC（软件使能 + SVR）*/
    lapic_init();

    /* 使能 CPU 中断 */
    __asm__ volatile("sti");

    KLOG_INFO("x86_64 exception init complete, interrupts enabled\n");
}

/* ── handle_exception ───────────────────────────────────────── */

void handle_exception(void *frame_ptr)
{
    trap_frame_t *frame = (trap_frame_t *)frame_ptr;
    uint64_t vec = frame->vector;

    if (vec < IDT_MAX_ENTRIES && irq_handlers[vec]) {
        irq_handlers[vec](frame_ptr);
    } else if (vec < 32) {
        bool from_user = ((frame->cs & 0x3ULL) == 0x3ULL);

        if (from_user) {
            task_t *cur = task_current();
            KLOG_ERROR("User exception #%llu in task '%s' (id=%u) at RIP=0x%llx EC=0x%llx, terminating task\n",
                       vec,
                       cur ? cur->name : "<null>",
                       cur ? cur->id : 0,
                       frame->rip,
                       frame->error_code);

            if (vec == 14) {
                uint64_t cr2;
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
                KLOG_ERROR("  User PF CR2=0x%llx, bits: P=%d W=%d U=%d R=%d I=%d\n",
                           cr2,
                           (int)(frame->error_code & 1),
                           (int)((frame->error_code >> 1) & 1),
                           (int)((frame->error_code >> 2) & 1),
                           (int)((frame->error_code >> 3) & 1),
                           (int)((frame->error_code >> 4) & 1));

                KLOG_ERROR("  User regs: RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx\n",
                           frame->rax, frame->rbx, frame->rcx, frame->rdx);
                KLOG_ERROR("             RSI=0x%llx RDI=0x%llx RBP=0x%llx RSP=0x%llx\n",
                           frame->rsi, frame->rdi, frame->rbp, frame->rsp);

                if (cur && cur->pgd) {
                    uint64_t rip_page = frame->rip & ~0xfffULL;
                    uint64_t rip_pa = mm_vm_get_paddr((void *)phys_to_virt((uint64_t)cur->pgd), rip_page);
                    if (rip_pa) {
                        uint8_t *p = (uint8_t *)phys_to_virt(rip_pa + (frame->rip & 0xfffULL));
                        KLOG_ERROR("  RIP bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                   p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                    }
                }
            }

            /*
             * 关键：不要在异常 C 处理函数内直接 task_exit()/sched_schedule()。
             * 调度应在 exception.S 的返回路径（sched_check_and_yield）统一执行，
             * 避免破坏异常返回现场。
             */
            if (cur) {
                cur->state = TASK_DEAD;
                sched_dequeue(cur);
            }

            /* 请求在异常返回路径执行重调度 */
            sched_tick();
            return;
        }

        /* 未注册的 CPU 异常：打印详细信息 */
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        
        KLOG_ERROR("CPU exception #%llu at RIP=0x%llx EC=0x%llx\n",
                   vec, frame->rip, frame->error_code);
        KLOG_ERROR("  RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx\n",
                   frame->rax, frame->rbx, frame->rcx, frame->rdx);
        KLOG_ERROR("  RSI=0x%llx RDI=0x%llx RBP=0x%llx RSP=0x%llx\n",
                   frame->rsi, frame->rdi, frame->rbp, frame->rsp);
        KLOG_ERROR("  CS=0x%llx SS=0x%llx RFLAGS=0x%llx\n",
                   frame->cs, frame->ss, frame->rflags);
        KLOG_ERROR("  CR3=0x%llx\n", cr3);
        
        if (vec == 14) {
            /* Page Fault: 打印 CR2（出错地址） */
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            KLOG_ERROR("  CR2 (fault addr) = 0x%llx\n", cr2);
            KLOG_ERROR("  Error code bits: P=%d W=%d U=%d R=%d I=%d\n",
                       (int)(frame->error_code & 1),
                       (int)((frame->error_code >> 1) & 1),
                       (int)((frame->error_code >> 2) & 1),
                       (int)((frame->error_code >> 3) & 1),
                       (int)((frame->error_code >> 4) & 1));
        }
        
        while (1)
            __asm__ volatile("hlt");
    }
    /* 其余未注册中断静默忽略（包括 spurious）*/
}
