/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/guest_loader.h — guest 镜像加载(内核 / DTB / initrd → guest 物理内存)

   移植自 x-kernel: virt/kvmm-api/src/loader.rs

   从内核 VFS(ext4 rootfs)读取 guest 镜像文件, 按页翻译写入 guest 物理
   内存；并按需修补 DTB 的 initrd / memory / bootargs 节点。

   ── guest 内存布局(两个架构各自一套常数)────────────────────────

    AArch64(依 imgs/guests/aarch64/linux.dts 的 chosen/reg 节点)：
        0x70000000 + 192 MiB   guest RAM
        0x70200000             kernel Image(2 MiB 对齐的 ARM64 raw Image)
        0x74000000             DTB(4 KiB 内)
        0x78000000             initrd

    RISC-V 64(依 imgs/guests/rv64/linux.dts)：
        0xA0000000 + 192 MiB   guest RAM
        0xA0200000             kernel Image(2 MiB 对齐的 RISC-V raw Image)
        0xA4000000             DTB
        0xA8000000             initrd

    RISC-V 的基址为什么是 0xA0000000 而不是 QEMU virt 惯用的 0x80000000：
    后者被宿主内核自己占了(kernel 0x80200000, rootfs 0x88000000..0x98000000)。
    0xA0000000 是「在 rootfs 之上, 又避开宿主自己」的一段空洞, 偏移量与
    x-kernel 一致(+2 MiB / +64 MiB / +128 MiB)。

    注意： 这个地址是 guest 眼里的 GPA —— 从 G-stage 改成按需分页起, 它不再
    对应任何一段宿主物理内存：guest 的每一页都是缺页时从 PMM 现拿的, 落到
    哪里由 PMM 决定(aarch64 那边同理, 它的 GPA 基址是 0x70000000)。
    所以 platform.conf 里那段 guest_ram 预留也一并删掉了。
    历史注记：早期实现是 identity 映射(GPA == HPA), 基址才必须是"宿主上
    真实存在且已直接映射的一段", 见 include/riscv64/gstage.h 的说明。
*/
#ifndef GUEST_LOADER_H
#define GUEST_LOADER_H

#include "page.h"
#include "types.h"
#include "arch.h"

/*
   guest 的默认 vCPU 数量。由 Makefile 的 GUEST_VCPUS= 经
   -DGUEST_LINUX_NR_VCPUS=N 传入；直接编某个文件时缺省为 1。

   注意： 它只是“调用方没有指定数量时的默认值”, 不再是运行期权威：
     - /dev/vmm 的 VMM_IOC_BOOT_CFG(vmm-run -c N)会把它替换成调用方给的值；
     - 直启路径(main.c)仍用它, 所以 `make ... GUEST_VCPUS=N GUEST_LINUX=1`
       的行为不变；
     - guest_loader_run_linux() 里 vm->cfg.nr_vcpus <= 0 时才回退到它。

   运行期支持的数量与 rootfs 里安装的 DTB 一一对应：
     1 → linux.dtb(构建默认就是单核时)/ linux-1cpu.dtb(构建默认是多核时)
     2/4/8/12 → linux-gicv3-smpN.dtb
   数量与 DTB 的 cpu@N 节点数不一致时, guest 会去 CPU_ON 一个 VMM 根本没准备
   的 vCPU —— 所以 guest_loader_check_vcpus() 会在启动前同时校验数量和 DTB。
*/
#ifndef GUEST_LINUX_NR_VCPUS
#define GUEST_LINUX_NR_VCPUS 1
#endif

#if ARCH_AARCH64

