/*
 * boot/x86_64/tss.c — x86_64 Task State Segment (TSS) 初始化
 *
 * TSS 在 x86_64 中用于：
 *   1. 特权级切换时加载内核栈（RSP0/RSP1/RSP2）
 *   2. 中断栈表（IST）用于关键异常处理
 *
 * 本实现设置基本的 TSS，提供 RSP0（Ring 0 栈指针）。
 */

#include "types.h"
#include "klog.h"
#include "string.h"

/* ── TSS 结构定义（x86_64 格式）─────────────────────────────────── */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;          /* Ring 0 栈指针 */
    uint64_t rsp1;          /* Ring 1 栈指针（未使用）*/
    uint64_t rsp2;          /* Ring 2 栈指针（未使用）*/
    uint64_t reserved1;
    uint64_t ist[7];        /* 中断栈表 IST1-IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    /* I/O 权限位图基址（未使用则设为 sizeof(tss)）*/
} __attribute__((packed)) tss_t;

/* ── 内核栈（静态分配 16KB，必须在 TSS 之前定义以避免地址冲突）─── */
static uint8_t g_kernel_stack[16384] __attribute__((aligned(16)));

/* ── 全局 TSS 实例（静态分配）───────────────────────────────────── */
static tss_t g_tss __attribute__((aligned(16)));

/* 对外导出的当前 RSP0（供汇编路径读取） */
uint64_t g_x86_tss_rsp0 = 0;

/* ── TSS 描述符结构（16 字节，GDT 中占两个 slot）───────────────── */
static void
tss_set_gdt_entry(uint64_t *gdt, uint64_t base, uint32_t limit)
{
    uint64_t tss_base = base;
    
    /* 低 8 字节：TSS 描述符（Type=0x9: Available TSS）*/
    gdt[0] = (limit & 0xFFFF)                   |  /* Limit 15:0 */
             ((tss_base & 0xFFFF) << 16)        |  /* Base 15:0 */
             (((tss_base >> 16) & 0xFF) << 32)  |  /* Base 23:16 */
             (0x89ULL << 40)                    |  /* Type=9, P=1, DPL=0 */
             ((((limit >> 16) & 0xF) | 0) << 48) | /* Limit 19:16, G=0, AVL=0 */
             (((tss_base >> 24) & 0xFF) << 56);    /* Base 31:24 */
    
    /* 高 8 字节：Base 63:32 + 保留 */
    gdt[1] = (tss_base >> 32);
}

/* ── TSS 初始化────────────────────────────────────────────────────── */
void
x86_tss_init(void)
{
    /* 清零 TSS */
    memset(&g_tss, 0, sizeof(g_tss));
    
    /* 设置 RSP0（Ring 0 栈顶）*/
    g_tss.rsp0 = (uint64_t)g_kernel_stack + sizeof(g_kernel_stack);
    g_x86_tss_rsp0 = g_tss.rsp0;
    
    /* I/O 位图基址设为超出 TSS 末尾（禁用 I/O 权限检查）*/
    g_tss.iomap_base = sizeof(tss_t);
    
    KLOG_INFO("TSS setup: g_kernel_stack=0x%llx size=%lu top=0x%llx\n",
              (uint64_t)g_kernel_stack, sizeof(g_kernel_stack), 
              (uint64_t)g_kernel_stack + sizeof(g_kernel_stack));
    KLOG_INFO("TSS setup: &g_tss=0x%llx g_tss.rsp0=0x%llx\n",
              (uint64_t)&g_tss, g_tss.rsp0);
    
    /* 获取当前 GDT 基址 */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdt_desc;
    __asm__ volatile("sgdt %0" : "=m"(gdt_desc));
    uint64_t *gdt = (uint64_t *)gdt_desc.base;
    
    /* 在 GDT[6:7] 添加 TSS 描述符（16 字节，占两个 slot）*/
    tss_set_gdt_entry(&gdt[6], (uint64_t)(uintptr_t)&g_tss, sizeof(tss_t) - 1);
    
    /* 加载 TR（Task Register）指向 GDT[6]（选择子 0x30）*/
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)0x30));
    
    KLOG_INFO("TSS initialized: base=0x%llx RSP0=0x%llx TR=0x30\n", 
              (uint64_t)&g_tss, g_tss.rsp0);
}

/* ── 更新 TSS.RSP0（任务切换时调用）──────────────────────────────── */
void
x86_tss_set_rsp0(uint64_t rsp0)
{
    g_tss.rsp0 = rsp0;
    g_x86_tss_rsp0 = rsp0;
}
