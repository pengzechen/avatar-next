/*
 * boot/x86_64/tss.h — x86_64 TSS（Task State Segment）声明
 */

#ifndef X86_64_TSS_H
#define X86_64_TSS_H

#include "types.h"

/* TSS 初始化（系统启动时调用一次） */
void x86_tss_init(void);
void x86_tss_init_cpu(uint32_t cpu_id, uint64_t rsp0);

/* 更新 TSS.RSP0（任务切换时更新内核栈）*/
void x86_tss_set_rsp0(uint64_t rsp0);

/*
 * 取**本核** TSS 的 GDT 选择子与线性基址。
 *
 * 给 VMCS 宿主区用的：VM-exit 时 CPU 会按 VMCS 里的 HOST_SEL_TR/HOST_BASE_TR
 * 装回宿主的 TR，那份值是 VMCS 创建时抓的一次性快照 —— 必须抓"本核"的。
 * 详见 vmx_refresh_host_state() 里的注释。
 */
void x86_tss_current(uint16_t *sel, uint64_t *base);

/*
 * 由 cpu_id 直接算 TSS 选择子（纯编码，不查 TR、不判"本核"）。
 *
 * 给不变量自检当**独立真值**用：x86_tss_current() 是写入路径，
 * 自检若拿它的返回值当期望，就等于自己证明自己。
 */
uint16_t x86_tss_sel_of_cpu(uint32_t cpu_id);

#endif /* X86_64_TSS_H */