/* guest 物理内存布局 */
#define GUEST_LINUX_MEM_BASE   0x70000000ULL
#define GUEST_LINUX_MEM_SIZE   0x0C000000ULL /* 192 MiB, 与 DTS 一致 */
#define GUEST_LINUX_KERNEL_GPA 0x70200000ULL
#define GUEST_LINUX_DTB_GPA    0x74000000ULL
/*
   initrd 落点。上下都挨着别人的地盘, 所以只有 16 MiB 可用：

     0x76000000 ┌──────────────────────────┐
                │ CMA(32 MiB, 内核自己挑的)│  ← DTB 里没有 linux,cma,
     0x78000000 ├──────────────────────────┤     是 dma_contiguous_reserve()
                │ initrd(16 MiB 上限)      │     找的洞, 正好落在这里
     0x79000000 ├──────────────────────────┤
                │ NPU 恒等窗口(48 MiB)     │
     0x7C000000 └──────────────────────────┘

   往上越过 0x79000000 不会报错 —— initrd 的页会直接把 NPU 窗口那段 IPA
   覆盖掉, 之后 guest 驱动拿到的"物理地址"在 NPU 眼里是 initramfs 的内容。
   往下则是撞进 CMA。两边都不安全, 所以只有 16 MiB。

   挡越界的是 guest_loader.c 里那个加载前的大小检查(不能放到加载后,
   那时页已经映射进去了)。带 NPU 的 initramfs 现在 9.2 MiB(gzip 后),
   余量 7 MiB。

   注意： 想把 initrd 做大, 得先在 guest DTS 里把 linux,cma 显式钉在一个
      确定的位置, 再把这里挪过去 —— 不能只改这一个常量。否则 CMA 会被
      挤到别处, 或者和 initrd 抢同一段内存, 而两者都不报错。
*/
#define GUEST_LINUX_INITRD_GPA 0x78000000ULL

/*
   ── NPU DMA 恒等窗口(IPA)───────────────────────────────────────────────

   RK3588 真机上把 NPU 透传给 guest 时, 这块内存必须 IPA == PA：
   NPU 是没有 IOMMU 的物理设备, 只认物理地址, 而 guest 的普通 RAM 是逐页
   从 PMM 现分配的(物理不连续, PA 与 IPA 无关)。详见
   include/aarch64/stage2.h 里 s2_ctx_t.npu_dma_base 的说明。

   三处必须一致, 缺一个 NPU 就会写到随机的宿主页：
     1. guest DTS 的 reserved-memory(NPU 节点的 memory-region 指向它)
     2. 这里 —— VMM 用它建立 stage-2 的恒等映射
     3. platforms/rk3588-aarch64/platform.conf 的 reserves —— 把同一段
        物理内存从宿主 PMM 里摘出来, 否则宿主会把页分给别人

   注意： 只在 RK3588 上启用。QEMU 上没有 NPU, 而且宿主 PA 0x79000000 根本不是
   RAM —— 设了反而会把 guest 的正常访问映射到不存在的物理内存上。
*/
#if PLATFORM_RK3588
#define GUEST_LINUX_NPU_DMA_GPA  0x79000000ULL /* guest RAM 顶部 48 MiB */
#define GUEST_LINUX_NPU_DMA_SIZE 0x03000000ULL
#else
#define GUEST_LINUX_NPU_DMA_GPA  0ULL
#define GUEST_LINUX_NPU_DMA_SIZE 0ULL
#endif

/*
   initrd 的上界(不含)：从 GUEST_LINUX_INITRD_GPA 起还能用多少。

   取「下一个被固定占用的 guest 地址」而不是 RAM 末尾 —— 有 NPU 时那个地址
   是恒等窗口的起点, initrd 越过它就会把窗口的 IPA 覆盖掉(不报错, 见
   GUEST_LINUX_INITRD_GPA 的注释)。没有 NPU 时窗口大小为 0, 上界自然落到
   RAM 末尾。

   注：下界那边(CMA)不用管 —— CMA 在 initrd 下方, initrd 只会往上长。
   但那也正是 initrd 不能再往下挪的原因, 见 GUEST_LINUX_INITRD_GPA。

   注意： 放在 NPU 那两个宏之后：它依赖它们。挪到前面会静默拿到全 0,
      于是每次启动都在"initrd 太大"上失败 —— 而且看起来像镜像坏了。
*/
#define GUEST_LINUX_INITRD_LIMIT \
    ((GUEST_LINUX_NPU_DMA_GPA) ? (GUEST_LINUX_NPU_DMA_GPA) : (GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE))

