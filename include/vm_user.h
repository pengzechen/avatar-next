/*
 * include/vm_user.h - 用户进程虚拟内存管理（架构无关接口）
 */

#ifndef VM_USER_H
#define VM_USER_H

#include "types.h"

/**
 * vm_create_user_process - 为用户进程创建独立的地址空间
 * @user_code_start: 用户代码段起始（内核虚拟地址）
 * @user_code_size: 用户代码段大小（字节）
 * @user_stack_top: 用户栈顶虚拟地址
 * @user_stack_size: 用户栈大小（字节）
 *
 * 架构要求的用户 PGD 内核映射（RISC-V/x86 高半区、AArch64 无）由
 * mm_vm_init_user_pgd() 在内部注入；调用方不要再自己复制。
 *
 * 返回：页表基址（物理地址），失败返回 0
 */
uint64_t vm_create_user_process(uint64_t user_code_start,
                                uint64_t user_code_size,
                                uint64_t user_stack_top,
                                uint64_t user_stack_size);

/**
 * vm_destroy_user_process - 销毁用户进程页表
 * @pgd_phys: 页表基址（物理地址）
 *
 * 整棵页表、leaf 页和共享页引用由架构钩子 mm_vm_destroy_user_space()
 * 回收，包括 PGD 自身。
 */
void vm_destroy_user_process(uint64_t pgd_phys);

/**
 * vm_unmap_user_range - 解除用户虚拟地址范围映射并释放 leaf 物理页
 * @pgd_phys: 页表基址（物理地址）
 * @vaddr: 用户虚拟地址起始
 * @size: 解除映射的字节数
 *
 * 返回实际释放的 4KB 页数量。中间页表页保留，随进程销毁时统一释放。
 */
uint64_t vm_unmap_user_range(uint64_t pgd_phys, uint64_t vaddr, uint64_t size);

#endif /* VM_USER_H */
