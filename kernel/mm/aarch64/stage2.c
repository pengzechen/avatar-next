/*
 * kernel/mm/aarch64/stage2.c — AArch64 Stage-2 MMU 实现（每 VM 一份 + 按需分页）
 *
 * 老的实现是一组全局页表 + identity map + "init 后把非 RAM 清成无效"。
 * 那套只能有一个 VM，而且要求 guest RAM 预先分配并整片清零。现在是：
 *
 *   - 每个 VM 一个 s2_ctx_t；静态 L1/L2 按 slot 索引（STAGE2_MAX_VMS 份）；
 *   - init 之后表是**空的** —— "空表即全 trap"，不再需要 enable_mmio_trap；
 *   - guest RAM 靠**缺页驱动分配**：没人预映射，guest 第一次访问某页时
 *     VMM 在缺页处理里调 stage2_map_page() 现分配一个宿主物理页；
 *   - 宿主自己要写的几块（内核映像/DTB/initrd/初始栈）在加载期用
 *     stage2_map_range() 显式映射。
 *
 * 页表布局（VTCR SL0=1, T0SZ=32）：
 *   L1: 4 entries × 1 GiB
 *   L2: 4 × 512 entries × 2 MiB —— RAM 窗口内一律是 table descriptor
 *   L3: 每个 2 MiB 块一张，**按需从 PMM 分配**
 *
 * 记账不需要额外位图：l3_tbl[i] 非空就说明"该 2 MiB 块有页"，销毁时遍历
 * 每张表的每个有效表项即可把按需页精确还回去。
 */

#include "aarch64/stage2.h"
#include "pmm.h"
#include "mm_vm.h"
#include "cache.h"    /* clean_and_invalidate_dcache_range */
#include "barrier.h"  /* barrier_sync / barrier_instr_full */
#include "klog.h"
#include "string.h"

/* ── 静态表：每个 VM 一组（BSS，4KB 对齐）────────────────────
 *
 * L1 只有 32 字节，但 LPAE 要求表按自身大小对齐，统一按 4 KiB 对齐最省心
 * （4 个 VM 各浪费半页，可忽略）。*/
/*
 * ⚠️ 每个 VM 的 L1 必须**各自**占一个 4 KiB 页。
 *
 * VTTBR_EL2 要求根页表 4 KiB 对齐（低 12 位非零时硬件会忽略它们）。
 * 原来写成 `uint64_t g_s2_l1[MAX_VMS][4]` —— 每个 slot 只有 32 字节，
 * 于是 slot 1 落在 base+0x20：VTTBR 截断成 base 后指向的是 **slot 0 的表**。
 *
 * 症状极具误导性（实测 vm2 永远起不来、vm1 完全正常）：
 *   - 打印的 VTTBR 是截断后的值 ⇒ 看着完全正确；
 *   - 软件查表 stage2_lookup() 走 s2->l2（那是好的）⇒ HIT；
 *   - 硬件走 vm1 的 L1/L2 ⇒ 找不到 vm2 的映射 ⇒
 *     ESR.FSC=0x6（translation fault, level 2）反复触发。
 * 也就是说：**映射加到了 A 表，硬件在用 B 表**。
 */
typedef struct {
    uint64_t entry[S2_L1_ENTRIES];
    uint8_t  _pad[4096 - S2_L1_ENTRIES * sizeof(uint64_t)];
} s2_l1_page_t;

static s2_l1_page_t g_s2_l1[STAGE2_MAX_VMS] __attribute__((aligned(4096)));
static uint64_t g_s2_l2[STAGE2_MAX_VMS][S2_L1_ENTRIES][S2_L2_ENTRIES]
                        __attribute__((aligned(4096)));

/* ── 内部工具 ─────────────────────────────────────────────── */

static void ipa_idx(uint64_t ipa, int *i1, int *i2, int *i3)
{
    *i1 = (int)((ipa >> 30) & 0x3);
    *i2 = (int)((ipa >> 21) & 0x1FF);
    *i3 = (int)((ipa >> 12) & 0x1FF);
}

/*
 * RAM 窗口内的 2 MiB 块序号（0..S2_MAX_L3_TABLES-1），窗口外返回 -1。
 *
 * ⚠️ 这个序号**故意与 L2 下标解耦**：RAM 窗口起点不保证 1 GiB 对齐
 * （aarch64 guest 用的是 0x70000000，落在 L2[1][384] 而不是 L2[0][0]），
 * 直接拿它当 L2 下标会算错。
 */
static int ram_blk_index(const s2_ctx_t *s2, uint64_t ipa)
{
    uint64_t idx;

    if (ipa < s2->ram_base || ipa >= s2->ram_end)
        return -1;

    idx = (ipa - s2->ram_base) / S2_BLOCK_SIZE;
    return (idx < S2_MAX_L3_TABLES) ? (int)idx : -1;
}