/* guest 镜像在 rootfs 中的路径 */
#define GUEST_LINUX_KERNEL_PATH "/guests/linux/linux.bin"
#define GUEST_LINUX_DTB_PATH    "/guests/linux/linux.dtb"
/*
   单核 DTB 的固定路径。

   linux.dtb 是“构建默认”那一份：GUEST_VCPUS=8 的构建里它其实是 8 核 DTB,
   不能拿来跑 `vmm-run -c 1`(guest 会去 CPU_ON 1..7)。所以 rootfs 另外
   安装一份真正的单核 DTB 到这里, 运行期 N==1 而构建默认 !=1 时用它。
*/
#define GUEST_LINUX_DTB_PATH_1CPU "/guests/linux/linux-1cpu.dtb"
#define GUEST_LINUX_INITRD_PATH   "/guests/linux/initrd.gz"

/* vCPU 入口参数：guest 的 x0(ARM64 boot 约定 = DTB 物理地址) */
extern volatile uint64_t g_guest_entry_x0;

#elif ARCH_X86_64
/*
   ── x86_64 guest 物理布局(GPA)─────────────────────────────

   与 arm64/riscv 不同, x86 的 guest 物理空间必须从 0 开始：
   Linux 假定低端有 640KB 常规内存, 1MB 起是内核, e820 也是这么报的。

   所以这里出现了一个 arm64/riscv 没有的概念：GPA ≠ HPA。
   guest 的物理 0 当然不能映射到宿主的物理 0, 由 EPT 负责翻译。
   所有往 guest 内存写数据的地方都要过 `gpa_ptr(vm, ...)`
   (kernel/vmm/x86_64/guest_boot.c)—— 直接拿 GPA 去 phys_to_virt 会写到
   宿主的低端物理内存上, 后果是宿主当场崩。

   注意： 另有一条同源的约束：不要对 `guest_loader_gpa_ptr()` 的返回值做跨页的
   指针算术。identity 时代 `phys_to_virt(gpa)` 是线性的, `p + off` 恰好就是
   "gpa + off"；改成按需分页后每个 guest 页各自分配, 物理不连续, 那句写法会
   走到宿主的物理下一页上, 而且没有任何报错。跨页一律用
   guest_loader_write_guest() / read_guest() / fill_guest() 那一族。

   历史注记：早期实现是"预占宿主一段固定窗口(GUEST_X86_HPA_BASE)+ 线性
   偏移", platform.conf 里还有对应的 192 MiB 预留。按需分页之后两者都删了。
*/
#define GUEST_LINUX_MEM_BASE       0x00000000ULL /* guest 物理 0(不是宿主物理 0) */
#define GUEST_LINUX_MEM_SIZE       0x0C000000ULL /* 192 MiB */

/* 宿主侧承载 guest RAM 的物理窗口(见 platforms/qemu-virt-x86_64/platform.conf
   的 reserves：必须在这里预留, 否则中间的宿主分配会落进窗口被 guest 踩掉)。
*/
#define GUEST_X86_HPA_BASE         0x20000000ULL /* 512 MiB 处, 192 MiB */

