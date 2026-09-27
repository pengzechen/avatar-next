/*
 * kernel/vmm/guest_loader.c — guest 镜像加载与启动
 *
 * 移植自 x-kernel: virt/kvmm-api/src/loader.rs，适配 Avatar OS：
 *   - kvfs → avatar 的 vfs_open / vfs_read_to_phys
 *   - 加载目标为 guest 物理地址（经 stage2 的 identity map 直接换算）
 *   - DTB 修补逻辑（initrd / memory）按 FDT 规范逐 token 走查
 */

#include "guest_loader.h"
#include "cache.h"
#include "klog.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "task/task.h"
#include "vfs.h"
#include "vmm/vmm.h"
#include "vmm/vmm_mmio.h"

#if ARCH_AARCH64
#include "aarch64/stage2.h"
/* guest 入口时 x0 的值（ARM64 boot 约定：DTB 物理地址）*/
volatile uint64_t g_guest_entry_x0;
#elif ARCH_RISCV64
#include "riscv64/gstage.h"
#elif ARCH_X86_64
#include "x86_64/ept.h"
#endif

/*
 * 每 VM 的静态表槽位数必须装得下 guest RAM 窗口 —— 按需分页的账本是
 * 「每 2 MiB 块一张页表」（aarch64 是 L3，riscv 是 L0），窗口比它大就会在
 * 缺页路径上被 ram_blk_index() 判为"窗口外"，表现是 guest 访问那段内存
 * 时被当成 MMIO 去解码，或者直接停机。两条常量分别定义在两个架构头里，
 * 只有这里同时看得见它们。
 */
#if ARCH_AARCH64
_Static_assert(GUEST_LINUX_MEM_SIZE / S2_BLOCK_SIZE <= S2_MAX_L3_TABLES,
               "GUEST_LINUX_MEM_SIZE 超出 S2_MAX_L3_TABLES 覆盖范围");
#elif ARCH_RISCV64
_Static_assert(GUEST_LINUX_MEM_SIZE / GSTAGE_BLOCK_SIZE <= GSTAGE_MAX_L0_TABLES,
               "GUEST_LINUX_MEM_SIZE 超出 GSTAGE_MAX_L0_TABLES 覆盖范围");
#elif ARCH_X86_64
_Static_assert(GUEST_LINUX_MEM_SIZE / EPT_BLOCK_SIZE <= EPT_MAX_PT_TABLES,
               "GUEST_LINUX_MEM_SIZE 超出 EPT_MAX_PT_TABLES 覆盖范围");
#endif

/*
 * guest 内核命令行。
 *
 * ⚠️ 上限 78 字节（不含结尾 NUL）：DTB 里 /chosen/bootargs 属性只有 79 字节
 * 的槽位，而 guest_loader_patch_dtb_bootargs() 是**原地改写、不支持加长**。
 * 超了补丁就失败，guest 会退回 DTS 里那句 —— 表现和成功一模一样（照常启动），
 * 所以这个宏等于没生效。历史上就是这样：原串 86 字节，静默失败了很久，
 * 直到调 guest 启动速度时才发现。下面的 _Static_assert 让超长直接编译不过。
 *
 * 内容取舍：
 *   quiet          console_loglevel 7→4，只放行 ERR 及以上。
 *                  guest 每写一个字符到 UARTDR 都要陷入 EL2 做一次完整的
 *                  世界切换（PL011 在 stage-2 里是无效映射），实测 35~70µs
 *                  /字符。约 1.2 万字符的启动日志要花掉 0.86s —— 而裸跑同样
 *                  的日志只差 0.02s，因为那边没有 exit。quiet 只影响往串口写
 *                  的部分，内核环形缓冲里仍是完整的，事后 dmesg 全看得到。
 *   console=       guest 控制台走 PL011（与 vpl011 同地址 0x09000000）。
 *   rdinit=/init   显式指定 init，不依赖内核「没有 init= 就找 /init」的回退。
 *   panic_on_warn=0 / oops=panic  调试用：警告不停机，oops 停。
 *
 * 想恢复完整启动日志：去掉 "quiet "（还剩 55 字节，仍在预算内）。
 * 不带 earlycon：它和 quiet 同时开意义不大（早期消息同样被 loglevel 压掉），
 * 而且会让每条消息打印两遍（bootconsole + 真 console），白白多一倍 exit。
 */
