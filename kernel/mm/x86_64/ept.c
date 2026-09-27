/*
 * kernel/mm/x86_64/ept.c — x86_64 EPT (Extended Page Table) 实现
 *
 * 移植自 x-kernel: virt/kvmm/src/mm/ept.rs，适配 Avatar OS。
 *
 * ── 每 VM 一份 + 按需分页 ────────────────────────────────────
 *
 * 老实现是一组全局页表 + 全 4 GiB 预建 identity map（"先全映射再把非 RAM 清
 * 成无效"），只能有一个 VM，而且 guest RAM 必须先整段预留。现在是：
 *
 *   - 每个 VM 一个 ept_ctx_t；静态 PML4/PDPT/PD 按 slot 索引（EPT_MAX_VMS 份）；
 *   - init 之后表是**空的** —— "空表即全 trap"，不再需要 enable_mmio_trap；
 *   - guest RAM 靠**缺页驱动分配**：没人预映射，guest 第一次访问某页时
 *     VMM 在 EPT violation 处理里调 x86_ept_map_page/block() 现分配一个宿主
 *     物理页；
 *   - 宿主自己要写的几块（bzImage/initrd/boot_params/初始页表）在加载期用
 *     x86_ept_map_range() 显式映射。
 *
 * 页表布局（4 级）：
 *   PML4[0] → PDPT
 *   PDPT[i] (i<4, 每项 1 GiB) → PD[i]
 *   PD[i][j] (512 项 × 2 MiB) —— guest RAM 窗口内的那几项是"指向 PT 的表项"，
 *                                窗口外一律留空（访问 → EPT violation）
 *   PT：每个 2 MiB 块一张，**按需从 PMM 分配**，叶项是 4 KiB 页
 *
 * 记账不需要额外位图：pt_tbl[i] 非空就说明"该 2 MiB 块有页"，销毁时遍历每张
 * 表的每个有效表项即可把按需页精确还回去（与 stage2.c / gstage.c 同款）。
 *
 * ⚠️ x86 与另外两个架构有一处**语义**差异值得记一笔：guest 的 GPA 必须从 0
 * 开始（Linux 假定低端有常规内存、内核装载在 1 MiB），而宿主的物理 0 不能给
 * 它。老实现靠 EPT 的线性偏移（hpa = hpa_base + gpa）一次解决；按需分页之后
 * 每个 GPA 各自映射到一个现分配的宿主页，这个约束自然满足，**不再需要
 * hpa_base**。
 */

#include "x86_64/ept.h"
#include "pmm.h"
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

/* ── 静态表：每个 VM 一组（BSS，4KiB 对齐）───────────────────
 *
 * EPT 各级表都要求 4 KiB 对齐，且**每个 VM 必须各自独占**——写成
 * `[MAX_VMS][N]` 却不保证对齐、或让某个 slot 的起点落在别人页内，硬件就会
 * 走到错误的表上。这个坑在 aarch64（VTTBR 对齐）和 riscv（hgatp 16 KiB
 * 对齐）两边都实测踩过，症状统一是"映射加到了 A 表，硬件在用 B 表"。
 */
static uint64_t g_ept_pml4[EPT_MAX_VMS][EPT_PDPT_ENTRIES]
    __attribute__((aligned(4096)));
static uint64_t g_ept_pdpt[EPT_MAX_VMS][EPT_PDPT_ENTRIES]
    __attribute__((aligned(4096)));
static uint64_t g_ept_pd[EPT_MAX_VMS][EPT_L1_ENTRIES][EPT_PD_ENTRIES]
    __attribute__((aligned(4096)));

/* ── 内部工具 ─────────────────────────────────────────────── */

static inline int pdpt_index(uint64_t gpa) { return (int)((gpa >> 30) & 0x1FF); }
static inline int pd_index(uint64_t gpa)   { return (int)((gpa >> 21) & 0x1FF); }
static inline int pt_index(uint64_t gpa)   { return (int)((gpa >> 12) & 0x1FF); }