/* guest 内部各块的 GPA */
#define GUEST_LINUX_SETUP_GPA      0x00010000ULL /* bzImage 引导扇区 + setup */
#define GUEST_LINUX_KERNEL_GPA     0x00100000ULL /* 保护模式内核(pref_address 为 0 时) */
#define GUEST_LINUX_BOOTPARAMS_GPA 0x00070000ULL /* zero page(boot_params) */
#define GUEST_LINUX_CMDLINE_GPA    0x00080000ULL /* 命令行 */
#define GUEST_LINUX_PGTBL_GPA      0x00090000ULL /* 临时页表(12 KiB) */
#define GUEST_LINUX_GDT_GPA        0x00098000ULL /* 临时 GDT */
#define GUEST_LINUX_IDT_GPA        0x00099000ULL /* 空 IDT(全 0) */
#define GUEST_LINUX_TSS_GPA        0x0009A000ULL /* 空 TSS(全 0) */
#define GUEST_LINUX_PROBE_GPA      0x0009B000ULL /* 入口探针 stub */
/*
   早期栈。注意： 必须放在远离内核装载区的高处：
   内核装在 16 MiB, 解压后占 16~41 MiB, 解压器/重定位还会用到周围的内存。
   这里起初写成 0x00BFF000(12.5 MiB, 注释还写着"顶部附近"——少了一个 0),
   结果初始栈正好落在内核要反复使用的低内存区里被覆盖, 症状是 guest 栈上
   全是垃圾, 随后跳到镜像之外执行, 再读一个空指针 (#PF cr2=0)。
   放到 190 MiB(RAM 是 192 MiB, initrd 在 96 MiB)就安全了。

   注意： 另一处同类陷阱：引导产物必须避开「setup 堆」区间。
   引导协议规定(boot.rst)：loadflags.CAN_USE_HEAP 置位时, 解压器的堆是
       [0x10000 + heap_end_ptr + 0x200 , 0x9FF00]
   我先把 heap_end_ptr 设成 0x7E00 ⇒ 堆 = [0x18000, 0x9FF00]。
   而原先 boot_params(0x70000)/cmdline(0x80000)/页表(0x90000)/GDT(0x98000)/
   IDT(0x99000)/TSS(0x9A000) 全都落在这个区间里, 会被解压器的分配覆盖 ——
   一旦 boot_params 或页表被冲掉, 内核读到的就是垃圾指针(实测表现正是
   「取到空指针后往地址 0 拷」)。
   现在统一放到 0x1000–0x9000(低于 setup 代码 0x10000, 也在堆区间之外)。
*/
#define GUEST_LINUX_STACK_GPA      0x0BE00000ULL /* 190 MiB */
#define GUEST_LINUX_STACK_SIZE     0x1000ULL
#define GUEST_LINUX_INITRD_GPA     0x06000000ULL /* 96 MiB 处 */
/* initrd 上界：本架构没有"后面还要用"的固定区域, 所以就是 RAM 末尾。
   为什么要有这个上界, 以及 aarch64 上它为什么不是 RAM 末尾, 见该架构块里
   GUEST_LINUX_INITRD_LIMIT 的说明(guest_loader.c 用它做加载前检查)。
*/
#define GUEST_LINUX_INITRD_LIMIT   (GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE)

#define GUEST_LINUX_KERNEL_PATH "/guests/x86_64/bzImage"
#define GUEST_LINUX_INITRD_PATH "/guests/x86_64/initrd"

/*
   命令行(tgoskits 的 x86 Linux 配置同款, 逐项都有原因)：
     console=ttyS0                guest 控制台走 COM1(我们模拟的 PIO 16550)
     earlycon=uart8250,io,0x3f8 earlyprintk=serial,ttyS0,115200   早期控制台也走 PIO, 否则 boot 前半段无输出
     rdinit=/init                 initramfs 里直接跑 /init
     nox2apic                     本 VMM 不做 x2APIC, 正好与实现一致
     no_timer_check               跳过对 PIT/LAPIC 的时钟校验(桩设备不产生中断)
     irqpoll                      没有外部设备中断时的兜底轮询
     pci=conf1 pci=nomsi          我们没做 PCI 配置空间(桩返回全 1)

   注意： 不要再加 `tsc=unstable`：guest 的 TSC 就是宿主的 TSC(嵌套 VMX,
      我们没写 TSC_OFFSET), 而 CPUID 0x15/0x16 是透传宿主真值的,
      频率精确。标成 unstable 反而会让内核弃用 TSC, 回头去指望
      PIT/HPET 这些我们没实现成时钟的桩设备。
*/
#define GUEST_LINUX_BOOTARGS \
    "quiet console=ttyS0 earlycon=uart8250,io,0x3f8 earlyprintk=serial,ttyS0,115200 " \
    "rdinit=/init ibt=off " \
    "no_timer_check pci=conf1 pci=nomsi " \
    "lapic panic_on_warn=0 oops=panic "

