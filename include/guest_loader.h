/*
 * include/guest_loader.h — guest 镜像加载（内核 / DTB / initrd → guest 物理内存）
 *
 * 移植自 x-kernel: virt/kvmm-api/src/loader.rs
 *
 * 从内核 VFS（ext4 rootfs）读取 guest 镜像文件，按页翻译写入 guest 物理
 * 内存；并按需修补 DTB 的 initrd / memory 节点。
 *
 * guest 内存布局（依 imgs/…/linux.dts 的 chosen/reg 节点）：
 *   0x70000000 + 192 MiB   guest RAM
 *   0x70200000             kernel Image（2 MiB aligned ARM64 raw Image）
 *   0x74000000             DTB（4 KiB 内）
 *   0x78000000             initrd
 */
#ifndef GUEST_LOADER_H
#define GUEST_LOADER_H

#include "types.h"

/* guest 物理内存布局 */
#define GUEST_LINUX_MEM_BASE   0x70000000ULL
#define GUEST_LINUX_MEM_SIZE   0x0C000000ULL   /* 192 MiB，与 DTS 一致 */
#define GUEST_LINUX_KERNEL_GPA 0x70200000ULL
#define GUEST_LINUX_DTB_GPA    0x74000000ULL
#define GUEST_LINUX_INITRD_GPA 0x78000000ULL

/* guest 镜像在 rootfs 中的路径 */
#define GUEST_LINUX_KERNEL_PATH "/guests/linux/linux.bin"
#define GUEST_LINUX_DTB_PATH    "/guests/linux/linux.dtb"
#define GUEST_LINUX_INITRD_PATH "/guests/linux/initrd.gz"

/* vCPU 入口参数：guest 的 x0（ARM64 boot 约定 = DTB 物理地址）*/
extern volatile uint64_t g_guest_entry_x0;

/*
 * guest_loader_load_file — 把 rootfs 中的文件加载到 guest 物理地址 gpa
 * 返回加载字节数，失败返回负值。
 */
int guest_loader_load_file(const char *path, uint64_t gpa);

/*
 * guest_loader_patch_dtb_initrd / _memory — 修补 DTB 中的 initrd / memory 节点
 * 返回 0 成功。
 */
int guest_loader_patch_dtb_initrd(uint64_t dtb_gpa, uint32_t dtb_size,
                                  uint64_t initrd_start, uint64_t initrd_end);
int guest_loader_patch_dtb_memory(uint64_t dtb_gpa, uint32_t dtb_size,
                                  uint64_t mem_base, uint64_t mem_size);

/*
 * guest_loader_nop_dtb_nodes — 用 FDT_NOP 屏蔽未模拟的设备节点
 *
 * 必须屏蔽 DTB 中 VMM 未实现的设备（GICv2M / PCIe / PL061 …）：guest 访问
 * 它们会 Stage-2 fault，而 MMIO 总线未命中时按权限故障恢复 → guest 重试 →
 * 立刻再次 fault，形成死循环（表现为 guest 卡死在设备探测）。
 *
 * 返回被屏蔽的节点数。
 */
int guest_loader_nop_dtb_nodes(uint64_t dtb_gpa, uint32_t dtb_size,
                               const char *const *names, int nr_names);

/*
 * guest_loader_run_linux — 加载并启动 Linux guest（BSP 侧调用）
 * 返回 0 表示已成功建立 VM 并创建 vCPU 任务。
 */
int guest_loader_run_linux(void);

#endif /* GUEST_LOADER_H */