/*
 * RAM 窗口内的 2 MiB 块序号（0..EPT_MAX_PT_TABLES-1），窗口外返回 -1。
 *
 * ⚠️ 这个序号**故意与 PD 下标解耦**：RAM 窗口起点不保证 1 GiB 对齐
 * （aarch64 的 guest 在 0x70000000、riscv 在 0xA0000000；x86 虽然从 0 开始，
 * 但保持同一形状，省得三份代码长得不一样）。直接拿它当 PD 下标会算错。
 */
static int ram_blk_index(const ept_ctx_t *e, uint64_t gpa)
{
    uint64_t idx;

    if (gpa < e->ram_base || gpa >= e->ram_end)
        return -1;

    idx = (gpa - e->ram_base) / EPT_BLOCK_SIZE;
    return (idx < EPT_MAX_PT_TABLES) ? (int)idx : -1;
}

/*
 * 确保 gpa 所在的 2 MiB 块有一张 PT（没有就从 PMM 现分配）。
 * 返回该表，失败返回 NULL（PMM 没页，或地址不在 RAM 窗口内）。
 */
static uint64_t *pt_ensure(ept_ctx_t *e, uint64_t gpa)
{
    int idx = ram_blk_index(e, gpa);
    int pi, di;
    uint64_t pa;
    uint64_t *t;

    if (idx < 0)
        return NULL;
    if (e->pt_tbl[idx])
        return e->pt_tbl[idx];

    pa = pmm_alloc_pages(g_pmm, 1);
    if (!pa)
        return NULL;
    t = (uint64_t *)phys_to_virt(pa);
    memset(t, 0, EPT_PAGE_SIZE);

    /* 在 PD 里装上指向 PT 的表项（RWX 但**不设** EPT_LARGE：这是表不是大页）*/
    pi = pdpt_index(gpa);
    di = pd_index(gpa);
    e->pd[pi][di] = (pa & EPT_ADDR_MASK) | EPT_RWX;

    e->pt_tbl[idx] = t;
    return t;
}

/* 叶项：4 KiB 页，RAM 语义（WB，可读可写可执行）*/
static inline uint64_t ept_leaf(uint64_t pa)
{
    return (pa & EPT_ADDR_MASK) | EPT_RWX | EPT_MT_WB;
}

/* ── 生命周期 ─────────────────────────────────────────────── */

void x86_ept_vm_init(ept_ctx_t *e, uint32_t slot,
                     uint64_t ram_base, uint64_t ram_size)
{
    int i;

    if (slot >= EPT_MAX_VMS) {
        KLOG_ERROR("[ept] invalid slot=%u (max %d)\n", slot, EPT_MAX_VMS);
        return;
    }

    memset(e, 0, sizeof(*e));
    e->slot     = slot;
    e->ram_base = ram_base;
    e->ram_size = ram_size;
    e->ram_end  = ram_base + ram_size;
    e->pml4     = g_ept_pml4[slot];
    e->pdpt     = g_ept_pdpt[slot];

    /* 空表 = 全 trap：没有一条有效映射，任何 GPA 访问都触发 EPT violation */
    memset(e->pml4, 0, sizeof(g_ept_pml4[slot]));
    memset(e->pdpt, 0, sizeof(g_ept_pdpt[slot]));
    memset(g_ept_pd[slot], 0, sizeof(g_ept_pd[slot]));
    for (i = 0; i < EPT_L1_ENTRIES; i++)
        e->pd[i] = g_ept_pd[slot][i];

    /*
     * ⚠️ PML4→PDPT→PD 这几条是"表结构"，不是"映射"，所以即使一张空表也要有。
     * 漏掉的症状极具误导性：pt_ensure()/map_page() 只碰 PD/PT，看上去**完全
     * 正常**（第二次访问同一 GPA 能命中已有表项、还回同一个 HPA），但硬件从
     * PML4 就走进了死胡同 —— guest 于是在同一条取指上无限重复 fault。
     * aarch64 那边就是这么踩过一次的（见 kernel/mm/aarch64/stage2.c）。
     *
     * 保留 4 条而不是只建 RAM 窗口那一条：窗口外的 GPA（设备 0xFEC00000 之类）
     * 因此会走到 PD 级才判定无效，与老实现的"走到 PD 再发现项是 0"一致。
     */
    e->pml4[0] = (virt_to_phys(e->pdpt) & EPT_ADDR_MASK) | EPT_RWX;
    for (i = 0; i < EPT_L1_ENTRIES; i++)
        e->pdpt[i] = (virt_to_phys(e->pd[i]) & EPT_ADDR_MASK) | EPT_RWX;

    /* root_pa | (page_walk_length-1)<<3 | memory_type（4 级 → 3；WB → 6）*/
    e->eptp = (virt_to_phys(e->pml4) & EPT_ADDR_MASK) | (3ULL << 3) | 6ULL;

    KLOG_INFO("[ept] vm slot=%u mem=0x%llx+0x%llx pml4_pa=0x%llx eptp=0x%llx\n",
              slot,
              (unsigned long long)ram_base, (unsigned long long)ram_size,
              (unsigned long long)virt_to_phys(e->pml4),
              (unsigned long long)e->eptp);
}

