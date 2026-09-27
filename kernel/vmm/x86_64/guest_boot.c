/*
 * kernel/vmm/x86_64/guest_boot.c — x86_64 guest Linux 引导（bzImage / 64 位入口）
 *
 * 与 arm64/riscv 的差别很大：那两个架构是「把 Image 拷到内存 + 传 DTB」，
 * 而 x86 要走 Linux 的引导协议（Documentation/arch/x86/boot.rst）：
 *
 *   1. 读 bzImage 的 setup header（0x1f1 起），校验 "HdrS"
 *   2. 把引导扇区+setup 装到 0x10000；保护模式内核装到 pref_address
 *      （为 0 时用 0x100000）
 *   3. 在 0x70000 构造 boot_params（zero page）：setup_header 原样拷进去、
 *      填命令行/initrd/E820
 *   4. 自建临时页表（PML4→PDPT→PD，2 MiB 大页 identity 映射前 1 GiB）
 *   5. 自建 GDT（空 + 64 位代码段 + 数据段）
 *   6. vCPU 直接以**长模式**起步，RIP = 内核装载地址 + 0x200（64 位入口点），
 *      RSI = boot_params 的物理地址
 *
 * ⚠️ 本架构独有：**GPA ≠ HPA**。
 *   guest 物理地址必须从 0 开始（Linux 假定低端有常规内存、内核在 1 MiB），
 *   而宿主的物理 0 显然不能给它。所以 guest RAM 放在宿主的
 *   GUEST_X86_HPA_BASE 窗口里，由 EPT 翻译。**所有往 guest 内存写数据的
 *   地方都要过 gpa_ptr()** —— 直接拿 GPA 去 phys_to_virt 就写到宿主的
 *   低端物理内存上了，宿主当场崩。
 *
 * 引导协议为什么选 64 位入口（而不是走 16 位实模式 setup）：
 *   HdrS + xloadflags 的 XLF_KERNEL_64 表明内核自带 64 位入口，直接跳过去
 *   可以完全绕开实模式、BIOS 调用、VGA 这些我们没实现的东西。
 */

#include "vmm/vmm.h"
#include "guest_loader.h"
#include "klog.h"
#include "string.h"
#include "pmm.h"
#include "mm_vm.h"
#include "cache.h"
#include "x86_64/vmx.h"
#include "task/task.h"

#if ARCH_X86_64

/* ── bzImage setup header 的文件内偏移（Documentation/arch/x86/boot.rst）── */
#define HDR_OFF           0x1f1
#define HDR_SETUP_SECTS   (HDR_OFF + 0x00)   /* u8  */
#define HDR_SYSSIZE       (HDR_OFF + 0x03)   /* u32 */
#define HDR_BOOT_FLAG     (HDR_OFF + 0x0d)   /* u16，应为 0xAA55 */
#define HDR_MAGIC         (HDR_OFF + 0x11)   /* "HdrS" */
#define HDR_VERSION       (HDR_OFF + 0x15)   /* u16 */
#define HDR_LOADFLAGS     (HDR_OFF + 0x18)   /* u8  */
#define HDR_CODE32_START  (HDR_OFF + 0x23)   /* u32 */
#define HDR_INITRD_MAX    (HDR_OFF + 0x3b)   /* u32：initrd 必须在这之下 */
#define HDR_KERNEL_ALIGN  (HDR_OFF + 0x3f)   /* u32 */
#define HDR_RELOCATABLE   (HDR_OFF + 0x43)   /* u8  */
#define HDR_XLOADFLAGS    (HDR_OFF + 0x45)   /* u16 */
#define HDR_CMDLINE_SIZE  (HDR_OFF + 0x47)   /* u32 */
#define HDR_PREF_ADDRESS  (HDR_OFF + 0x67)   /* u64：首选装载地址 */
#define HDR_INIT_SIZE     (HDR_OFF + 0x6f)   /* u32 */
#define HDR_HDR_SIZE      0x80               /* 拷进 boot_params 的长度 */

#define XLF_KERNEL_64     (1u << 0)          /* 有 64 位入口 */

/* ── boot_params（zero page）里的字段偏移 ── */
#define BP_E820_ENTRIES   0x1e8   /* u8  */
#define BP_TYPE_OF_LOADER 0x210   /* u8  */
#define BP_LOADFLAGS      0x211   /* u8  */
#define BP_CODE32_START   0x214   /* u32 */
#define BP_RAMDISK_IMAGE  0x218   /* u32 */
#define BP_RAMDISK_SIZE   0x21c   /* u32 */
#define BP_CMD_LINE_PTR   0x228   /* u32 */
#define BP_INITRD_ADDR_MAX 0x22c  /* u32 */
#define BP_KERNEL_ALIGN   0x230   /* u32 */
#define BP_RELOCATABLE    0x234   /* u8  */
#define BP_XLOADFLAGS     0x236   /* u16 */
#define BP_CMDLINE_SIZE   0x238   /* u32 */
#define BP_E820_TABLE     0x2d0   /* 每项 20 字节：addr(8) size(8) type(4) */
#define BP_SIZE           0x1000  /* zero page 本身 4 KiB */

#define BP_LOADED_HIGH    (1u << 0)
#define BP_CAN_USE_HEAP   (1u << 7)   /* 解压器用 heap_end_ptr 里的堆 */
#define BP_HEAP_END_PTR   0x224        /* u16 */
#define BP_SETUP_MOVE_SZ  0x212        /* u16 */

/* E820 类型 */
#define E820_RAM          1
#define E820_RESERVED     2

