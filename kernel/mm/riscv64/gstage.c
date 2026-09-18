/*
 * kernel/mm/riscv64/gstage.c — RISC-V H-extension G-stage (Stage-2) MMU 实现
 *
 * 移植自 x-kernel: virt/kvmm/src/mm/gstage.rs，适配 Avatar OS：
 *   - Rust GlobalPage 动态分配 → 静态 BSS 页表（与 aarch64 stage2.c 一致风格）
 *   - log::info → KLOG_INFO
 *   - hgatp 用数值 CSR 0x680（-march=rv64gc 汇编器不识别 hgatp 名字）
 *   - hfence.gvma 用 .insn 编码（同上，汇编器不识别该助记符）
 *
 * 页表布局（Sv39x4 identity map）：
 *   Root: 2048 项 × 16 KiB（4 个连续 4KiB 页，16KiB 对齐），前 4 项有效
 *   L1:   每个 root 项指向一个 512 项表，用 2 MiB 大页 leaf 填充
 */

#include "riscv64/gstage.h"
#include "mm_vm.h"
#include "klog.h"
#include "string.h"

/* ── RISC-V PTE 位（S-stage / G-stage 通用）─────────────────── */
#define PTE_V   (1ULL << 0)
#define PTE_R   (1ULL << 1)
#define PTE_W   (1ULL << 2)
#define PTE_X   (1ULL << 3)
#define PTE_U   (1ULL << 4)
#define PTE_A   (1ULL << 6)
#define PTE_D   (1ULL << 7)

#define L2_ENTRIES  4     /* 覆盖 4 GiB 的 root 项数（每项 1 GiB）*/
#define L1_ENTRIES  512   /* 每个 L1 表的项数（每项 2 MiB）      */
#define ROOT_ENTRIES 2048 /* Sv39x4 root = 16 KiB / 8            */

/* hgatp 字段 */
#define HGATP_MODE_SV39X4  (8ULL << 60)
#define HGATP_VMID_SHIFT   44

/* ── 静态页表（16KiB 对齐 root + 4 个 4KiB 对齐 L1）──────────── */
static uint64_t g_gstage_root[ROOT_ENTRIES]      __attribute__((aligned(16384)));
static uint64_t g_gstage_l1[L2_ENTRIES][L1_ENTRIES] __attribute__((aligned(4096)));

/* guest RAM 范围与 HPA 基址（由 rv_gstage_init 记录，供软件翻译使用）*/
static uint64_t s_mem_base;
static uint64_t s_mem_size;
static uint64_t s_hpa_base;
static uint32_t s_vmid;

/* ── G-stage TLB 刷新 ─────────────────────────────────────────
 * hfence.gvma / sfence.vma 助记符在 -march=rv64gc 下不被识别，
 * 用 .insn 数值编码：SYSTEM(0x73), funct3=0, funct7(HFENCE.GVMA)=0x31,
 * rd=x0, rs1=x0(vaddr), rs2=x0(vmid) → 刷新全部 G-stage 映射。
 */
static inline void gstage_flush(void)
{
    /*
     * 注意：这里的 hfence.gvma / sfence.vma 是**TLB 失效指令**本身，
     * 不是通用内存屏障，barrier API 里没有对应物，因此保留内联汇编。
     * （别再拿"统一屏障"去替换它们 —— 换掉就丢掉了失效动作。）
     */
    __asm__ volatile(
        ".insn r 0x73, 0, 0x31, x0, x0, x0\n"  /* hfence.gvma */
        "sfence.vma\n"
        ::: "memory");
}

/* ── 公开 API ─────────────────────────────────────────────── */