void x86_ept_vm_destroy(ept_ctx_t *e)
{
    uint64_t freed = 0;
    int i, j;

    if (!e || !e->pml4)
        return;

    for (i = 0; i < EPT_MAX_PT_TABLES; i++) {
        uint64_t *t = e->pt_tbl[i];
        if (!t)
            continue;

        for (j = 0; j < EPT_PT_ENTRIES; j++) {
            if (t[j] & EPT_READ) {
                pmm_free_pages(g_pmm, t[j] & EPT_ADDR_MASK, 1);
                t[j] = 0;
                freed++;
            }
        }
        pmm_free_pages(g_pmm, virt_to_phys(t) & EPT_ADDR_MASK, 1);  /* 表页本身 */
        e->pt_tbl[i] = NULL;
    }

    /* 先关掉映射（清表）再刷 —— 顺序反了会短暂留下指向已释放页的映射 */
    memset(e->pml4, 0, sizeof(g_ept_pml4[e->slot]));
    memset(e->pdpt, 0, sizeof(g_ept_pdpt[e->slot]));
    memset(g_ept_pd[e->slot], 0, sizeof(g_ept_pd[e->slot]));
    x86_ept_invept_all(e);

    KLOG_INFO("[ept] vm slot=%u destroyed: %llu pages (%llu KB) reclaimed, "
              "premap=%llu fault=%llu, pmm free=%llu MB\n",
              e->slot, (unsigned long long)freed,
              (unsigned long long)(freed * 4),
              (unsigned long long)e->nr_premap,
              (unsigned long long)e->nr_fault,
              (unsigned long long)(pmm_get_free_pages(g_pmm) * 4 / 1024));

    /*
     * eptp 一并清掉：留着的话下一次 INVEPT 会拿一个已经被清空的 pml4 去刷。
     * 清空之后 x86_ept_invept_all() 会因为 eptp==0 直接返回。
     */
    memset(e, 0, sizeof(*e));
}

uint64_t x86_ept_eptp(const ept_ctx_t *e)
{
    return e ? e->eptp : 0;
}

/* ── 映射 ─────────────────────────────────────────────────── */

int x86_ept_ipa_is_ram(const ept_ctx_t *e, uint64_t gpa)
{
    return (gpa >= e->ram_base && gpa < e->ram_end);
}

uint64_t x86_ept_map_page(ept_ctx_t *e, uint64_t gpa, int zero)
{
    uint64_t *t = pt_ensure(e, gpa);
    int ti = pt_index(gpa);
    uint64_t pa;

    if (!t)
        return 0;

    /* 已映射：幂等（缺页重试、以及"先 map_range 后 fault"都会走到）*/
    if (t[ti] & EPT_READ)
        return t[ti] & EPT_ADDR_MASK;

    pa = pmm_alloc_pages(g_pmm, 1);
    if (!pa)
        return 0;               /* ⚠️ 调用方必须处理，不能当成功继续 */

    if (zero)
        memset(phys_to_virt(pa), 0, EPT_PAGE_SIZE);

    t[ti] = ept_leaf(pa);
    return pa;
}