/* 64 位入口点相对内核装载地址的偏移（引导协议规定）*/
#define CODE64_OFFSET     0x200

static uint64_t s_kernel_load;   /* 保护模式内核的装载物理地址（GPA）*/

/* ── GPA → 宿主内核可直接写的指针 ────────────────────────── */

static void *gpa_ptr(uint64_t gpa)
{
    return phys_to_virt(GUEST_X86_HPA_BASE + gpa);
}

/* guest 物理窗口对应的宿主物理基址（vmx.c 的 EPT 初始化要用）*/
uint64_t x86_guest_hpa_base(void)
{
    return GUEST_X86_HPA_BASE;
}

/* guest RAM 大小（vmx.c 的诊断要判断地址是否落在 guest 内存里）*/
uint64_t x86_guest_mem_size(void)
{
    return GUEST_LINUX_MEM_SIZE;
}

/* ================================================================
 * MP table（Intel MP Spec 1.4）—— 照 tgoskits 的做法
 *
 * 没有 MP table / ACPI 时内核会走
 *   "APIC: ACPI MADT or MP tables are not detected"
 *   "Switch to virtual wire mode setup with no configuration"
 * 于是 Local APIC 未被正常启用，中断起不来（`Attempted to kill the idle
 * task` panic）。只提供 LAPIC + ISA 中断源，**不提供 IOAPIC**（本 VMM 未
 * 实现 IOAPIC，声明了内核会去用然后失败）。
 * ================================================================ */
#define MP_TABLE_GPA   0x9F800ULL
#define MP_FP_GPA      0x9FC00ULL

static void mp_w(void *base, uint32_t off, uint64_t v, int n)
{
    uint8_t *b = (uint8_t *)base;
    for (int i = 0; i < n; i++)
        b[off + i] = (uint8_t)(v >> (8 * i));
}

static uint8_t mp_sum(const uint8_t *p, uint32_t n)
{
    uint8_t s = 0;
    while (n--)
        s = (uint8_t)(s + *p++);
    return s;
}

static void build_mptable(void)
{
    uint8_t *t = (uint8_t *)gpa_ptr(MP_TABLE_GPA);
    uint8_t *f = (uint8_t *)gpa_ptr(MP_FP_GPA);
    uint32_t o = 44;                       /* 条目紧跟在 44 字节头之后 */
    uint16_t nent = 0;
    static const uint8_t irq[5] = { 0, 1, 3, 4, 14 };

    memset(t, 0, 0x800);
    memset(f, 0, 16);

    t[o] = 0; t[o + 1] = 0; t[o + 2] = 0x14; t[o + 3] = 0x03;   /* type0 处理器：id0，enabled|BSP */
    o += 20; nent++;
    t[o] = 1; t[o + 1] = 0; memcpy(t + o + 2, "ISA   ", 6); o += 8; nent++;
    t[o] = 1; t[o + 1] = 1; memcpy(t + o + 2, "PCI   ", 6); o += 8; nent++;
    /* type2：IOAPIC @0xFEC00000（symmetric I/O 模式要求它存在）*/
    t[o] = 2; t[o + 1] = 0; t[o + 2] = 0x11; t[o + 3] = 0x01;
    mp_w(t, o + 4, 0xFEC00000ULL, 4);
    o += 8; nent++;

    /*
     * **不**写 type-3（ISA 中断源）条目：写了内核会判定为 symmetric I/O
     * 模式并依赖 IOAPIC 投递中断，而本 VMM 没有 IOAPIC 实体 ⇒ 中断全无 ⇒
     * `Attempted to kill the idle task`。只留 LAPIC + 总线，内核走
     * virtual-wire（LAPIC 直连），我们的 vLAPIC 定时器才起作用。
     */
    (void)irq;

    memcpy(t, "PCMP", 4);
    mp_w(t, 4, o, 2);
    t[6] = 4;
    mp_w(t, 34, nent, 2);
    mp_w(t, 36, 0xFEE00000ULL, 4);
    t[7] = (uint8_t)(0 - mp_sum(t, o));    /* 校验和：整表求和 = 0 */

    memcpy(f, "_MP_", 4);
    mp_w(f, 4, MP_TABLE_GPA, 4);
    f[8] = 1; f[9] = 4;                    /* 长度（16B 单位）、spec */
    f[10] = (uint8_t)(0 - mp_sum(f, 16));

    KLOG_INFO("[x86boot] MP table @0x%llx: %u entries / %u bytes\n",
              (unsigned long long)MP_TABLE_GPA, (unsigned)nent, (unsigned)o);
}


/*
 * ── 小工具 ──
 *
 * 一律走 memcpy：boot_params 的字段在结构体里是自然对齐的，但用
 * `*(uint16_t *)(bp+off) = v` 这种写法会让 GCC 报 -Wstringop-overflow
 * （它跟丢 gpa_ptr 的来源后认为目标对象大小是 0），而且将来万一有
 * 非对齐字段就是踩坑。memcpy 语义清楚、编译期也不再告警。
 */
