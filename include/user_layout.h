#ifndef USER_LAYOUT_H
#define USER_LAYOUT_H

/*
 * include/user_layout.h — 用户地址空间布局常量
 *
 * 所有架构共用同一套用户虚拟地址布局：
 *
 *   0x00010000 ── 代码/数据（flat binary 或 PIE ELF 基址）
 *   0x30000000 ── mmap 分配区起始（ET_EXEC ELF）
 *   0x50000000 ── mmap 分配区起始（ET_DYN PIE / flat binary）
 *   0x6ff00000 ── 用户栈底（栈顶 - 1MB）
 *   0x70000000 ── 用户栈顶
 *
 * 修改此文件即可统一调整全内核的地址空间布局，
 * 无需逐一更改 task.c / exec.c / bin_loader.c / syscall.c 等。
 */

/* 代码段加载基址（flat binary 拷贝目标；PIE ELF image_base） */
#define USER_CODE_BASE      0x10000ULL

/* 用户栈 */
#define USER_STACK_TOP      0x70000000ULL   /* 栈顶（高地址端） */
#define USER_STACK_SIZE     0x100000ULL     /* 1 MB */

/* mmap 分配起始地址
 *   PIE / flat binary：代码紧贴低端，heap/mmap 放高位以留出空间
 *   ET_EXEC：ELF 自带虚拟地址，mmap 放在较低的空闲区
 */
#define USER_MMAP_BASE_PIE  0x50000000ULL   /* ET_DYN / flat binary */
#define USER_MMAP_BASE_EXEC 0x30000000ULL   /* ET_EXEC */

/* bin_loader 创建进程时的默认优先级 */
#define USER_PROCESS_PRIO   10u

#endif /* USER_LAYOUT_H */