void rv_gstage_init(uint64_t mem_base, uint64_t mem_size,
                    uint64_t hpa_base, uint32_t vmid)
{
    uint64_t mem_end = mem_base + mem_size;
    int i, j;

    s_mem_base = mem_base;
    s_mem_size = mem_size;
    s_hpa_base = hpa_base;
    s_vmid     = vmid;

    /* 清零整个 16KiB root（2048 项）*/
    memset(g_gstage_root, 0, sizeof(g_gstage_root));

    for (i = 0; i < L2_ENTRIES; i++) {
        uint64_t l1_pa = virt_to_phys(g_gstage_l1[i]);

        /* root 项：非 leaf（V=1, R=W=X=0）指向 L1 表 */
        g_gstage_root[i] = ((l1_pa >> 12) << 10) | PTE_V;

        /* L1 用 2 MiB 大页 leaf 填充 */
        for (j = 0; j < L1_ENTRIES; j++) {
            uint64_t gpa = ((uint64_t)i << 30) | ((uint64_t)j << 21);
            int is_ram   = (gpa >= mem_base && gpa < mem_end);
            uint64_t flags = is_ram
                ? (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_U)
                : (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
            uint64_t hpa = is_ram ? (hpa_base + (gpa - mem_base)) : gpa;
            g_gstage_l1[i][j] = ((hpa >> 12) << 10) | flags;
        }
    }

    KLOG_INFO("[gstage] init mem=0x%llx+0x%llx hpa=0x%llx root_pa=0x%llx vmid=%u\n",
              (unsigned long long)mem_base, (unsigned long long)mem_size,
              (unsigned long long)hpa_base,
              (unsigned long long)virt_to_phys(g_gstage_root), vmid);
}

void rv_gstage_activate(void)
{
    uint64_t root_ppn = virt_to_phys(g_gstage_root) >> 12;
    uint64_t hgatp = HGATP_MODE_SV39X4 |
                     ((uint64_t)s_vmid << HGATP_VMID_SHIFT) |
                     root_ppn;

    /* hgatp = CSR 0x680（数值形式，避免汇编器不识别 hgatp 名字）*/
    __asm__ volatile("csrw 0x680, %0" :: "r"(hgatp) : "memory");
    gstage_flush();

    KLOG_INFO("[gstage] hgatp=0x%llx activated\n", (unsigned long long)hgatp);
}

int rv_gstage_gpa_to_hpa(uint64_t gpa, uint64_t *hpa_out)
{
    if (gpa < s_mem_base)
        return 0;
    uint64_t offset = gpa - s_mem_base;
    if (offset >= s_mem_size)
        return 0;
    if (hpa_out)
        *hpa_out = s_hpa_base + offset;
    return 1;
}

/*
 * rv_gstage_enable_mmio_trap — guest RAM 之外的 GPA 全部置为无效
 *
 * 对标 kvmm mm/gstage.rs：只映射 guest RAM，其余（设备 MMIO、未支持 GPA）
 * 保持无效 → G-stage page fault → 陷入 HS-mode → MMIO 总线模拟。
 * 这样 guest 永远碰不到真实宿主设备。
 */
void rv_gstage_enable_mmio_trap(void)
{
    uint64_t mem_end = s_mem_base + s_mem_size;
    int i, j;
    int cleared = 0;

    for (i = 0; i < L2_ENTRIES; i++) {
        for (j = 0; j < L1_ENTRIES; j++) {
            uint64_t gpa = ((uint64_t)i << 30) | ((uint64_t)j << 21);

            if (gpa >= s_mem_base && gpa < mem_end)
                continue;

            if (g_gstage_l1[i][j] & PTE_V) {
                g_gstage_l1[i][j] = 0;   /* 无效 → 访问即触发 G-stage fault */
                cleared++;
            }
        }
    }

    gstage_flush();

    KLOG_INFO("[gstage] MMIO trap enabled: %d non-RAM 2MiB blocks unmapped\n",
              cleared);
}

/*
 * rv_gstage_map_region — 为 GPA 区间建立 identity 映射
 * 用于把某段 IPA 透传到真实硬件（当前未使用，供后续直通设备）。
 */
void rv_gstage_map_region(uint64_t gpa, uint64_t size, uint64_t hpa)
{
    uint64_t start = gpa & ~0x1FFFFFULL;          /* 2MiB 对齐 */
    uint64_t end   = (gpa + size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    uint64_t addr;

    for (addr = start; addr < end; addr += (2ULL << 20)) {
        int i = (int)((addr >> 30) & (L2_ENTRIES - 1));
        int j = (int)((addr >> 21) & (L1_ENTRIES - 1));
        uint64_t cur_hpa = hpa + (addr - gpa);

        g_gstage_l1[i][j] = ((cur_hpa >> 12) << 10)
                          | PTE_V | PTE_R | PTE_W | PTE_A | PTE_D;
    }
    gstage_flush();
}