static void bp_put8(uint8_t *bp, uint32_t off, uint8_t v)
{
    memcpy(bp + off, &v, 1);
}
static void bp_put16(uint8_t *bp, uint32_t off, uint16_t v)
{
    memcpy(bp + off, &v, 2);
}
static void bp_put32(uint8_t *bp, uint32_t off, uint32_t v)
{
    memcpy(bp + off, &v, 4);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* ── 构造 E820 内存图 ─────────────────────────────────────── */
static void build_e820(uint8_t *bp)
{
    struct { uint64_t addr, size; uint32_t type; } ent[5];
    int n = 0;

    ent[n].addr = 0x00000000ULL; ent[n].size = 0x0009F000ULL; ent[n].type = E820_RAM;      n++;
    ent[n].addr = 0x0009F000ULL; ent[n].size = 0x00061000ULL; ent[n].type = E820_RESERVED; n++;
    /* guest RAM 的可扩展区（1 MiB 起，到 192 MiB 顶）*/
    ent[n].addr = 0x00100000ULL;
    ent[n].size = GUEST_LINUX_MEM_SIZE - 0x00100000ULL;
    ent[n].type = E820_RAM; n++;
    /* LAPIC 窗口：标成保留，免得内核把这个区间当普通内存用 */
    ent[n].addr = 0xFEE00000ULL; ent[n].size = 0x00100000ULL; ent[n].type = E820_RESERVED; n++;

    bp_put8(bp, BP_E820_ENTRIES, (uint8_t)n);
    for (int i = 0; i < n; i++) {
        uint8_t *e = bp + BP_E820_TABLE + i * 20;
        memcpy(e + 0,  &ent[i].addr, 8);
        memcpy(e + 8,  &ent[i].size, 8);
        memcpy(e + 16, &ent[i].type, 4);
    }
    KLOG_INFO("[x86boot] e820: %d entries, RAM 1MiB..0x%llx\n", n,
              (unsigned long long)GUEST_LINUX_MEM_SIZE);
}

/* ── 临时页表：identity 映射前 1 GiB（2 MiB 大页）──────────── */
static void build_pgtbl(uint8_t *pg)
{
    uint64_t *pml4 = (uint64_t *)pg;
    uint64_t *pdpt = (uint64_t *)(pg + 0x1000);
    uint64_t *pd   = (uint64_t *)(pg + 0x2000);

    memset(pg, 0, 0x3000);

    /*
     * ⚠️ 页表项里放的是 **guest 物理地址**，不是宿主物理地址。
     *
     * 一开始顺手写了 GUEST_X86_HPA_BASE + ...，于是 guest 把 0x20091000
     * 当成下一页表的 GPA 去走 —— EPT 里当然没有这个 GPA，每一步都缺页，
     * 而缺页处理又"解码失败就跳过指令"，RIP 每次 +3，看起来像 guest 在
     * 乱跑。判据就是 EPT violation 报出来的 gpa 落在宿主窗口里（0x2xxxxxxx）。
     */
    pml4[0] = (GUEST_LINUX_PGTBL_GPA + 0x1000) | 0x3ULL;   /* P|RW → PDPT */
    pdpt[0] = (GUEST_LINUX_PGTBL_GPA + 0x2000) | 0x3ULL;   /* P|RW → PD   */

    for (int i = 0; i < 512; i++)                        /* 512 × 2 MiB = 1 GiB */
        pd[i] = ((uint64_t)i << 21) | 0x83ULL;           /* P|RW|PS */

    /*
     * ── 高半区映射（缺了它 kernel 会在早期陷入自取异常的死循环）──
     *
     * 只有 identity 映射是不够的：Linux 的内核虚拟地址在
     * __START_KERNEL_map = 0xffffffff80000000 起，`__startup_64` 之后
     * 的代码就开始按**这些虚拟地址**取指/访存。此时 guest 的 CR3 还是
     * 我们这张临时表，上面没有高半区 → guest 自己 #PF → 它的处理程序
     * 又踩同样的坑 → 无限循环。
     *
     * 判据（实测）：直方图里除了宿主 tick 什么都没有（异常在 guest 内部
     * 消化，根本不到 VMM），RIP 停在 guest 的陷阱入口（SAVE_ALL 的
     * push 序列）反复出现。
     *
     * 做法与 QEMU 的 boot page tables 一致：
     *   VA 0xffffffff80000000 + X  →  物理 X
     * PML4 索引 = (0xffffffff80000000>>39)&0x1FF = 511
     * PDPT 索引 = (>>30)&0x1FF = 510，PD 索引 = 0
     */
    {
        uint64_t *hpdpt = (uint64_t *)(pg + 0x3000);
        uint64_t *hpd   = (uint64_t *)(pg + 0x4000);

        memset(hpdpt, 0, 0x2000);
        pml4[511]  = (GUEST_LINUX_PGTBL_GPA + 0x3000) | 0x3ULL;  /* P|RW */
        hpdpt[510] = (GUEST_LINUX_PGTBL_GPA + 0x4000) | 0x3ULL;  /* P|RW */
        for (int i = 0; i < 512; i++)
            hpd[i] = ((uint64_t)i << 21) | 0x83ULL;      /* 2 MiB 页，1 GiB */
    }

    KLOG_INFO("[x86boot] page tables @gpa 0x%llx, identity 0..1GiB\n",
              (unsigned long long)GUEST_LINUX_PGTBL_GPA);
}

/* ── 临时 GDT：空 + 64 位代码段 + 数据段 ──────────────────── */
static void build_gdt(uint8_t *gdt)
{
    uint64_t *g = (uint64_t *)gdt;

    /*
     * ⚠️ 必须用 **Linux 自己的 boot_gdt 布局**：
     *   GDT[0]=0x00 空  GDT[1]=0x08 (32 位代码，占位)
     *   GDT[2]=0x10 **64 位代码**   GDT[3]=0x18 **数据**
     * 因为内核的 `startup_64` 会用**硬编码选择子**重载段寄存器：
     * CS=0x10、DS/ES/SS=0x18（见 arch/x86/boot/compressed/head_64.S 的
     * 常量与 boot_gdt）。原来把代码段放在 0x08、数据段放在 0x10，
     * 内核加载 CS=0x10 时拿到的是**数据段**（不可执行 → #GP），
     * 加载 DS=0x18 时又超出 GDT limit（→ #GP）——早期就崩，
     * 表现正是「进了 64 位入口之后不久就异常循环」。
     */
    g[0] = 0;
    /*
     * **32 位**保护模式的描述符（走 Linux 传统引导协议，见下面的入口说明）。
     * 数值直接对标 tgoskits `virtualization/axvm/src/arch/x86_64/boot/linux_boot.rs`
     * 里的引导 stub，以及 QEMU 的 load_linux()。
     */
    g[1] = 0x0000000000000000ULL;   /* 0x08: 保留（32 位协议不用）*/
    g[2] = 0x00CF9A000000FFFFULL;   /* 0x10: 32 位代码 */
    g[3] = 0x00CF92000000FFFFULL;   /* 0x18: 32 位数据 */

    KLOG_INFO("[x86boot] gdt @gpa 0x%llx (code64=0x%x data=0x%x)\n",
              (unsigned long long)GUEST_LINUX_GDT_GPA, 0x08, 0x10);
}

/*
 * x86_guest_boot — x86_64 的 Linux 引导入口（由 guest_loader_run_linux 调用）
 *
 * 返回 0 = vCPU 任务已创建（由它跑 guest）。
 */
int x86_guest_boot(void)
{
    /*
     * ⚠️ x86 这条路径**还没**迁到 VM 池（vmm.c 的 vm_alloc/vm_get）：它自带
     * 一个 static vm_t，也没有 stage-2 的按需分页（EPT 仍是整段预留 +
     * enable_mmio_trap）。多 VM 在 x86 上还没做，别被这里的 static 误导。
     * 下面传 &vm 只是为了让 guest_loader_load_file 的新签名（它要靠 host-va
     * 翻译才知道往哪写）能编过。
     */
    static vm_t vm;
    uint8_t hdr[HDR_HDR_SIZE];
    int klen, ilen;
    uint32_t setup_bytes, cmdline_len;
    uint64_t pref;

    /* ── 1. 预留并清零 guest RAM（宿主侧的物理窗口）── */
    KLOG_INFO("[x86boot] reserving host RAM: 0x%llx - 0x%llx (guest 192 MiB)\n",
              (unsigned long long)GUEST_X86_HPA_BASE,
              (unsigned long long)(GUEST_X86_HPA_BASE + GUEST_LINUX_MEM_SIZE - 1));
    pmm_mark_allocated(g_pmm, GUEST_X86_HPA_BASE,
                       GUEST_X86_HPA_BASE + GUEST_LINUX_MEM_SIZE - 1);
    memset(phys_to_virt(GUEST_X86_HPA_BASE), 0, GUEST_LINUX_MEM_SIZE);
    clean_dcache_range(phys_to_virt(GUEST_X86_HPA_BASE), GUEST_LINUX_MEM_SIZE);

    /* TEMP-DBG: 哨兵 —— 放在 guest 不会碰的两处（低端 0x4000 与中间 0x5000000）。
     * 若宿主 PMM 没真正预留 guest RAM，宿主自己的分配会踩掉它们；
     * 这就是「每次运行解压位置都不同」的来源。*/
    {
        volatile uint64_t *s1 = (volatile uint64_t *)gpa_ptr(0x4000);
        volatile uint64_t *s2 = (volatile uint64_t *)gpa_ptr(0x5000000);
        s1[0] = 0xA5A5A5A55A5A5A5AULL; s1[1] = 0x1122334455667788ULL;
        s2[0] = 0xDEADBEEFCAFEBABEULL; s2[1] = 0x00FF00FF00FF00FFULL;
        KLOG_WARN("[SENTINEL] 已埋: 0x4000=%llx 0x5000000=%llx\n",
                  (unsigned long long)s1[0], (unsigned long long)s2[0]);
    }

    /* ── 2. 读 bzImage 头，校验 HdrS ── */
    {
        /* 先把整个文件读进来再解析：直接按段加载需要知道 setup_sects，
         * 而它在文件头里。guest_loader_load_file 一次装完最简单。*/
        klen = guest_loader_load_file(&vm, GUEST_LINUX_KERNEL_PATH,
                                      GUEST_X86_HPA_BASE + GUEST_LINUX_SETUP_GPA);
        if (klen <= 0) {
            KLOG_ERROR("[x86boot] 无法加载 %s\n", GUEST_LINUX_KERNEL_PATH);
            return -1;
        }
    }

    {
        const uint8_t *img = (const uint8_t *)gpa_ptr(GUEST_LINUX_SETUP_GPA);

        /*
         * ── 未压缩的原始内核 ELF（与 aarch64/riscv64 同款思路）──
         *
         * `vmlinux.bin` 的 `e_entry` 就是 0x1000000（= 装载地址），PT_LOAD 段的
         * `p_paddr` 是最终物理地址。内核自己的 `startup_64` 会建页表/栈并继续，
         * **整段自解压器被跳过** —— 而解压器正是此前所有 session 的死因
         * （inflate_fast 里距离表变坏、缺页风暴、cr2=0 …）。
         * 入口环境（长模式 + identity/高半区页表 + `%rsi = boot_params`）
         * 与本文件里已经调通的那条 64 位路径完全一致。
         */
        if (le32(img + HDR_MAGIC) != 0x53726448u /* "HdrS" 小端 */) {
            KLOG_ERROR("[x86boot] 不是 bzImage：HdrS 魔数缺失\n");
            return -1;
        }
        if (le16(img + HDR_BOOT_FLAG) != 0xAA55) {
            KLOG_ERROR("[x86boot] boot_flag != 0xAA55\n");
            return -1;
        }
        if (!(le16(img + HDR_XLOADFLAGS) & XLF_KERNEL_64)) {
            KLOG_ERROR("[x86boot] 内核没有 64 位入口（xloadflags=0x%x），"
                       "本 VMM 不走 16 位实模式路径\n", le16(img + HDR_XLOADFLAGS));
            return -1;
        }
        memcpy(hdr, img + HDR_OFF, HDR_HDR_SIZE);

        setup_bytes = ((uint32_t)hdr[HDR_SETUP_SECTS - HDR_OFF] + 1u) * 512u;
        pref        = le64(hdr + (HDR_PREF_ADDRESS - HDR_OFF));
        s_kernel_load = pref ? pref : GUEST_LINUX_KERNEL_GPA;

        KLOG_INFO("[x86boot] bzImage %d bytes, header version 0x%x, "
                  "setup=%u bytes, pref=0x%llx → load@0x%llx, payload@file+%u\n",
                  klen, le16(hdr + (HDR_VERSION - HDR_OFF)), setup_bytes,
                  (unsigned long long)pref, (unsigned long long)s_kernel_load,
                  setup_bytes);
    }

    /* ── 3. 把保护模式内核搬到装载地址（**仅 bzImage**）──
     * 原始内核 ELF 的各段在第 2 步已按 p_paddr 就位，绝不能在这里再搬一次。*/
    {
        uint64_t pm_len = (uint64_t)klen - setup_bytes;
        if ((int64_t)pm_len <= 0) {
            KLOG_ERROR("[x86boot] bzImage 长度异常\n");
            return -1;
        }
        /* 先清掉装载区内可能残留的头副本，再按偏移搬（源在 GPA 0x10000，
         * 目标在 0x100000 或 pref，两者区间可能重叠 → 用 memmove 语义）。*/
        memmove(gpa_ptr(s_kernel_load),
                (const uint8_t *)gpa_ptr(GUEST_LINUX_SETUP_GPA) + setup_bytes,
                pm_len);
        KLOG_INFO("[x86boot] protected-mode kernel: %llu bytes @gpa 0x%llx\n",
                  (unsigned long long)pm_len, (unsigned long long)s_kernel_load);
    }

    /* ── 4. initrd ── */
    ilen = guest_loader_load_file(&vm, GUEST_LINUX_INITRD_PATH,
                                  GUEST_X86_HPA_BASE + GUEST_LINUX_INITRD_GPA);
    if (ilen <= 0) {
        KLOG_ERROR("[x86boot] 无法加载 %s\n", GUEST_LINUX_INITRD_PATH);
        return -1;
    }
    KLOG_INFO("[x86boot] initrd: %d bytes @gpa 0x%llx\n", ilen,
              (unsigned long long)GUEST_LINUX_INITRD_GPA);

    /* ── 5. 命令行 ── */
    cmdline_len = (uint32_t)strlen(GUEST_LINUX_BOOTARGS) + 1;
    memcpy(gpa_ptr(GUEST_LINUX_CMDLINE_GPA), GUEST_LINUX_BOOTARGS, cmdline_len);

    /* ── 6. boot_params ── */
    {
        uint8_t *bp = (uint8_t *)gpa_ptr(GUEST_LINUX_BOOTPARAMS_GPA);

        memset(bp, 0, BP_SIZE);
        memcpy(bp + HDR_OFF, hdr, HDR_HDR_SIZE);   /* setup_header 原样拷入 */

        bp_put8(bp, BP_TYPE_OF_LOADER, 0xff);      /* 0xff = 未知 bootloader */
        /*
         * loadflags 必须带上 CAN_USE_HEAP —— 解压器（extract_kernel）就是
         * 靠它决定去用 boot_params 里声明的那块堆，并据此算出「解压输出
         * 往哪写」。少了这一位，它算出来的目标指针会是空/错值，现象是
         * guest 在 64 位入口之后不久就 write 到 cr2=0（err=0x2 写缺页）
         * 并陷入异常风暴 —— 而同一镜像用 QEMU 自己的 load_linux() 裸跑
         * 完全正常（QEMU 是置了这两位的）。
         * heap_end_ptr 取 QEMU 同款 0x8000-0x200：堆在 setup 区之后。
         */
        bp_put8(bp, BP_LOADFLAGS, BP_LOADED_HIGH | BP_CAN_USE_HEAP);
        /* heap_end_ptr：**原值保留**（tgoskits 同款）。32 位入口的栈就是
         * 由内核用它算出来的（esp = heap_end_ptr + STACK_SIZE - 4），
         * 自己编一个值会把栈挪到别处。*/
        bp_put16(bp, BP_HEAP_END_PTR,
                 le16(hdr + (BP_HEAP_END_PTR - HDR_OFF)));
        bp_put16(bp, BP_SETUP_MOVE_SZ, 0x8000);
        bp_put32(bp, BP_CODE32_START, (uint32_t)s_kernel_load);
        bp_put32(bp, BP_RAMDISK_IMAGE, GUEST_LINUX_INITRD_GPA);
        bp_put32(bp, BP_RAMDISK_SIZE, (uint32_t)ilen);
        bp_put32(bp, BP_CMD_LINE_PTR, GUEST_LINUX_CMDLINE_GPA);
        /*
         * 照搬 tgoskits boot_params.rs 的字段清单：
         *   sentinel(0x1ef)=0xff  ← 内核据此判断「装载器确实填过零页」；
         *                            为 0 时内核会走 sanitize_boot_params()
         *                            把 screen_info/ramdisk 等字段清掉。
         *   ext_cmd_line_ptr(0xc8) / ext_ramdisk_*(0xc0,0xc4)：高位，我们地址 <4G 故为 0。
         */
        bp_put8(bp, 0x1ef, 0xff);
        bp_put32(bp, 0x0c8, 0);
        bp_put32(bp, 0x0c0, 0);
        bp_put32(bp, 0x0c4, 0);
        /*
         * initrd 必须落在 initrd_addr_max 之下 —— 这个字段内核自己会
         * 用它做检查，取头里的值（通常 0x37FFFFFF）即可，不要自己编。
         *
         * ⚠️ 其余头字段（kernel_alignment / cmdline_size / xloadflags /
         * relocatable_kernel）**一律不要改写**：它们是内核自报的能力，
         * 覆盖成我们以为的值会让解压/重定位阶段走进死循环 —— 典型症状
         * 是 guest 在解压后的内核区域里纯 CPU 空转，一次设备访问都没有
         * （直方图上只有宿主 tick）。上面 hdr 已经整块拷进 boot_params，
         * 这里只补「装载器才知道」的那几个字段。
         */

        build_e820(bp);

        {
            extern uint64_t x86_guest_hpa_base(void);
            const uint8_t *h = (const uint8_t *)gpa_ptr(GUEST_LINUX_BOOTPARAMS_GPA);
            KLOG_INFO("[x86boot] hdr: code32=0x%x init_size=0x%x pref=0x%llx "
                      "loadflags=0x%x setup_sects=%u reloc=%u align=0x%x "
                      "cmdline_size=0x%x xload=0x%x\n",
                      le32(h + BP_CODE32_START),
                      le32(h + 0x260 /* init_size */),
                      (unsigned long long)le64(h + 0x258 /* pref_address */),
                      h[BP_LOADFLAGS],
                      h[0x1f1], h[BP_RELOCATABLE], le32(h + BP_KERNEL_ALIGN),
                      le32(h + BP_CMDLINE_SIZE), le16(h + BP_XLOADFLAGS));
        }

        KLOG_INFO("[x86boot] boot_params @gpa 0x%llx: cmdline='%s'\n",
                  (unsigned long long)GUEST_LINUX_BOOTPARAMS_GPA,
                  GUEST_LINUX_BOOTARGS);
    }
    build_mptable();

    /* ── 7. 临时页表 + GDT + 空 IDT/TSS ──

/* ── 7. 临时页表 + GDT + 空 IDT/TSS ──
     * IDT/TSS 全 0 即可：IDT 全 0 = 任何异常都 triple fault（与真机同阶段
     * 行为一致）；TSS 在 guest 自己 lidt/ltr 之前不会被真正使用，但
     * VM-entry 要求 TR base 落在 guest 物理空间内。*/
    build_pgtbl((uint8_t *)gpa_ptr(GUEST_LINUX_PGTBL_GPA));
    build_gdt((uint8_t *)gpa_ptr(GUEST_LINUX_GDT_GPA));
    memset(gpa_ptr(GUEST_LINUX_IDT_GPA), 0, 0x1000);
    memset(gpa_ptr(GUEST_LINUX_TSS_GPA), 0, 0x1000);

    /* ── 8. 建 VM（EPT 会把 guest RAM 映射到宿主窗口、其余置无效）── */
    vm.cfg.mem_base = GUEST_LINUX_MEM_BASE;
    vm.cfg.mem_size = GUEST_LINUX_MEM_SIZE;
    vm.cfg.nr_vcpus = 1;

    if (vm_create(&vm) != 0) {
        KLOG_ERROR("[x86boot] vm_create failed\n");
        return -1;
    }

    /* ── 9. vCPU 入口状态（在 vm_create 之后：vm_init 会 memset vcpu）── */
    {
        vcpu_t *vcpu = &vm.vcpus[0];

        /*
         * ── 入口探针 stub ──
         *
         * 把入口指向一段自己塞的机器码：先往 COM1(0x3F8) 写一个字符，
         * 再跳真正的内核 64 位入口。它把问题一刀切成两半：
         *   看到 'A'   → 长模式/页表/EPT/COM1 PIO 全通，问题在内核内部
         *   看不到 'A' → 入口路径本身没跑起来（#PF 死在更早的地方）
         * 注意 stub 不能碰 %rsi（64 位引导协议里它就是 boot_params）。
         */
        /*
         * 入口探针 stub：默认**关闭**（走真入口）。
         *
         * 打开方式：把下面的 #define 改成 1。它把入口指向一段自写机器码，
         * 用「有没有 reason=30（PIO exit）」当信号灯，可以在**不改 VMM
         * 逻辑**的前提下验证任何寄存器/内存假设 —— 调试期靠它证明了
         * 「入口/长模式/页表/EPT/COM1 通路都正常」「%rsi=0x70000 正确」
         * 「0x70000/0x2000000/0x2500000 三个地址都可读」。
         */
#define GUEST_X86_ENTRY_PROBE 0
        /*
         * 入口 RIP：**无条件**设置。
         *
         * ⚠️ 这里踩过一次：g_rip 原来只写在 `#if GUEST_X86_ENTRY_PROBE`
         * 分支里，探针一关，g_rip 就保持 0，guest 从地址 0 开始执行 →
         * 第 1 次 VM-exit 就是 triple fault（reason=2, rip=0x10000），
         * 串口一个字都没有、看起来像"整个 VMM 没起来"。探针只是可选的
         * 跳板，入口本身与它无关。
         */
        /*
         * ── 入口：走 **Linux 32 位引导协议**，不是 64 位直启 ──
         *
         * 对标 tgoskits（`linux_boot.rs`）：进 32 位保护模式、跳到
         * `code32_start`（即解压器的 `startup_32`，它在本镜像里 rva=0），
         * 唯一要传的参数是 `esi = boot_params`。**32→64 位切换、GDT、
         * 临时页表、identity map、5 级页表探测全部由内核自己的
         * `startup_32`/`startup_64` 完成** —— 与 QEMU 裸跑走的**完全同一条路**。
         *
         * 为什么放弃 64 位直启：那条路要求装载器把环境"拼"对（我们逐字节
         * 比对了 7 个地址、压缩数据、代码布局都一致，解压器仍然在
         * `inflate_fast` 里跑飞），说明还有某个直启契约没满足。换成内核
         * 自己的路径后，环境一致性由构造保证。
         *
         * g_cr3 = 0 是本文件的约定：**非零 = 64 位直启，0 = 32 位协议**
         * （vmx.c 的 vmx_vcpu_setup 据此设 guest 状态，这样不用改头文件）。
         */
        vcpu->g_rip = s_kernel_load;          /* bzImage: startup_32 / ELF: e_entry */
        vcpu->g_cr3 = 0;                      /* 分页关闭（32 位保护模式）*/
        vcpu->regs.rsi = GUEST_LINUX_BOOTPARAMS_GPA;   /* esi = boot_params */
        vcpu->regs.rbx = 0;                   /* 32 位协议要求 ebx/edi/ebp=0 */
        vcpu->regs.rdi = 0;
        vcpu->regs.rbp = 0;

#if GUEST_X86_ENTRY_PROBE
        {
            uint8_t *st = (uint8_t *)gpa_ptr(GUEST_LINUX_PROBE_GPA);
            uint64_t ent = s_kernel_load + CODE64_OFFSET;
            int i = 0;
            /* 探针1：入口活着 */
            st[i++] = 0xba; st[i++] = 0xf8; st[i++] = 0x03;  /* mov $0x3f8,%dx */
            st[i++] = 0xb0; st[i++] = 'A';
            st[i++] = 0xee;
            /*
             * 探针2..4：测三个关键地址的**可读性**（读安全、失败即 triple
             * fault，可观测）。每读通一个就写一个字符，靠 reason=30 的
             * 次数就能判断卡在哪个地址 —— 把「内核里的指针为空」与
             * 「页表/EPT 覆盖有洞」彻底分开。
             *   '2' = boot_params (0x70000)
             *   '3' = 装载地址附近 (0x2000000)
             *   '4' = init_size 尾部 (0x2500000)
             */
            /*
             * 探针2：把 boot_params 关键字段的**低两字节**按 ASCII hex
             * 打到串口（每个字段前先打一个标签字符）。这样日志里能直接
             * 读出数值，不用再猜「非零/为零」。
             * 标签：i=init_size  h=heap_end_ptr  l=loadflags  e=e820_entries
             */
            {
                static const uint32_t fld[4] = {0x260, 0x224, 0x211, 0x1e8};
                static const uint8_t  tag[4] = {'i', 'h', 'l', 'e'};
                /* 打印 %bl 的低两字节（先高字节后低字节，各两位 hex）*/
                static const uint8_t hexseq[] = {
                    0x88,0xd8, 0x24,0x0f, 0x3c,0x0a, 0x72,0x02, 0x04,0x07,
                    0x04,0x30, 0xee,                       /* 低半字节 → 先存起来? 简化：逐 nibble 直接打 */
                };
                (void)hexseq;
                for (int k = 0; k < 4; k++) {
                    /* 标签 */
                    st[i++] = 0xb0; st[i++] = tag[k]; st[i++] = 0xee;
                    /* 取字段到 %rbx */
                    st[i++] = 0x48; st[i++] = 0xb8;
                    { uint64_t bp = GUEST_LINUX_BOOTPARAMS_GPA; memcpy(st+i,&bp,8); i+=8; }
                    st[i++] = 0x48; st[i++] = 0x8b; st[i++] = 0x98;
                    memcpy(st+i, &fld[k], 4); i += 4;
                    /* 打高字节（bits 15:8）再打低字节（bits 7:0）*/
                    for (int half = 0; half < 2; half++) {
                        /* %al = (value >> (half?0:8)) & 0xff */
                        st[i++] = 0x88; st[i++] = 0xd8;                 /* mov %bl,%al */
                        if (half == 0) { st[i++]=0xc0; st[i++]=0xe8; st[i++]=0x08; } /* shr $8,%al */
                        /* 高 nibble */
                        st[i++] = 0xd0; st[i++] = 0xe8;                 /* shr $1,%al */
                        st[i++] = 0xd0; st[i++] = 0xe8;
                        st[i++] = 0xd0; st[i++] = 0xe8;
                        st[i++] = 0xd0; st[i++] = 0xe8;
                        st[i++] = 0x24; st[i++] = 0x0f;                 /* and $0xf,%al */
                        st[i++] = 0x3c; st[i++] = 0x0a;                 /* cmp $10,%al */
                        st[i++] = 0x72; st[i++] = 0x02;
                        st[i++] = 0x04; st[i++] = 0x07;
                        st[i++] = 0x04; st[i++] = 0x30;
                        st[i++] = 0xee;
                        /* 低 nibble */
                        st[i++] = 0x88; st[i++] = 0xd8;
                        if (half == 0) { st[i++]=0xc0; st[i++]=0xe8; st[i++]=0x08; }
                        st[i++] = 0x24; st[i++] = 0x0f;
                        st[i++] = 0x3c; st[i++] = 0x0a;
                        st[i++] = 0x72; st[i++] = 0x02;
                        st[i++] = 0x04; st[i++] = 0x07;
                        st[i++] = 0x04; st[i++] = 0x30;
                        st[i++] = 0xee;
                    }
                }
            }
            {
                static const uint64_t probe_addrs[3] = {
                    0x00070000ULL, 0x02000000ULL, 0x02500000ULL
                };
                static const uint8_t probe_ch[3] = { '2', '3', '4' };
                for (int k = 0; k < 3; k++) {
                    st[i++] = 0x48; st[i++] = 0xb8;              /* movabs $imm64,%rax */
                    memcpy(st + i, &probe_addrs[k], 8); i += 8;
                    st[i++] = 0x48; st[i++] = 0x8b; st[i++] = 0x18; /* mov (%rax),%rbx */
                    st[i++] = 0xb0; st[i++] = probe_ch[k];
                    st[i++] = 0xee;
                }
            }
            {
                uint64_t ent = s_kernel_load + CODE64_OFFSET;
                st[i++] = 0x48; st[i++] = 0xb8;
                memcpy(st + i, &ent, 8); i += 8;
                st[i++] = 0xff; st[i++] = 0xe0;
            }
            vcpu->g_rip = GUEST_LINUX_PROBE_GPA;
            KLOG_INFO("[x86boot] probe stub @0x%llx -> real entry 0x%llx\n",
                      (unsigned long long)GUEST_LINUX_PROBE_GPA,
                      (unsigned long long)ent);
        }
#endif /* GUEST_X86_ENTRY_PROBE */
        if (vcpu->g_cr3 != 0) {
            /* 64 位直启路径（保留）：自带临时页表，栈在 190 MiB 处 */
            vcpu->g_rsp = GUEST_LINUX_STACK_GPA + 0x1000;
            vcpu->g_cr3 = GUEST_LINUX_PGTBL_GPA;   /* GPA：EPT 会翻译 */
        } else {
            /* 32 位协议：分页关闭，栈只要在低端可用内存里即可。
             * 内核的 startup_32 会立刻用 boot_params->hdr.scratch 重设 esp。*/
            vcpu->g_rsp = 0x8000;
        }
        vcpu->g_gdt_base = GUEST_LINUX_GDT_GPA;
        vcpu->g_gdt_limit = 4 * 8 - 1;   /* 4 项：空/0x08/0x10/0x18 */
        vcpu->g_boot_linux = 1;
        /*
         * 64 位引导入口的两个参数（缺一不可）：
         *   %rsi = boot_params 物理地址
         *   %rdi = **解压输出缓冲区**的地址
         *
         * 少了 %rdi 的后果极具误导性：解压器拿到 output=NULL，于是它那段
         * 逐字拷贝循环 `movzwl (%rbp,%rcx,2) → mov %r8w,(%r11,%rcx,2)`
         * 一路往地址 0 写 —— 实测 cr2=0、err=0x2（写缺页），随后陷进
         * 异常循环、串口一个字都没有。而 QEMU 自己的 32 位路径会把这个
         * 缓冲区准备好，所以同一镜像裸跑完全正常。
         * 输出地址就是内核装载地址（pref_address）。
         */
        vcpu->regs.rflags = 0x2;                        /* IF=0，初始不开中断 */

        KLOG_INFO("[x86boot] entry=0x%llx rsi(boot_params)=0x%llx cr3=0x%llx\n",
                  (unsigned long long)vcpu->g_rip,
                  (unsigned long long)vcpu->regs.rsi,
                  (unsigned long long)vcpu->g_cr3);
    }

    /*
     * ── 9b. 建 VMCS ──
     *
     * 玩具 guest（VMM_TEST）是测试自己调 vmx_vcpu_setup 的，Linux 路径没有
     * 人替它调 —— 而 x86 的 guest 状态**全在 VMCS 里**（RIP/CR3/段/GDT…），
     * 不建的话 vmlaunch 时 VMCS 是空的，vmptrld 直接失败：
     *   [VMX] vmptrld failed ... / guest entry failed
     * 症状看着像"VMX 坏了"，其实只是没人建表。
     * entry 参数在 g_boot_linux 路径下不用（RIP 取自 vcpu->g_rip），传 0 即可。
     */
    if (vmx_vcpu_setup(&vm.vcpus[0], 0) != 0) {
        KLOG_ERROR("[x86boot] vmx_vcpu_setup failed\n");
        return -1;
    }

    /* ── 10. 交给 vCPU 任务 ── */
    {
        task_t *vt = vcpu_task_create(&vm.vcpus[0], 5);
        if (!vt) {
            KLOG_ERROR("[x86boot] vcpu_task_create failed\n");
            return -1;
        }
        KLOG_INFO("[x86boot] Linux vCPU task id=%u running\n", vt->id);
    }
    return 0;
}

#endif /* ARCH_X86_64 */