uint64_t x86_ept_map_block(ept_ctx_t *e, uint64_t gpa, int zero)
{
    uint64_t base = gpa & ~(EPT_BLOCK_SIZE - 1);
    uint64_t *t = pt_ensure(e, base);
    uint64_t n = 0;
    int i;

    if (!t)
        return 0;

    for (i = 0; i < EPT_PT_ENTRIES; i++) {
        uint64_t pa;

        if (t[i] & EPT_READ)
            continue;                   /* 已经映射过 */

        pa = pmm_alloc_pages(g_pmm, 1);
        if (!pa)
            break;                      /* PMM 没页：装多少算多少 */

        if (zero)
            memset(phys_to_virt(pa), 0, EPT_PAGE_SIZE);

        t[i] = ept_leaf(pa);
        n++;
    }
    return n;
}

uint64_t x86_ept_map_range(ept_ctx_t *e, uint64_t gpa, uint64_t size, int zero)
{
    uint64_t off;
    uint64_t n = 0;

    for (off = 0; off < size; off += EPT_PAGE_SIZE) {
        if (x86_ept_map_page(e, gpa + off, zero) == 0) {
            KLOG_ERROR("[ept] map_range failed at gpa=0x%llx "
                       "(+0x%llx of 0x%llx), pmm free=%llu pages\n",
                       (unsigned long long)gpa,
                       (unsigned long long)off, (unsigned long long)size,
                       (unsigned long long)pmm_get_free_pages(g_pmm));
            return n;
        }
        n++;
    }

    /* 批量映射末尾统一刷一次；中途每页都刷会慢到不可用 */
    if (n)
        x86_ept_invept_all(e);
    return n;
}

int x86_ept_lookup(const ept_ctx_t *e, uint64_t gpa, uint64_t *hpa_out)
{
    uint64_t *t;
    int idx;
    uint64_t entry;

    if (!x86_ept_ipa_is_ram(e, gpa))
        return 0;

    idx = ram_blk_index(e, gpa);
    if (idx < 0)
        return 0;

    t = e->pt_tbl[idx];
    if (!t)
        return 0;

    entry = t[pt_index(gpa)];
    if (!(entry & EPT_READ))
        return 0;

    if (hpa_out)
        *hpa_out = (entry & EPT_ADDR_MASK) | (gpa & 0xFFF);
    return 1;
}

void x86_ept_invept_all(const ept_ctx_t *e)
{
    /*
     * INVEPT —— 刷新 EPT 派生 TLB。
     *
     * 工具链不认 `invept` 助记符（需要特定 binutils 配置），手工写编码：
     *     66 0F 38 80 /r
     * /r 的 reg 字段 = 类型（1=单 context，2=全 context），
     *      rm 字段 = 16 字节描述符（低 8 字节是 EPTP）。
     * 这里生成 `invept %rax,(%rdx)` → ModRM=0x02。
     */
    struct { uint64_t eptp; uint64_t rsvd; } __attribute__((aligned(16))) desc;
    uint32_t type = 1;   /* single-context：只刷本 VM 这个 EPTP */

    if (!e || e->eptp == 0)
        return;          /* 还没配置（或已销毁）：没有 TLB 要刷 */

    desc.eptp = e->eptp;
    desc.rsvd = 0;

    /*
     * ⚠️ INVEPT 是**传统编码 + 强制 66 前缀**（SDM: 66 0F 38 80 /r），
     * **没有** VEX 形式。最初写成 VEX（C4 E2 79 80）直接吃了个 #6
     * invalid opcode —— 那套字节是给别的指令用的。
     */
    __asm__ volatile(".byte 0x66,0x0f,0x38,0x80,0x02"   /* invept %rax,(%rdx) */
                     :: "a"(type), "d"(&desc) : "memory");
}