/* x86 没有 DTB, 也不需要摘节点 */
#define GUEST_LINUX_UNSUPPORTED_NODES { "", NULL }

#elif ARCH_RISCV64

/* guest 物理内存布局(见文件头「RISC-V 的基址为什么是 0xA0000000」) */
#define GUEST_LINUX_MEM_BASE     0xA0000000ULL
#define GUEST_LINUX_MEM_SIZE     0x0C000000ULL /* 192 MiB */
#define GUEST_LINUX_KERNEL_GPA   0xA0200000ULL /* +2   MiB */
#define GUEST_LINUX_DTB_GPA      0xA4000000ULL /* +64  MiB */
#define GUEST_LINUX_INITRD_GPA   0xA8000000ULL /* +128 MiB */
/* initrd 上界 —— 同 x86_64：本架构后面没有固定区域, 就是 RAM 末尾。
   aarch64 那份为什么不是 RAM 末尾, 见该架构块的 GUEST_LINUX_INITRD_LIMIT。
*/
#define GUEST_LINUX_INITRD_LIMIT (GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE)

#define GUEST_LINUX_KERNEL_PATH "/guests/rv64/linux.bin"
#define GUEST_LINUX_DTB_PATH    "/guests/rv64/linux.dtb"
#define GUEST_LINUX_INITRD_PATH "/guests/rv64/initrd.gz"

/* RISC-V boot 约定：a0 = boot hartid, a1 = DTB 物理地址(写在 loader 里) */
#define GUEST_LINUX_BOOT_HARTID 0ULL

#endif /* ARCH_* */

/*
   aarch64 / riscv 的 guest 初始 SP：RAM 窗口最后一页的起始地址
   (与历史实现一致；栈向下增长)。这两个架构的 loader 都用它填
   sp_el1 / a2, 不再各自写 `... - 0x1000`。
   (x86 的初始栈由 boot protocol / 自身布局管理, 不使用本宏。)
*/
#define GUEST_LINUX_STACK_TOP (GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE - PAGE_SIZE)

/*
   guest 必须从 DTB 里摘掉的, VMM 没有模拟的设备节点。

   为什么必须屏蔽：VMM 只模拟控制台与中断控制器, 其余设备一旦被 guest
   访问就会 G-stage/Stage-2 fault, 而 exit handler 在 MMIO 总线未命中时
   按「权限故障」处理(恢复属性让 guest 重试)→ 立刻再次 fault → 死循环,
   表现为 guest 卡死在探测那台设备上。启动前直接把这些节点从 DTB 中摘除
   (替换为 FDT_NOP), guest 就根本不会去枚举它们。

   移植自 kvmm loader.rs 的 nop_dtb_nodes。
*/
#if ARCH_AARCH64
/* VMM 只模拟了 PL011 与 GICD, DTB 里其余设备都没有实现。 */
#define GUEST_LINUX_UNSUPPORTED_NODES \
    { "v2m@8020000",         /* GICv2M MSI 帧(导致 GIC 初始化卡死) */ \
      "virtio_mmio@a000000", /* virtio-mmio transport(当前未模拟) */ \
      "pcie@10000000",       /* PCIe ECAM */ \
      "pl061@9030000",       /* GPIO */ \
      "pl031@9010000",       /* RTC */ \
      "flash@0",             /* CFI flash */ \
      "fw-cfg@9020000" }     /* QEMU fw_cfg */
#elif ARCH_RISCV64
/* 只模拟了 16550A 与 PLIC；virtio-mmio 未实现(guest 也不该从它启动)。 */
#define GUEST_LINUX_UNSUPPORTED_NODES { "virtio_mmio@a000000" }
#endif

/* vm_t 由 vmm.h 定义；这里只用到指针 */
struct vm;
typedef struct vm vm_t;

