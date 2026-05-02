#ifndef X86_64_EXCEPTION_H
#define X86_64_EXCEPTION_H

/*
 * x86_64 专用异常/中断类型定义
 * 由 include/exception.h 在 ARCH_X86_64 时自动包含
 */

#include "types.h"

/* ── IDT 向量分配 ────────────────────────────────────────────────
 *
 *  0x00 – 0x1F : CPU 异常 (Intel 手册保留)
 *  0x20 – 0x2F : 外部中断 (IRQ 0-15，8259A PIC 映射或 I/O APIC)
 *  0x30 – 0xEF : 可用 (OS 自定义)
 *  0xF0 – 0xFF : LAPIC 本地中断
 *
 * 项目使用：
 *   0x20 = LAPIC Timer   (IRQ remapping 起始, 对应原 IRQ0)
 *   0xFF = LAPIC Spurious
 */
#define IDT_EXCEPTION_BASE    0x00u
#define IDT_IRQ_BASE          0x20u   /* 外部/本地中断起始向量         */
#define IDT_LAPIC_TIMER_VEC   0x20u   /* LAPIC timer 使用的 IDT 向量  */
#define IDT_LAPIC_SPURIOUS    0xFFu   /* LAPIC 伪中断向量              */
#define IDT_MAX_ENTRIES       256u

/* ── trap_frame ─────────────────────────────────────────────────
 *
 * exception.S 中的 ISR 存根在调用 C handler 前将寄存器压栈，
 * 布局（由高到低，RSP 指向底部）：
 *
 *   [CPU 硬件自动压栈]        SS / RSP / RFLAGS / CS / RIP
 *   [可选]                    error_code (无错误码异常压 0)
 *   [ISR 存根压栈]            vector_num
 *   [ISR 存根压栈]            r15..r8, rbp, rdi, rsi, rdx, rcx, rbx, rax
 */
typedef struct {
    /* ISR 存根保存的通用寄存器（push 顺序：rax 最后） */
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;
    /* ISR 存根填充的辅助字段 */
    uint64_t vector;      /* 中断向量号                     */
    uint64_t error_code;  /* 错误码（无则为 0）              */
    /* CPU 硬件自动压栈 */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} trap_frame_t;

/* ── IRQ handler typedef ────────────────────────────────────────
 * frame 实际类型是 trap_frame_t *
 */
typedef void (*irq_handler_t)(void *);

/* ── IDT 门描述符（64-bit Interrupt Gate）───────────────────────
 *
 * 63            48 47 46 45 44 43   40 39    32
 * [ offset[63:32] | P | DPL | 0 | type=0xE |  reserved  ]
 * 31            16 15                               0
 * [ selector      |             offset[15:0]        ]
 * offset[31:16] 在中间 64 位
 */
typedef struct __attribute__((packed)) {
    uint16_t offset_low;   /* offset[15:0]             */
    uint16_t selector;     /* code segment selector    */
    uint8_t  ist;          /* IST (Interrupt Stack Table), 0=disabled */
    uint8_t  type_attr;    /* P | DPL | 0 | type       */
    uint16_t offset_mid;   /* offset[31:16]            */
    uint32_t offset_high;  /* offset[63:32]            */
    uint32_t reserved;
} idt_entry_t;

/* type_attr 常用值：
 *   0x8E = P(1) | DPL(00) | 0 | type(1110) — kernel interrupt gate
 *   0xEF = P(1) | DPL(11) | 0 | type(1111) — user trap gate
 */
#define IDT_ATTR_KERNEL_INT  0x8Eu

/* ── IDTR 结构 ──────────────────────────────────────────────────*/
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idtr_t;

/* ── 声明 ────────────────────────────────────────────────────────*/

/* 初始化 IDT，安装默认存根，加载 IDTR，使能中断 */
void exception_init(void);

/* 设置单个 IDT 门（供 exception.c 内部使用） */
void idt_set_gate(uint8_t vec, void (*handler)(void), uint16_t sel, uint8_t attr);

#endif /* X86_64_EXCEPTION_H */
