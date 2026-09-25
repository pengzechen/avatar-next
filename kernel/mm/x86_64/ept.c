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
     * INVEPT —— 刷新 EPT 派生 TLB（影响所有 VPID）。
     *
     * 工具链不认 `invept` 助记符（需要特定 binutils 配置），手工写 VEX：
     *     VEX.128.66.0F38.W0 80 /r   →   C4 E2 79 80 /r
     * /r 的 reg 字段 = 类型（1=单 context，2=全 context），
     *      rm 字段 = 16 字节描述符（低 8 字节是 EPTP）。
     * 这里生成 `invept %rax,(%rdx)` → ModRM=0x02。
     *
     * 什么时候必须调：**运行期改过 EPT 之后**。x86_ept_init() 在任何 guest
     * 运行前调用，那时 TLB 里不可能有旧项，不调也没事；但一旦开始按需
     * map/unmap（MMIO 设备、guest 内存热插拔），漏掉它就会出现「页表改了
     * 但 guest 还看得见旧映射」的幽灵故障。
     */
    struct { uint64_t eptp; uint64_t rsvd; } __attribute__((aligned(16))) desc;
    uint32_t type = 1;   /* single-context：只刷我们自己这个 EPTP */

    if (s_mem_size == 0)
        return;          /* EPT 还没配置：没有 TLB 要刷 */

    desc.eptp = x86_ept_eptp();
    desc.rsvd = 0;

    /*
     * ⚠️ INVEPT 是**传统编码 + 强制 66 前缀**（SDM: 66 0F 38 80 /r），
     * **没有** VEX 形式。最初写成 VEX（C4 E2 79 80）直接吃了个 #6
     * invalid opcode —— 那套字节是给别的指令用的。
     * 这里生成 `invept %rax,(%rdx)`：ModRM=0x02（reg=rax 放类型，
     * rm=rdx 放 16 字节描述符）。
     */
    __asm__ volatile(".byte 0x66,0x0f,0x38,0x80,0x02"   /* invept %rax,(%rdx) */
                     :: "a"(type), "d"(&desc) : "memory");
}