/*
   guest_loader_load_file — 把 rootfs 中的文件加载到 guest 物理地址 gpa
   返回加载字节数, 失败返回负值。

   注意： 需要 vm 是因为按需分页：同一个 GPA 在不同 VM 里指向不同的物理页,
   而且页面可能是"还没分配"的。内部走 vm_write_guest() 逐页分配。
*/
int guest_loader_load_file(vm_t *vm, const char *path, uint64_t gpa);

/*
   guest_loader_patch_dtb_* — 修补 DTB 中的 initrd / memory / bootargs 节点

   注意： 入参是宿主缓冲, 不是 guest 物理地址。补丁代码按线性偏移索引 FDT,
   而按需分页下 DTB 可能落在不连续的物理页上 —— 直接对 guest 内存做
   `dtb[i]` 会写到宿主或别的 VM 里。调用方先读到宿主缓冲, 补完再
   vm_write_guest() 写回。

   返回 0 成功。
*/
int guest_loader_patch_dtb_initrd(uint8_t *dtb, uint32_t dtb_size, uint64_t initrd_start, uint64_t initrd_end);
int guest_loader_patch_dtb_memory(uint8_t *dtb, uint32_t dtb_size, uint64_t mem_base, uint64_t mem_size);

/*
   guest_loader_patch_dtb_bootargs — 原地改写 /chosen/bootargs

   原地改写, 不支持加长：新串(含结尾 NUL)比 DTB 里现有的属性长就失败,
   并打一条 WARN。失败时 guest 会用 DTB 自带的那句命令行照常启动 —— 表现和
   成功一模一样, 所以每个架构都配了一条 _Static_assert 把常见的手滑挡在
   编译期(见 kernel/vmm/loader/guest_loader.c 顶部的 GUEST_LINUX_BOOTARGS)。

   返回 0 成功。
*/
int guest_loader_patch_dtb_bootargs(uint8_t *dtb, uint32_t dtb_size, const char *bootargs);

/*
   guest_loader_nop_dtb_nodes — 用 FDT_NOP 屏蔽未模拟的设备节点

   参见上面 GUEST_LINUX_UNSUPPORTED_NODES 的说明。
   返回被屏蔽的节点数。
*/
int guest_loader_nop_dtb_nodes(uint8_t *dtb, uint32_t dtb_size, const char *const *names, int nr_names);

/*
   guest_loader_dtb_node_has_prop — 某个节点上有没有某个属性

   加它只有一个原因：NPU 透传要的两个 DTB 属性, 真机上分别踩过一次 ——

     缺 `no-map`        → 池建不起来(memremap(WC) 打在 System RAM 上 return NULL)
                          症状：guest 里驱动 probe 成功, 作业永远超时
     缺 `dma-coherent`  → 一跑就 guest 内核 Oops(dma_sync_* 拿 phys_to_virt
                          去 dcache clean, 而 no-map 区没有线性映射)

   两个症状都不指向 DTB, 都耗掉一轮真机。DTB 就在宿主手里, 所以在这儿查。

   注意： 必须按节点查, 不能只查属性名在不在。 第一版就是查 strings 块,
      结果 `dma-coherent` 在 QEMU 的 fw-cfg / virtio_mmio / pcie 三个节点上
      本来就有 —— 判据永远通过, 等于没查。真踩过这个假阳性。

   node 要写完整节点名(如 "npu@fdab0000"), 只匹配根下的直接子节点。
   返回 1 = 有, 0 = 没有(或没这个节点), -1 = 不是合法 FDT。
*/
int guest_loader_dtb_node_has_prop(const uint8_t *dtb, uint32_t dtb_size, const char *node, const char *prop);

/*
   guest_loader_gpa_ptr — guest 物理地址 → 宿主内核可直接读写 的指针

   这是宿主代码碰 guest 内存的唯一入口：按本 VM 的 stage-2(aarch64 的
   stage-2 / riscv 的 G-stage / x86 的 EPT)查表翻译, 未映射且 @alloc 非 0 时
   现分配一页(清零)。

   注意： 声明必须放在这里。x86 的 guest_boot.c 要用它, 而早先忘记声明时 GCC 只
   给一条 `-Wimplicit-function-declaration` 警告, 按 `int` 处理返回值 ——
   于是返回的 64 位指针被截断低 32 位(反汇编里就是一句 `movslq %eax,%r15`),
   症状是随后读写一个"看着像物理地址"的野指针(实测 CR2=0x10ee202)。
   三层翻译里最不该出问题的一层, 栽在一行缺失的声明上。
*/
void *guest_loader_gpa_ptr(vm_t *vm, uint64_t gpa, int alloc);

