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
    uint64_t image_base;    /* 实际使用的加载基址（load bias） */
    uint16_t phnum;         /* e_phnum */
    uint16_t phent;         /* e_phentsize */
    char     interp_path[128]; /* PT_INTERP 路径，无则为空字符串 */
} elf_image_info_t;

/**
 * elf_image_load - 将 ELF 镜像加载到已初始化的用户页表
 * 自动选择加载基址（ET_DYN → USER_CODE_BASE，ET_EXEC → 0）。
 * 兼容静态链接。
 */
int elf_image_load(uint8_t *file_data, uint64_t file_size,
                   void *pgd, elf_image_info_t *out);

/**
 * elf_image_load_at - 将 ELF 镜像加载到指定基址
 * 用于加载动态链接器（interpreter）到 USER_INTERP_BASE 等独立地址。
 * @force_base: 强制 image_base，必须页对齐且非零
 */
int elf_image_load_at(uint8_t *file_data, uint64_t file_size,
                      void *pgd, uint64_t force_base, elf_image_info_t *out);

#endif /* KERNEL_LOADER_ELF_IMAGE_H */
