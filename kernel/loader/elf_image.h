/*
 * kernel/loader/elf_image.h — ELF 镜像加载接口
 *
 * 职责：ELF 格式校验 + PT_LOAD 段加载 + RELA 重定位
 * 不涉及进程创建、用户栈构建、内核映射复制或文件 I/O。
 */

#ifndef KERNEL_LOADER_ELF_IMAGE_H
#define KERNEL_LOADER_ELF_IMAGE_H

#include "types.h"

/*
 * ELF 镜像加载结果
 */
typedef struct {
    uint64_t entry_point;   /* 用户态入口点（已加 image_base） */
    uint64_t min_vaddr;     /* 已映射的用户虚拟地址下界 */
    uint64_t max_vaddr;     /* 已映射的用户虚拟地址上界（不含） */
    uint64_t phdr_uaddr;    /* 程序头在用户空间的虚拟地址（AT_PHDR auxv） */
    uint16_t phnum;         /* e_phnum */
    uint16_t phent;         /* e_phentsize */
} elf_image_info_t;

/**
 * elf_image_load - 将 ELF 镜像加载到已初始化的用户页表
 * @file_data: ELF 文件数据（内核虚拟地址）
 * @file_size: 文件大小
 * @pgd:       用户页表（内核虚拟地址），调用前内核映射已复制且页表已清零
 * @out:       加载结果输出
 *
 * 返回：0 成功，负值失败
 */
int elf_image_load(uint8_t *file_data, uint64_t file_size,
                   void *pgd, elf_image_info_t *out);

#endif /* KERNEL_LOADER_ELF_IMAGE_H */