/*
 * 确保 ipa 所在的 2 MiB 块有一张 L3 表（没有就从 PMM 现分配）。
 * 返回该表，失败返回 NULL（PMM 没页）。
 */
static uint64_t *l3_ensure(s2_ctx_t *s2, uint64_t ipa)
{
    int idx = ram_blk_index(s2, ipa);
    int i1, i2, i3;
    uint64_t pa;
    uint64_t *t;

    if (idx < 0)
        return NULL;
    if (s2->l3_tbl[idx])
        return s2->l3_tbl[idx];

    pa = pmm_alloc_pages(g_pmm, 1);
    if (!pa)
        return NULL;
    t = (uint64_t *)phys_to_virt(pa);
    memset(t, 0, S2_PAGE_SIZE);

    ipa_idx(ipa, &i1, &i2, &i3);
    s2->l2[i1 * S2_L2_ENTRIES + i2] = (pa & ~0xFFFULL) | LPAE_VALID | LPAE_TABLE;
    s2->l3_tbl[idx] = t;
    return t;
}

/* ── 生命周期 ─────────────────────────────────────────────── */

void stage2_vm_init(s2_ctx_t *s2, uint32_t slot, uint32_t vmid,
                    uint64_t ram_base, uint64_t ram_size)
{
    int i;

    if (slot >= STAGE2_MAX_VMS) {
        KLOG_ERROR("[stage2] invalid slot=%u (max %d)\n", slot, STAGE2_MAX_VMS);
        return;
    }

    memset(s2, 0, sizeof(*s2));
    s2->slot     = slot;
    s2->vmid     = vmid & 0xFF;
    s2->ram_base = ram_base;
    s2->ram_size = ram_size;
    s2->ram_end  = ram_base + ram_size;
    s2->l1       = g_s2_l1[slot].entry;
    s2->l2       = &g_s2_l2[slot][0][0];

    /* 空表 = 全 trap：没有一条有效映射，任何 IPA 访问都陷入 EL2 */
    memset(s2->l1, 0, sizeof(g_s2_l1[slot].entry));
    memset(s2->l2, 0, sizeof(g_s2_l2[slot]));

    /*
     * ⚠️ L1 必须指向 L2 —— 这几条 entry 是"表结构"，不是"映射"，所以即使
     * 一张空表也要有。漏掉的症状极具误导性：l3_ensure()/stage2_map_page()
     * 只碰 L2/L3，看上去**完全正常**（第二次访问同一 IPA 能命中已有表项、
     * 还回同一个 PA），但硬件从 L1 就走进死胡同 —— guest 于是在同一条取指
     * 上无限重复 fault：`ifetch fault ipa=0x70200000 -> pa=0x40002000` × N。
     */
    for (i = 0; i < S2_L1_ENTRIES; i++) {
        s2->l1[i] = (virt_to_phys(&g_s2_l2[slot][i][0]) & ~0xFFFULL) |
                    LPAE_VALID | LPAE_TABLE;
    }

    s2->vtcr = VTCR_T0SZ(32) | VTCR_SL0(1) | VTCR_TG0_4K |
               VTCR_SH0_IS | VTCR_IRGN0_WBWA | VTCR_ORGN0_WBWA |
               VTCR_PS_36BITS;
    s2->vttbr = (virt_to_phys(s2->l1) & ~0xFFFULL) |
                ((uint64_t)s2->vmid << VTTBR_VMID_SHIFT);

    stage2_activate(s2);

    KLOG_INFO("[stage2] vm slot=%u vmid=%u mem=0x%llx+0x%llx "
              "l1=0x%llx VTCR=0x%llx VTTBR=0x%llx\n",
              slot, s2->vmid,
              (unsigned long long)ram_base, (unsigned long long)ram_size,
              (unsigned long long)virt_to_phys(s2->l1),
              (unsigned long long)s2->vtcr, (unsigned long long)s2->vttbr);
}