#if ARCH_AARCH64
#define GUEST_LINUX_BOOTARGS                                                   \
  " console=ttyAMA0 rdinit=/init  oops=panic"

_Static_assert(sizeof(GUEST_LINUX_BOOTARGS) - 1 <= 78,
               "GUEST_LINUX_BOOTARGS 超出 DTB 的 /chosen/bootargs 槽位"
               "（79 字节含结尾 NUL），补丁会静默失败");

#elif ARCH_RISCV64
/*
 * RISC-V 侧同一个套路，但**槽位大得多**：imgs/guests/rv64/linux.dts 里
 * 那句自带命令行是详细日志版（90 字节），所以这里可以更宽松地在
 * 「详细 / quiet」之间切。默认对齐 aarch64 走 quiet。
 *
 * 内容取舍与 aarch64 相同（逐字符 UART 写都要陷一次 G-stage → MMIO 模拟，
 * 约 35~70µs/字符）：
 *   quiet          console_loglevel 7→4，只放行 ERR 及以上。
 *   console=       guest 控制台走 16550A（与 vuart16550 同地址 0x10000000）。
 *   rdinit=/init   显式指定 init，不依赖内核回退。
 *   panic_on_warn=0 / oops=panic  调试用：警告不停机，oops 停。
 *
 * 想恢复完整启动日志：去掉 "quiet "（还剩 32 字节，仍在预算内），想连
 * 8250 驱动接管之前的部分也看到就再加
 * `earlycon=uart8250,mmio,0x10000000`（代价是每条早期消息打两遍）。
 *
 * 注意：**补丁失败时会退回 DTB 里那句自带命令行**，而 imgs/guests/rv64/
 * linux.dts 里写的正是详细日志那一版 —— 所以「补丁静默失败」在这边表现得
 * 不是「少打日志」而是「日志变多」，反过来更好发现。
 */
#define GUEST_LINUX_BOOTARGS                                                   \
  "quiet console=ttyS0 rdinit=/init panic_on_warn=0 oops=panic"

_Static_assert(sizeof(GUEST_LINUX_BOOTARGS) - 1 <= 90,
               "GUEST_LINUX_BOOTARGS 超出 DTB 的 /chosen/bootargs 槽位"
               "（91 字节含结尾 NUL），补丁会静默失败 —— 详见 "
               "imgs/guests/rv64/linux.dts 的注释（槽位由那句自带命令行决定）");
#endif

/* ── 启动 Linux guest ─────────────────────────────────────── */
/*
 * 调用者两种：
 *   - kernel_main（RUN_GUEST_LINUX 直启）：开机直接进 guest，没有宿主 shell；
 *   - /dev/vmm 的 bootlinux（宿主 shell 里跑 /bin/vmm-run）。
 *
 * 两个调用者靠 vm_t 是函数静态变量天然互斥，这里再加一道显式闸门：
 * 直启之后 shell 是不存在的，但反过来 —— 在 shell 里先启动过 guest、
 * 停掉、再启动 —— 会真的重入本函数。
 *
 * 重入是支持的：guest RAM 每次都被 memset 清干净、镜像重新加载、DTB 重新
 * 修补、vm_create() 重建 Stage-2 与 vGIC/vPL011。所以只需要挡住「上一个
 * guest 还在跑」这一种情况。
 */
#if ARCH_X86_64
/*
 * x86 的引导协议与 arm64/riscv 完全不同（bzImage + zeropage + E820 +
 * 长模式入口），而且本架构还有 GPA≠HPA 的额外一层。与其在下面这段
 * 共用的 DTB 逻辑里塞满 #if，不如整条路径单独实现、在这里分流 ——
 * 这样 arm64/riscv 那两段一行都不用动（回归风险为 0）。
 */
extern int x86_guest_boot(vm_t *vm);
#endif

