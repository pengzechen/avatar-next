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
 * 返回：页表基址（物理地址），失败返回 0
 */
uint64_t vm_create_user_process(uint64_t user_code_start, uint64_t user_code_size,
                                uint64_t user_stack_top, uint64_t user_stack_size);

/**
 * vm_destroy_user_process - 销毁用户进程页表
 * @pgd_phys: 页表基址（物理地址）
 */
void vm_destroy_user_process(uint64_t pgd_phys);

#endif /* VM_USER_H */