void stage2_vm_destroy(s2_ctx_t *s2)
{
    uint64_t freed = 0;
    int i, j;

    for (i = 0; i < S2_MAX_L3_TABLES; i++) {
        uint64_t *t = s2->l3_tbl[i];
        if (!t)
            continue;

        for (j = 0; j < S2_L3_ENTRIES; j++) {
            if (t[j] & LPAE_VALID) {
                pmm_free_pages(g_pmm, t[j] & ~0xFFFULL, 1);
                t[j] = 0;
                freed++;
            }
        }
        pmm_free_pages(g_pmm, virt_to_phys(t) & ~0xFFFULL, 1);  /* 表页本身 */
        s2->l3_tbl[i] = NULL;
    }

    /* 先关掉映射（清表）再 flush —— 顺序反了会短暂留下指向已释放页的映射 */
    memset(s2->l1, 0, sizeof(g_s2_l1[s2->slot].entry));
    memset(s2->l2, 0, sizeof(g_s2_l2[s2->slot]));
    stage2_tlb_flush_vm(s2);

    KLOG_INFO("[stage2] vm%u destroyed: %llu pages (%llu KB) reclaimed, "
              "premap=%llu fault=%llu, pmm free=%llu MB\n",
              s2->vmid, (unsigned long long)freed,
              (unsigned long long)(freed * 4),
              (unsigned long long)s2->nr_premap,
              (unsigned long long)s2->nr_fault,
              (unsigned long long)(pmm_get_free_pages(g_pmm) * 4 / 1024));

    memset(s2, 0, sizeof(*s2));
}

void stage2_activate(const s2_ctx_t *s2)
{
    uint64_t hcr = 0;

    __asm__ volatile(
        "msr vtcr_el2, %[vtcr]\n"
        "msr vttbr_el2, %[vttbr]\n"
        "mrs %[hcr], hcr_el2\n"
        "orr %[hcr], %[hcr], #1\n"
        "msr hcr_el2, %[hcr]\n"
        "isb\n"
        : [hcr] "+r"(hcr)
        : [vtcr] "r"(s2->vtcr), [vttbr] "r"(s2->vttbr)
        : "memory"
    );
}

/* ── 映射 ─────────────────────────────────────────────────── */

int stage2_ipa_is_ram(const s2_ctx_t *s2, uint64_t ipa)
{
    return (ipa >= s2->ram_base && ipa < s2->ram_end);
}

uint64_t stage2_map_page(s2_ctx_t *s2, uint64_t ipa, int zero)
{
    uint64_t *t = l3_ensure(s2, ipa);
    int i1, i2, i3;
    uint64_t pa;

    if (!t)
        return 0;

    ipa_idx(ipa, &i1, &i2, &i3);

    /* 已映射：幂等（缺页重试、以及"先 map_range 后 fault"都会走到）*/
    if (t[i3] & LPAE_VALID)
        return t[i3] & ~0xFFFULL;

    pa = pmm_alloc_pages(g_pmm, 1);
    if (!pa)
        return 0;               /* ⚠️ 调用方必须处理，不能当成功继续 */

    if (zero)
        memset(phys_to_virt(pa), 0, S2_PAGE_SIZE);

    t[i3] = (pa & ~0xFFFULL) | LPAE_PAGE | LPAE_AF | LPAE_SH_IS |
            LPAE_MATTR_NORM | LPAE_S2AP_RW;
    return pa;
}


/*
 * stage2_map_block — 一次把 ipa 所在的整个 2 MiB 块装好（512 个 4 KiB 页）
 *
 * 为什么需要：按需分页下每次缺页都要一次完整的 EL2 往返（保存/恢复 128 个
 * EL2 系统寄存器 + PMM 分配 + 装表），TCG 下实测约 1.2ms。Linux 启动期间
 * 实测缺页 1447 次 ⇒ 白白多花约 1.7 秒（对照：guest 自报的启动时间只有
 * 1.8 秒，墙上却要 3.6 秒，差额就是它）。
 *
 * 而启动阶段的访问是**密集**的 —— 一次装一整块能把这 1447 次压到几十次。
 * 代价是最多 2 MiB 的过取，相对启动期的密集触碰可以接受（这正是 plan 里
 * 给 4 KiB 粒度留的"测速后再上"的口子）。
 *
 * 注意：块内 512 页是**各自独立分配**的，不要求物理连续 —— 省下的是 EL2
 * 往返次数，不是分配开销。返回本次装上的页数。
 */
uint64_t stage2_map_block(s2_ctx_t *s2, uint64_t ipa, int zero)
{
    uint64_t base = ipa & ~(S2_BLOCK_SIZE - 1);
    uint64_t *t = l3_ensure(s2, base);
    uint64_t n = 0;
    int i;

    if (!t)
        return 0;

    for (i = 0; i < S2_L3_ENTRIES; i++) {
        uint64_t pa;

        if (t[i] & LPAE_VALID)
            continue;                   /* 已经映射过 */

        pa = pmm_alloc_pages(g_pmm, 1);
        if (!pa)
            break;                      /* PMM 没页：装多少算多少 */

        if (zero)
            memset(phys_to_virt(pa), 0, S2_PAGE_SIZE);

        t[i] = (pa & ~0xFFFULL) | LPAE_PAGE | LPAE_AF | LPAE_SH_IS |
               LPAE_MATTR_NORM | LPAE_S2AP_RW;
        n++;
    }
    return n;
}

