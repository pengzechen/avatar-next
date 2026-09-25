/*
 * include/guest_loader.h — guest 镜像加载（内核 / DTB / initrd → guest 物理内存）
 *
 * 移植自 x-kernel: virt/kvmm-api/src/loader.rs
 *
 * 从内核 VFS（ext4 rootfs）读取 guest 镜像文件，按页翻译写入 guest 物理
 * 内存；并按需修补 DTB 的 initrd / memory / bootargs 节点。
 *
 * ── guest 内存布局（两个架构各自一套常数）────────────────────────
 *
 *  AArch64（依 imgs/aarch64/linux.dts 的 chosen/reg 节点）：
 *      0x70000000 + 192 MiB   guest RAM
 *      0x70200000             kernel Image（2 MiB 对齐的 ARM64 raw Image）
 *      0x74000000             DTB（4 KiB 内）
 *      0x78000000             initrd
 *
 *  RISC-V 64（依 imgs/guests/rv64/linux.dts）：
 *      0xA0000000 + 192 MiB   guest RAM
 *      0xA0200000             kernel Image（2 MiB 对齐的 RISC-V raw Image）
 *      0xA4000000             DTB
 *      0xA8000000             initrd
 *
 *  RISC-V 的基址为什么是 0xA0000000 而不是 QEMU virt 惯用的 0x80000000：
 *  前者被宿主内核自己占了（kernel 0x80200000、rootfs 0x88000000..0x98000000），
 *  后者 0xC0000000 以上**没有**建直接映射（见 kernel/mm/riscv64/mmu.S，
 *  只覆盖 0..0xBFFFFFFF），phys_to_virt() 会缺页。0xA0000000..0xAC000000
 *  正好落在「已映射、且在 rootfs 之上」的那段空洞里，并在
 *  platforms/qemu-virt-riscv64/platform.conf 的 reserves 里预先占掉。
 *  偏移量与 x-kernel 一致（+2 MiB / +64 MiB / +128 MiB）。
 */
#ifndef GUEST_LOADER_H
#define GUEST_LOADER_H

#include "types.h"
#include "arch.h"

#if ARCH_AARCH64

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

#elif ARCH_RISCV64

/* guest 物理内存布局（见文件头「RISC-V 的基址为什么是 0xA0000000」）*/
#define GUEST_LINUX_MEM_BASE   0xA0000000ULL
#define GUEST_LINUX_MEM_SIZE   0x0C000000ULL   /* 192 MiB */
#define GUEST_LINUX_KERNEL_GPA 0xA0200000ULL   /* +2   MiB */
#define GUEST_LINUX_DTB_GPA    0xA4000000ULL   /* +64  MiB */
#define GUEST_LINUX_INITRD_GPA 0xA8000000ULL   /* +128 MiB */

#define GUEST_LINUX_KERNEL_PATH "/guests/rv64/linux.bin"
#define GUEST_LINUX_DTB_PATH    "/guests/rv64/linux.dtb"
#define GUEST_LINUX_INITRD_PATH "/guests/rv64/initrd.gz"

/* RISC-V boot 约定：a0 = boot hartid，a1 = DTB 物理地址（写在 loader 里）*/
#define GUEST_LINUX_BOOT_HARTID 0ULL

#endif /* ARCH_* */

/*
 * guest 必须从 DTB 里摘掉的、VMM 没有模拟的设备节点。
 *
 * 为什么必须屏蔽：VMM 只模拟控制台与中断控制器，其余设备一旦被 guest
 * 访问就会 G-stage/Stage-2 fault，而 exit handler 在 MMIO 总线未命中时
 * 按「权限故障」处理（恢复属性让 guest 重试）→ 立刻再次 fault → **死循环**，
 * 表现为 guest 卡死在探测那台设备上。启动前直接把这些节点从 DTB 中摘除
 * （替换为 FDT_NOP），guest 就根本不会去枚举它们。
 *
 * 移植自 kvmm loader.rs 的 nop_dtb_nodes。
 */
#if ARCH_AARCH64
/* VMM 只模拟了 PL011 与 GICD，DTB 里其余设备都没有实现。*/
#define GUEST_LINUX_UNSUPPORTED_NODES                                       \
    {  "v2m@8020000",           /* GICv2M MSI 帧（导致 GIC 初始化卡死）*/   \
       "virtio_mmio@a000000",   /* virtio-mmio transport（当前未模拟）*/    \
       "pcie@10000000",         /* PCIe ECAM */                            \
       "pl061@9030000",         /* GPIO */                                 \
       "pl031@9010000",         /* RTC */                                  \
       "flash@0",               /* CFI flash */                            \
       "fw-cfg@9020000" }       /* QEMU fw_cfg */
#elif ARCH_RISCV64
/* 只模拟了 16550A 与 PLIC；virtio-mmio 未实现（guest 也不该从它启动）。*/
#define GUEST_LINUX_UNSUPPORTED_NODES                                       \
    {  "virtio_mmio@a000000" }
#endif

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
 * guest_loader_patch_dtb_bootargs — 原地改写 /chosen/bootargs
 *
 * 原地改写、**不支持加长**：新串（含结尾 NUL）比 DTB 里现有的属性长就失败，
 * 并打一条 WARN。失败时 guest 会用 DTB 自带的那句命令行照常启动 —— 表现和
 * 成功一模一样，所以每个架构都配了一条 _Static_assert 把常见的手滑挡在
 * 编译期（见 kernel/vmm/guest_loader.c 顶部的 GUEST_LINUX_BOOTARGS）。
 *
 * 返回 0 成功。
 */
int guest_loader_patch_dtb_bootargs(uint64_t dtb_gpa, uint32_t dtb_size,
                                    const char *bootargs);

/*
 * guest_loader_nop_dtb_nodes — 用 FDT_NOP 屏蔽未模拟的设备节点
 *
 * 参见上面 GUEST_LINUX_UNSUPPORTED_NODES 的说明。
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