/*
   ── 页感知的 guest 内存访问族 ────────────────────────────────

   注意： 不要对 guest_loader_gpa_ptr() 的返回值做跨页的指针算术。
   每个 guest 页都是各自分配的, 物理上不连续, `p + off`(off 超过一页)走到
   的是宿主的物理下一页, 而不是 guest 的下一页 —— 没有任何报错, 只是数据
   错位。identity 映射时代这个写法是对的, 按需分页之后不是了。
   跨页的一律用下面这几个(语义与 memcpy/memset/memmove 一致)。
*/
int guest_loader_write_guest(vm_t *vm, uint64_t gpa, const void *src, size_t n);
int guest_loader_read_guest(vm_t *vm, uint64_t gpa, void *dst, size_t n);
int guest_loader_fill_guest(vm_t *vm, uint64_t gpa, int byte, size_t n);

/*
   guest_loader_load_range — 把文件的 [file_off, file_off+len) 装到 guest 的 gpa
   len == 0 表示读到文件尾。返回装上的字节数, 失败返回 -1。

   x86 的 bzImage 走这条：它按引导协议分成「setup 段」与「保护模式内核」两段,
   分别装到不同的 GPA, 不要再"整个文件装到暂存区再搬"(那需要一段逐页的
   guest→guest 拷贝, 凭空多一层机制)。
*/
int guest_loader_load_range(vm_t *vm, const char *path, uint64_t file_off, uint64_t gpa, uint64_t len);

/* 把文件的 [file_off, file_off+len) 读进宿主缓冲(len == 0 = 到文件尾)。
   先看头部, 再决定各段装到哪, 就靠它。返回读到的字节数, 失败返回 -1。
*/
int guest_loader_read_file_range(const char *path, uint64_t file_off, void *buf, size_t len);

/* 取文件大小(字节), 失败返回 -1。 */
int guest_loader_file_size(const char *path);

/*
   把整个文件读进宿主缓冲(与 read_file_range 的区别：只从 0 读, 且校验
   "文件是否比缓冲还长")。返回读到的字节数, 失败返回 -1。

   实现从 guest_loader.c 拆到了 image_load.c —— 调用者仍在 guest_loader.c
  (把 DTB 读进宿主缓冲再打补丁那一步), 所以不能再是 static。
*/
int guest_loader_read_file(const char *path, uint8_t *buf, size_t cap);

/*
   guest_loader_check_vcpus — 本构建/本架构能否跑 nr 个 vCPU

   返回 0 可以；-1 不支持(并已用 KLOG_ERROR 说明原因)。规则：
     - 1..MAX_VCPUS；
     - 非 aarch64 只允许 1(AP 入口/SIPI/HSM 尚未实现)；
     - aarch64：N>1 需要 GICv3, 且 N ∈ {2,4,8,12}(仓库里只有这些 DTB)；
     - 选中路径的 DTB 必须真的在 rootfs 里(缺失通常意味着 rootfs 是旧的)。

   vmm_dev 在 vm_alloc() 之前调用它来尽早返回 EINVAL；loader 自己也会再调
   一次, 覆盖绕过 /dev/vmm 的直启路径。
*/
int guest_loader_check_vcpus(int nr);

/*
   guest_loader_run_linux — 加载并启动 Linux guest(BSP 侧调用)
   返回 0 表示已成功建立 VM 并创建 vCPU 任务。

   vm->cfg.nr_vcpus <= 0 表示使用构建默认 GUEST_LINUX_NR_VCPUS。
*/
int guest_loader_run_linux(vm_t *vm);

#endif /* GUEST_LOADER_H */
