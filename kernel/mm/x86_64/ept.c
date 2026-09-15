/*
 * kernel/mm/x86_64/ept.c — x86_64 EPT (Extended Page Table) 实现
 *
 * 移植自 x-kernel: virt/kvmm/src/mm/ept.rs，适配 Avatar OS：
 *   - Rust GlobalPage 动态分配 → 静态 BSS 页表（与 aarch64 stage2.c /
 *     riscv64 gstage.c 一致风格，不依赖运行时分配器）
 *   - log::info → KLOG_INFO
 *
 * 页表布局（identity map，GPA→HPA）：
 *   PML4[0] → PDPT
 *   PDPT[i] (i<4, 每项 1 GiB) → PD[i]
 *   PD[i][j] (512 项 × 2 MiB 大页) → 覆盖 [0, 4 GiB)
 */

#include "x86_64/ept.h"
#include "mm_vm.h"
#include "klog.h"
#include "string.h"

/* ── EPT PTE 位定义 ────────────────────────────────────────── */
#define EPT_READ    (1ULL << 0)
#define EPT_WRITE   (1ULL << 1)
#define EPT_EXEC    (1ULL << 2)
#define EPT_RWX     (EPT_READ | EPT_WRITE | EPT_EXEC)

/* 内存类型（位 5:3）*/
#define EPT_MT_UC   (0ULL << 3)   /* Uncacheable（设备）*/
#define EPT_MT_WB   (6ULL << 3)   /* Write-Back（RAM）  */

/* PD 级大页位 */
#define EPT_LARGE   (1ULL << 7)

/* 从 EPT 项提取物理地址（位 12-51）*/
#define EPT_ADDR_MASK  0x000FFFFFFFFFF000ULL

#define L1_ENTRIES  4     /* 覆盖 4 GiB 的 PDPT 项数（每项 1 GiB）*/
#define L2_ENTRIES  512   /* 每个 PD 的项数（每项 2 MiB）         */

/* ── 静态页表（4KiB 对齐）─────────────────────────────────── */
static uint64_t g_ept_pml4[512]                __attribute__((aligned(4096)));
static uint64_t g_ept_pdpt[512]                __attribute__((aligned(4096)));
static uint64_t g_ept_pd[L1_ENTRIES][L2_ENTRIES] __attribute__((aligned(4096)));

/* guest RAM 范围与 HPA 基址 */
static uint64_t s_mem_base;
static uint64_t s_mem_size;
static uint64_t s_hpa_base;

/* ── 公开 API ─────────────────────────────────────────────── */

void x86_ept_init(uint64_t mem_base, uint64_t mem_size, uint64_t hpa_base)
{
    uint64_t mem_end = mem_base + mem_size;
    int i, j;

    s_mem_base = mem_base;
    s_mem_size = mem_size;
    s_hpa_base = hpa_base;

    memset(g_ept_pml4, 0, sizeof(g_ept_pml4));
    memset(g_ept_pdpt, 0, sizeof(g_ept_pdpt));

    /* PML4[0] → PDPT */
    g_ept_pml4[0] = virt_to_phys(g_ept_pdpt) | EPT_RWX;

    for (i = 0; i < L1_ENTRIES; i++) {
        /* PDPT[i] → PD[i] */
        g_ept_pdpt[i] = virt_to_phys(g_ept_pd[i]) | EPT_RWX;

        /* PD 用 2 MiB 大页填满 */
        for (j = 0; j < L2_ENTRIES; j++) {
            uint64_t gpa = ((uint64_t)i << 30) | ((uint64_t)j << 21);
            int is_ram   = (gpa >= mem_base && gpa < mem_end);
            uint64_t mt  = is_ram ? EPT_MT_WB : EPT_MT_UC;
            uint64_t hpa = is_ram ? (hpa_base + (gpa - mem_base)) : gpa;
            g_ept_pd[i][j] = hpa | EPT_RWX | mt | EPT_LARGE;
        }
    }

    KLOG_INFO("[ept] init mem=0x%llx+0x%llx hpa=0x%llx pml4_pa=0x%llx\n",
              (unsigned long long)mem_base, (unsigned long long)mem_size,
              (unsigned long long)hpa_base,
              (unsigned long long)virt_to_phys(g_ept_pml4));
}

uint64_t x86_ept_eptp(void)
{
    /* root_pa | (page_walk_length-1)<<3 | memory_type
     * 4 级页表 → 3；内存类型 6 (WB) */
    return virt_to_phys(g_ept_pml4) | (3ULL << 3) | 6ULL;
}

int x86_ept_gpa_to_hpa(uint64_t gpa, uint64_t *hpa_out)
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
 * x86_ept_enable_mmio_trap — guest RAM 之外的 GPA 全部置为无效
 *
 * 对标 kvmm mm/ept.rs：只映射 guest RAM，其余保持无效 → EPT violation
 * → VM-exit 48 → MMIO 总线模拟。这样 guest 永远碰不到真实宿主设备。
 */
void x86_ept_enable_mmio_trap(void)
{
    uint64_t mem_end = s_mem_base + s_mem_size;
    int i, j;
    int cleared = 0;

    for (i = 0; i < L1_ENTRIES; i++) {
        for (j = 0; j < L2_ENTRIES; j++) {
            uint64_t gpa = ((uint64_t)i << 30) | ((uint64_t)j << 21);

            if (gpa >= s_mem_base && gpa < mem_end)
                continue;

            if (g_ept_pd[i][j] & EPT_READ) {
                g_ept_pd[i][j] = 0;   /* 无效 → 访问即触发 EPT violation */
                cleared++;
            }
        }
    }

    x86_ept_invept_all();

    KLOG_INFO("[ept] MMIO trap enabled: %d non-RAM 2MiB blocks unmapped\n",
              cleared);
}

void x86_ept_invept_all(void)
{
    /*
     * 当前为空实现（no-op）。
     *
     * x86_ept_init() 在**任何 guest 运行之前**调用，此时 CPU 的 EPT 派生
     * TLB 中不可能存在旧项，因此无需 INVEPT。INVEPT 仅在运行时修改 EPT
     * （unmap/remap）后才必需——那属于后续 MMIO/vdev 阶段的工作。
     *
     * 注：本工具链（gcc/as）不识别 `invept` 助记符（operand size mismatch,
     * 需要特定 binutils 配置）；届时实现需用 .byte 手工编码 VEX 形式
     *   VEX.128.66.0F38.W0 80 /r :  invept r32, m128
     * 并且**必须在 QEMU 下验证编码正确性**，故此处不预先放入未经验证的字节。
     */
}
