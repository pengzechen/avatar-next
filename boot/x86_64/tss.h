/*
 * boot/x86_64/tss.h — x86_64 TSS（Task State Segment）声明
 */

#ifndef X86_64_TSS_H
#define X86_64_TSS_H

#include "types.h"

/* TSS 初始化（系统启动时调用一次） */
void x86_tss_init(void);

/* 更新 TSS.RSP0（任务切换时更新内核栈）*/
void x86_tss_set_rsp0(uint64_t rsp0);

/* 当前 TSS.RSP0（供汇编入口路径读取） */
extern uint64_t g_x86_tss_rsp0;

#endif /* X86_64_TSS_H */