uint64_t stage2_map_range(s2_ctx_t *s2, uint64_t ipa, uint64_t size, int zero)
{
    uint64_t off;
    uint64_t n = 0;

    for (off = 0; off < size; off += S2_PAGE_SIZE) {
        if (stage2_map_page(s2, ipa + off, zero) == 0) {
            KLOG_ERROR("[stage2] map_range failed at ipa=0x%llx "
                       "(+0x%llx of 0x%llx), pmm free=%llu pages\n",
                       (unsigned long long)ipa,
                       (unsigned long long)off, (unsigned long long)size,
                       (unsigned long long)pmm_get_free_pages(g_pmm));
            return n;
        }
        n++;
    }

    /* 批量映射末尾统一刷一次；中途每页都刷会慢到不可用 */
    if (n)
        stage2_tlb_flush_vm(s2);
    return n;
}

int stage2_lookup(const s2_ctx_t *s2, uint64_t ipa, uint64_t *pa_out)
{
    int i1, i2, i3;
    uint64_t e;

    if (!stage2_ipa_is_ram(s2, ipa))
        return 0;

    ipa_idx(ipa, &i1, &i2, &i3);

    e = s2->l2[i1 * S2_L2_ENTRIES + i2];
    if (!(e & LPAE_VALID) || !(e & LPAE_TABLE))
        return 0;               /* RAM 窗口内只允许 table descriptor */

    if (!s2->l3_tbl[ram_blk_index(s2, ipa)])
        return 0;

    e = s2->l3_tbl[ram_blk_index(s2, ipa)][i3];
    if (!(e & LPAE_VALID))
        return 0;

    if (pa_out)
        *pa_out = (e & ~0xFFFULL) | (ipa & 0xFFF);
    return 1;
}

void stage2_map_device_region(s2_ctx_t *s2, uint64_t ipa, uint64_t pa,
                              uint64_t size)
{
    uint64_t off;

    for (off = 0; off < size; off += S2_PAGE_SIZE) {
        uint64_t *t = l3_ensure(s2, ipa + off);
        int i1, i2, i3;

        if (!t)
            return;
        ipa_idx(ipa + off, &i1, &i2, &i3);
        t[i3] = ((pa + off) & ~0xFFFULL) | LPAE_PAGE | LPAE_AF |
                LPAE_MATTR_DEV | LPAE_S2AP_RW | LPAE_XN;
    }
    stage2_tlb_flush_vm(s2);
}

/* ── TLB 维护 ─────────────────────────────────────────────── */

void stage2_tlb_flush_vm(const s2_ctx_t *s2)
{
    /*
     * ⚠️ 先重写 VTTBR 再发 TLBI：TLBI 是对"当前 VTTBR 指定的 VM"生效的，
     * 而本核可能刚被定时器抢占去跑了另一个 VM 的 vCPU 任务（它一进循环就
     * 改 VTTBR_EL2）。不拉回来的话会出现"改了映射但不生效，偶尔又生效"。
     */
    __asm__ volatile("msr vttbr_el2, %0" :: "r"(s2->vttbr) : "memory");
    barrier_sync();
    __asm__ volatile("tlbi vmalls12e1is" ::: "memory");
    barrier_sync();
    barrier_instr_full();
}

void stage2_tlb_flush_ipa(const s2_ctx_t *s2, uint64_t ipa)
{
    __asm__ volatile("msr vttbr_el2, %0" :: "r"(s2->vttbr) : "memory");
    barrier_sync();
    __asm__ volatile("tlbi ipas2e1is, %0" :: "r"(ipa >> 12) : "memory");
    barrier_sync();
    barrier_instr_full();
}

/* ── 精细权限（测试用）───────────────────────────────────── */

void stage2_set_ro(s2_ctx_t *s2, uint64_t ipa)
{
    uint64_t *t = l3_ensure(s2, ipa);
    int i3 = (int)((ipa >> 12) & 0x1FF);

    if (!t || !(t[i3] & LPAE_VALID))
        return;
    t[i3] = (t[i3] & ~LPAE_S2AP_RW) | LPAE_S2AP_RO;
    stage2_tlb_flush_ipa(s2, ipa);
}

void stage2_restore(s2_ctx_t *s2, uint64_t ipa)
{
    uint64_t *t = l3_ensure(s2, ipa);
    int i3 = (int)((ipa >> 12) & 0x1FF);

    if (!t || !(t[i3] & LPAE_VALID))
        return;
    t[i3] |= LPAE_S2AP_RW;
    stage2_tlb_flush_ipa(s2, ipa);
}