int guest_loader_run_linux(vm_t *vm) {
#if ARCH_X86_64
  return x86_guest_boot(vm);
#else
  int rc;

  /*
   * 重入保护：调用者（/dev/vmm 或直启路径）已经从 VM 池里拿到一个槽位并
   * 置成 LOADING，这里只需确认它可用。（从前是一个全局 "guest 在跑吗"，
   * 那在多 VM 下会拦住第二个 VM。）
   */
  if (!vm || vm->state != VM_LOADING) {
    KLOG_WARN("[guest] run_linux: vm slot not in LOADING state, refusing\n");
    return -1;
  }

  KLOG_INFO("\n=== Avatar OS: booting Linux guest (%s) ===\n",
#if ARCH_AARCH64
            "aarch64"
#elif ARCH_RISCV64
            "riscv64"
#else
            "unknown"
#endif
  );

  /*
   * ── 建立 VM（**必须**在加载映像之前）──────────────────────────
   *
   * stage-2 是在 vm_create() 里初始化的 —— 一张**空表**。而加载映像要往
   * guest 物理地址里写，那需要映射。按需分页下 guest_loader_write_guest() 会为每一页
   * 现分配，所以这里**不再需要**预先预留物理内存或整片清零：
   *
   *   pmm_mark_allocated(192 MiB)   ← 删掉了
   *   memset(192 MiB)               ← 删掉了
   *
   * 从前那两行既是"只能有一个 VM"的根源（第二份无处安放、memset 会抹掉
   * 第一个 VM 的内存），也让每个 VM 无论用多少都硬吃 192 MiB。现在宿主
   * 只为 guest 真正碰过的页付内存，且两个 VM 的页天然隔离。
   */
  rc = vm_create(vm);
  if (rc != 0) {
    KLOG_ERROR("[guest] vm_create failed\n");
    return -1;
  }

  /* 1. 加载 kernel Image */
  int klen = guest_loader_load_file(vm, GUEST_LINUX_KERNEL_PATH,
                                    GUEST_LINUX_KERNEL_GPA);
  if (klen <= 0)
    return -1;
  KLOG_INFO("[guest] kernel: %d bytes\n", klen);

  /*
   * 2. DTB：**读进宿主缓冲**再打补丁，最后一次性写回 guest。
   *
   * 不能像从前那样直接对 guest 内存 `dtb[i]`：补丁代码按线性偏移索引 FDT，
   * 而按需分页下 4 KB 的 DTB 可能落在不连续的物理页上 —— 越过页边界继续
   * 线性索引就会写到宿主或别的 VM 的内存里。
   */
  static uint8_t dtb_buf[8192];
  int dlen = guest_loader_read_file(GUEST_LINUX_DTB_PATH, dtb_buf,
                                    sizeof(dtb_buf));
  if (dlen <= 0)
    return -1;

  guest_loader_patch_dtb_memory(dtb_buf, (uint32_t)dlen,
                                GUEST_LINUX_MEM_BASE, GUEST_LINUX_MEM_SIZE);

  if (guest_loader_patch_dtb_bootargs(dtb_buf, (uint32_t)dlen,
                                      GUEST_LINUX_BOOTARGS) != 0) {
    KLOG_ERROR("[guest] bootargs 补丁失败：guest 会用 DTB 自带的那句命令行，"
               "GUEST_LINUX_BOOTARGS 不生效（原因见上一条 WARN）\n");
  }

  /* 3. 加载 initrd */
  int ilen = guest_loader_load_file(vm, GUEST_LINUX_INITRD_PATH,
                                    GUEST_LINUX_INITRD_GPA);
  if (ilen <= 0)
    return -1;

  guest_loader_patch_dtb_initrd(dtb_buf, (uint32_t)dlen,
                                GUEST_LINUX_INITRD_GPA,
                                GUEST_LINUX_INITRD_GPA + (uint64_t)ilen);

  /* ── 屏蔽 VMM 未模拟的 DTB 设备节点 ─────────────────────
   *
   * VMM 只模拟了 PL011 与 GICD。其余设备一旦被 guest 访问就会 Stage-2
   * fault，而 exit handler 在 MMIO 总线未命中时按「权限故障」处理
   * （stage2_restore 后让 guest 重试）→ 立刻再次 fault → **死循环**。
   *
   * 实测：guest 的 GIC 初始化会探测 "v2m@8020000"（GICv2M MSI 帧），
   * 因未模拟而卡死在 "Root IRQ handler: gic_handle_irq" 之后。
   *
   * 故启动前直接把这些节点从 DTB 中摘除（替换为 FDT_NOP），使 guest
   * 根本不去枚举它们。移植自 kvmm loader.rs 的 nop_dtb_nodes。*/
  {
    static const char *const unsupported_nodes[] =
        GUEST_LINUX_UNSUPPORTED_NODES;
    guest_loader_nop_dtb_nodes(
        dtb_buf, (uint32_t)dlen, unsupported_nodes,
        (int)(sizeof(unsupported_nodes) / sizeof(unsupported_nodes[0])));
  }

  /* 补好的 DTB 一次性写进 guest（memory/bootargs/initrd 都改完了）*/
  if (guest_loader_write_guest(vm, GUEST_LINUX_DTB_GPA, dtb_buf, (size_t)dlen) != 0)
    return -1;

  /*
   * 5. 配置 vCPU 的入口状态
   *
   * 必须在 vm_create() **之后**：两个架构的 vm_init 都会 memset 整个
   * vcpu 数组，先写会被清掉。
   */
  vcpu_t *vcpu = &vm->vcpus[0];

#if ARCH_AARCH64
  /*
   * x0 直接写 vcpu->r[0]：el2_vmcs.S 在 eret 前用 `ldr x0, [x0, #0]`
   * 最后加载 guest x0（VCPU_R0 偏移 0），因此无需改动汇编。
   * ARM64 boot 约定：x0 = DTB 物理地址，PSTATE = EL1h 且 DAIF 屏蔽。
   */
  vcpu->elr = GUEST_LINUX_KERNEL_GPA;
  vcpu->spsr = 0x5ULL | (0xFULL << 6); /* EL1h, DAIF 屏蔽 */
  vcpu->sp_el1 = GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE - 0x1000;
  vcpu->r[0] = GUEST_LINUX_DTB_GPA;
  g_guest_entry_x0 = GUEST_LINUX_DTB_GPA;

  KLOG_INFO("[guest] entry=0x%llx x0(DTB)=0x%llx sp_el1=0x%llx\n",
            (unsigned long long)vcpu->elr,
            (unsigned long long)GUEST_LINUX_DTB_GPA,
            (unsigned long long)vcpu->sp_el1);

#elif ARCH_RISCV64
  /*
   * RISC-V Linux boot 协议（Documentation/riscv/boot.rst）：
   *   pc = Image 起始（按 2 MiB 对齐装入）
   *   a0 = boot hartid
   *   a1 = DTB 物理地址
   * 其余寄存器无约定；sp 由内核自己在 head.S 里设好，这里给个合法值
   * 只是为了「万一它在设 sp 之前先出异常」时不至于落到 0。
   *
   * vsatp = 0（bare）：**不能**沿用宿主 satp —— guest 是独立地址空间，
   * 由 Linux 自己在 head.S 里建页表再写 satp。沿用宿主页表的话，guest
   * 一取指就会按宿主的内核映射跑，行为完全不可预期。
   *
   * vstvec 也留 0：Linux 在 head.S 很早就会 csrw stvec 换成自己的
   * trap vector；在此之前不让任何中断挂起（vsie=0，hvip 的注入发生在
   * 每次入口、由 vmm_arch_restore_guest_ctx 计算）。
   */
  vcpu->pc = GUEST_LINUX_KERNEL_GPA;    /* 恢复 PC = sret 目标 */
  vcpu->vsepc = GUEST_LINUX_KERNEL_GPA; /* guest 自己的 sepc   */
  vcpu->vsatp = 0;
  vcpu->vsstatus = 0;
  vcpu->vstvec = 0;
  vcpu->vsie = 0;
  vcpu->vsscratch = 0;
  vcpu->r[2] = GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE - 0x1000;
  vcpu->r[10] = GUEST_LINUX_BOOT_HARTID; /* a0 */
  vcpu->r[11] = GUEST_LINUX_DTB_GPA;     /* a1 */

  KLOG_INFO("[guest] entry=0x%llx a0(hartid)=%llu a1(DTB)=0x%llx\n",
            (unsigned long long)vcpu->vsepc, (unsigned long long)vcpu->r[10],
            (unsigned long long)vcpu->r[11]);
#endif

  /* 6. 创建 vCPU 任务 */
  task_t *vt = vcpu_task_create(vcpu, 5);
  if (!vt) {
    KLOG_ERROR("[guest] vcpu_task_create failed\n");
    return -1;
  }
  KLOG_INFO("[guest] Linux vCPU task id=%u running\n", vt->id);
  return 0;
#endif /* !ARCH_X86_64 */
}
