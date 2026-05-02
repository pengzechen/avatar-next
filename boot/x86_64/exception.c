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
 */
#define KERNEL_CS_SEL  0x10u

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
        /* 未注册的 CPU 异常：打印并挂起 */
        KLOG_ERROR("CPU exception #%llu at RIP=0x%llx EC=0x%llx\n",
                   vec, frame->rip, frame->error_code);
        while (1)
            __asm__ volatile("hlt");
    }
    /* 其余未注册中断静默忽略（包括 spurious）*/
}
