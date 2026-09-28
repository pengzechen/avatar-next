/*
 * kernel/mm/riscv64/gstage.c — RISC-V H-extension G-stage (Stage-2) 实现
 *
 * 每 VM 一份 + 按需分页。老实现是一组全局页表 + identity map（"先把 4 GiB
 * 全映射再把非 RAM 清成无效"），只能有一个 VM，而且 guest RAM 必须先整段
 * 预留。现在是：
 *
 *   - 每个 VM 一个 gstage_ctx_t；静态 root/L1 按 slot 索引（GSTAGE_MAX_VMS 份）；
 *   - init 之后表是**空的** —— "空表即全 trap"，不再需要 enable_mmio_trap；
 *   - guest RAM 靠**缺页驱动分配**：没人预映射，guest 第一次访问某页时
 *     VMM 在缺页处理里调 rv_gstage_map_page/block() 现分配一个宿主物理页；
 *   - 宿主自己要写的几块（内核映像/DTB/initrd/初始栈）在加载期用
 *     rv_gstage_map_range() 显式映射。
 *
 * 页表布局（Sv39x4）：
 *   root: 2048 项 × 1 GiB（16 KiB），本平台只用 root[0..3]（低 4 GiB）
 *   L1  : 4 × 512 项 × 2 MiB
 *   L0  : 每个 2 MiB 块一张，**按需从 PMM 分配**
 *
 * 记账不需要额外位图：l0_tbl[i] 非空就说明"该 2 MiB 块有页"，销毁时遍历
 * 每张表的每个有效表项即可把按需页精确还回去（与 aarch64 的 stage2.c 同款）。
 *
 * ⚠️ CSR 一律用**数值**地址（hgatp = 0x680）+ 内联汇编。原因见 hext.h 顶部：
 * -march=rv64gc 下汇编器把 `hgatp` 这个名字静默映射到 VS 级编号（0x680 →
 * vsatp 之类），编译全绿、运行期才炸 —— 这是本项目在 riscv 上最贵的一课。
 */

#include "riscv64/gstage.h"
#include "pmm.h"
#include "mm_vm.h"
#include "klog.h"
#include "string.h"

/* ── RISC-V PTE 位（S-stage / G-stage 通用）─────────────────── */
#define PTE_V (1ULL << 0)
#define PTE_R (1ULL << 1)
#define PTE_W (1ULL << 2)
#define PTE_X (1ULL << 3)
#define PTE_U (1ULL << 4)
#define PTE_A (1ULL << 6)
#define PTE_D (1ULL << 7)

/* RAM 页：可读可写可执行。U 位在 G-stage 里不参与权限判定，与老实现保持一致。*/
#define GSTAGE_RAM_FLAGS (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D)

/* PTE 里 PPN 的存放位置：Sv39 是 pte[53:10] = PPN */
#define PTE_PPN_SHIFT 10
#define PTE_PPN_MASK  ((1ULL << 44) - 1)

/*
 * ⚠️ PTE → PA 必须走移位，**不能**像 ARM 的 LPAE 那样直接 `pte & ~0xFFF`。
 *
 * ARM 把物理地址原样放在 pte[47:12]，所以"抹掉低 12 位"就是地址；RISC-V 存
 * 的是 PPN（物理页号）在 pte[53:10]，直接抹低位得到的是 `pa >> 2`。这个差异
 * 造成的故障非常隐蔽：
 *   - 查表返回的 PA 偏小 → 宿主从**错误的物理页**取指（症状是"MMIO 指令
 *     解不出来"、`inst=0x2781` 这种随机值，看着像解码器坏了）；
 *   - 释放路径按错误的地址 pmm_free → 满屏 `PMM: invalid free address`，
 *     而真正的页一页都没还回去。
 * 两处都不会报"翻译错了"，只会报各自的表象。
 */
#define PTE_TO_PA(e) ((((e) >> PTE_PPN_SHIFT) & PTE_PPN_MASK) << 12)

/* ── 静态表：每个 VM 一组（BSS）──────────────────────────────
 *
 * ⚠️ 每个 VM 的 root 必须**各自**独占一个 16 KiB 块。
 *
 * hgatp 要求根页表 16 KiB 对齐（PPN[1:0] 必须为 0，低 12 位被硬件忽略）。
 * 如果写成 `uint64_t g_root[MAX_VMS][2048]` 且只按 8 字节对齐，slot 1 会落在
 * base+0x4000 —— 看着也是 16 KiB 对齐的，但 slot 0/1 会共用同一段物理页的
 * 不同半区……真正致命的是**对齐不够**时 hgatp 截断后指向 slot 0 的表。
 *
 * 这个坑在 aarch64 那边实测踩过（见 kernel/mm/aarch64/stage2.c 里
 * s2_l1_page_t 上方那段）：症状是"映射加到了 A 表，硬件在用 B 表"，
 * 打印出来的 hgatp/VTTBR 完全正确、软件查表也 HIT，只有硬件一遍遍
 * 翻译失败。这里用同样手法防住：结构体补满 16 KiB，数组按 16 KiB 对齐。
 */
typedef struct {
    uint64_t entry[GSTAGE_ROOT_ENTRIES];
    uint8_t _pad[16384 - GSTAGE_ROOT_ENTRIES * sizeof(uint64_t)];
} gstage_root_page_t;

static gstage_root_page_t g_gstage_root[GSTAGE_MAX_VMS]
    __attribute__((aligned(16384)));

/* L1：每个 VM 4 张（低 4 GiB，每张覆盖 1 GiB）*/
static uint64_t g_gstage_l1[GSTAGE_MAX_VMS][4][GSTAGE_L1_ENTRIES]
    __attribute__((aligned(4096)));

/* ── 内部工具 ─────────────────────────────────────────────── */

static inline int root_index(uint64_t gpa)
{
    return (int)((gpa >> 30) & 0x7FF);
}
static inline int l1_index(uint64_t gpa)
{
    return (int)((gpa >> 21) & 0x1FF);
}
static inline int l0_index(uint64_t gpa)
{
    return (int)((gpa >> 12) & 0x1FF);
}

/*
 * RAM 窗口内的 2 MiB 块序号（0..GSTAGE_MAX_L0_TABLES-1），窗口外返回 -1。
 *
 * ⚠️ 这个序号**故意与 L1 下标解耦**：RAM 窗口起点不保证 1 GiB 对齐
 * （本平台 guest 用 0xA0000000，落在 root[2] 的 L1[256] 而不是 L1[0]），
 * 直接拿它当 L1 下标会算错。aarch64 那边同款函数有一样的说明。
 */
static int ram_blk_index(const gstage_ctx_t *g, uint64_t gpa)
{
    uint64_t idx;

    if (gpa < g->ram_base || gpa >= g->ram_end)
        return -1;

    idx = (gpa - g->ram_base) / GSTAGE_BLOCK_SIZE;
    return (idx < GSTAGE_MAX_L0_TABLES) ? (int)idx : -1;
}

/*
 * 确保 gpa 所在的 2 MiB 块有一张 L0 表（没有就从 PMM 现分配）。
 * 返回该表，失败返回 NULL（PMM 没页，或地址不在 RAM 窗口内）。
 */
static uint64_t *l0_ensure(gstage_ctx_t *g, uint64_t gpa)
{
    int idx = ram_blk_index(g, gpa);
    int ri, li;
    uint64_t pa;
    uint64_t *t;

    if (idx < 0)
        return NULL;
    if (g->l0_tbl[idx])
        return g->l0_tbl[idx];

    pa = pmm_alloc_pages(g_pmm, 1);
    if (!pa)
        return NULL;
    t = (uint64_t *)phys_to_virt(pa);
    memset(t, 0, GSTAGE_PAGE_SIZE);

    /* 在 L1 里装上指向 L0 的非叶项（V=1, R=W=X=0）*/
    ri = root_index(gpa);
    li = l1_index(gpa);
    g->l1[ri][li] = ((pa >> 12) << PTE_PPN_SHIFT) | PTE_V;

    g->l0_tbl[idx] = t;
    return t;
}

/* ── 生命周期 ─────────────────────────────────────────────── */

void rv_gstage_vm_init(gstage_ctx_t *g, uint32_t slot, uint32_t vmid,
                       uint64_t ram_base, uint64_t ram_size)
{
    int i;

    if (slot >= GSTAGE_MAX_VMS) {
        KLOG_ERROR("[gstage] invalid slot=%u (max %d)\n", slot, GSTAGE_MAX_VMS);
        return;
    }

    memset(g, 0, sizeof(*g));
    g->slot = slot;
    g->vmid = vmid & 0x3FFF; /* hgatp.VMID 是 14 位 */
    g->ram_base = ram_base;
    g->ram_size = ram_size;
    g->ram_end = ram_base + ram_size;
    g->root = g_gstage_root[slot].entry;

    /* 空表 = 全 trap：没有一条有效映射，任何 GPA 访问都陷入 HS-mode */
    memset(g->root, 0, sizeof(g_gstage_root[slot].entry));
    memset(g_gstage_l1[slot], 0, sizeof(g_gstage_l1[slot]));
    for (i = 0; i < 4; i++)
        g->l1[i] = g_gstage_l1[slot][i];

    /*
     * ⚠️ root 必须指向 L1 —— 这几条 entry 是"表结构"，不是"映射"，所以即使
     * 一张空表也要有。漏掉的症状极具误导性：l0_ensure()/map_page() 只碰
     * L1/L0，看上去**完全正常**（第二次访问同一 GPA 能命中已有表项、还回
     * 同一个 PA），但硬件从 root 就走进死胡同 —— guest 于是在同一条取指上
     * 无限重复 fault。aarch64 那边就是这么踩过一次的
     * （见 kernel/mm/aarch64/stage2.c 的 stage2_vm_init）。
     */
    for (i = 0; i < 4; i++) {
        uint64_t l1_pa = virt_to_phys(g_gstage_l1[slot][i]);
        g->root[i] = ((l1_pa >> 12) << PTE_PPN_SHIFT) | PTE_V;
    }

    g->hgatp = HGATP_MODE_SV39X4 | ((uint64_t)g->vmid << HGATP_VMID_SHIFT) |
               ((virt_to_phys(g->root) >> 12) & HGATP_PPN_MASK);

    rv_gstage_activate(g);

    KLOG_INFO("[gstage] vm slot=%u vmid=%u mem=0x%llx+0x%llx "
              "root_pa=0x%llx hgatp=0x%llx\n",
              slot, g->vmid, (unsigned long long)ram_base,
              (unsigned long long)ram_size,
              (unsigned long long)virt_to_phys(g->root),
              (unsigned long long)g->hgatp);
}

void rv_gstage_vm_destroy(gstage_ctx_t *g)
{
    uint64_t freed = 0;
    int i, j;

    for (i = 0; i < GSTAGE_MAX_L0_TABLES; i++) {
        uint64_t *t = g->l0_tbl[i];
        if (!t)
            continue;

        for (j = 0; j < GSTAGE_L0_ENTRIES; j++) {
            if (t[j] & PTE_V) {
                pmm_free_pages(g_pmm, PTE_TO_PA(t[j]), 1);
                t[j] = 0;
                freed++;
            }
        }
        pmm_free_pages(g_pmm, virt_to_phys(t) & ~0xFFFULL, 1); /* 表页本身 */
        g->l0_tbl[i] = NULL;
    }

    /* 先关掉映射（清表）再 flush —— 顺序反了会短暂留下指向已释放页的映射 */
    memset(g->root, 0, sizeof(g_gstage_root[g->slot].entry));
    memset(g_gstage_l1[g->slot], 0, sizeof(g_gstage_l1[g->slot]));
    rv_gstage_tlb_flush(g);

    KLOG_INFO("[gstage] vm%u destroyed: %llu pages (%llu KB) reclaimed, "
              "premap=%llu fault=%llu, pmm free=%llu MB\n",
              g->vmid, (unsigned long long)freed,
              (unsigned long long)(freed * 4), (unsigned long long)g->nr_premap,
              (unsigned long long)g->nr_fault,
              (unsigned long long)(pmm_get_free_pages(g_pmm) * 4 / 1024));

    memset(g, 0, sizeof(*g));
}

void rv_gstage_activate(const gstage_ctx_t *g)
{
    uint64_t cur;

    if (!g || !g->hgatp)
        return;

    /*
     * 幂等：hgatp 已经是这个值就直接返回。
     *
     * 本函数现在**每次进 guest 前**都会被调一次（见 hext_run.c 的
     * vmm_arch_restore_guest_ctx）—— 因为 hgatp 是 per-hart 的，而
     * vmm_arch_vm_init() 是在 helper（/bin/vmm-run）所在的 hart 上跑的。若每次
     * 都无条件写，顺带的 hfence.gvma 会把整个 G-stage TLB 刷掉，进 guest
     * 的成本白白翻倍。
     *
     * 多 VM 下这个比较自然就分流了：本核可能刚跑过另一个 VM 的 vCPU 任务，
     * 它的 hgatp 与本次不同 ⇒ 重写（这正是我们要的"切 VM 时换页表"）。
     */
    __asm__ volatile("csrr %0, 0x680" : "=r"(cur));
    if (cur == g->hgatp)
        return;

    /* hgatp = CSR 0x680（数值形式，见文件头警告）*/
    __asm__ volatile("csrw 0x680, %0" ::"r"(g->hgatp) : "memory");
    rv_gstage_tlb_flush(g);

    /*
     * ⚠️ DEBUG，不是 INFO。本函数在**每次进入 guest 前**都会被调一次，多 VM
     * 时每轮都在两个 VM 的 hgatp 之间来回切 ⇒ 这行日志会以「每次 guest
     * 入口一条」的速率刷屏。实测两个 VM 跑起来后它占了整个日志的 81%
     * （670/825 行），把真正有用的东西全冲掉了 —— 这是 klog 规范里
     * 「INFO 不得进循环体」那条的典型案例。
     */
    KLOG_DEBUG("[gstage] hgatp=0x%llx activated (vmid=%u)\n",
               (unsigned long long)g->hgatp, g->vmid);
}

/* ── 映射 ─────────────────────────────────────────────────── */

int rv_gstage_ipa_is_ram(const gstage_ctx_t *g, uint64_t gpa)
{
    return (gpa >= g->ram_base && gpa < g->ram_end);
}

uint64_t rv_gstage_map_page(gstage_ctx_t *g, uint64_t gpa, int zero)
{
    uint64_t *t = l0_ensure(g, gpa);
    int li = l0_index(gpa);
    uint64_t pa;

    if (!t)
        return 0;

    /* 已映射：幂等（缺页重试、以及"先 map_range 后 fault"都会走到）*/
    if (t[li] & PTE_V)
        return PTE_TO_PA(t[li]);

    pa = pmm_alloc_pages(g_pmm, 1);
    if (!pa)
        return 0; /* ⚠️ 调用方必须处理，不能当成功继续 */

    if (zero)
        memset(phys_to_virt(pa), 0, GSTAGE_PAGE_SIZE);

    t[li] = ((pa >> 12) << PTE_PPN_SHIFT) | GSTAGE_RAM_FLAGS;
    return pa;
}

/*
 * rv_gstage_map_block — 一次把 gpa 所在的整个 2 MiB 块装好（512 个 4 KiB 页）
 *
 * 为什么需要：按需分页下每次缺页都要一次完整的 HS-mode 往返（保存/恢复
 * vcpu 上下文 + PMM 分配 + 装表）。aarch64 那边实测把启动期的 1447 次缺页
 * 压到 6 次；riscv 的 guest Linux 触摸的内存只多不少，所以同一手法直接照搬。
 *
 * 代价是最多 2 MiB 的过取，相对启动期的密集触碰可以接受。
 * 块内 512 页是**各自独立分配**的，不要求物理连续 —— 省下的是陷入次数，
 * 不是分配开销。返回本次装上的页数。
 */
uint64_t rv_gstage_map_block(gstage_ctx_t *g, uint64_t gpa, int zero)
{
    uint64_t base = gpa & ~(GSTAGE_BLOCK_SIZE - 1);
    uint64_t *t = l0_ensure(g, base);
    uint64_t n = 0;
    int i;

    if (!t)
        return 0;

    for (i = 0; i < GSTAGE_L0_ENTRIES; i++) {
        uint64_t pa;

        if (t[i] & PTE_V)
            continue; /* 已经映射过 */

        pa = pmm_alloc_pages(g_pmm, 1);
        if (!pa)
            break; /* PMM 没页：装多少算多少 */

        if (zero)
            memset(phys_to_virt(pa), 0, GSTAGE_PAGE_SIZE);

        t[i] = ((pa >> 12) << PTE_PPN_SHIFT) | GSTAGE_RAM_FLAGS;
        n++;
    }
    return n;
}

uint64_t rv_gstage_map_range(gstage_ctx_t *g, uint64_t gpa, uint64_t size,
                             int zero)
{
    uint64_t off;
    uint64_t n = 0;

    for (off = 0; off < size; off += GSTAGE_PAGE_SIZE) {
        if (rv_gstage_map_page(g, gpa + off, zero) == 0) {
            KLOG_ERROR("[gstage] map_range failed at gpa=0x%llx "
                       "(+0x%llx of 0x%llx), pmm free=%llu pages\n",
                       (unsigned long long)gpa, (unsigned long long)off,
                       (unsigned long long)size,
                       (unsigned long long)pmm_get_free_pages(g_pmm));
            return n;
        }
        n++;
    }

    /* 批量映射末尾统一刷一次；中途每页都刷会慢到不可用 */
    if (n)
        rv_gstage_tlb_flush(g);
    return n;
}

int rv_gstage_lookup(const gstage_ctx_t *g, uint64_t gpa, uint64_t *pa_out)
{
    uint64_t *t;
    int idx;
    uint64_t e;

    if (!rv_gstage_ipa_is_ram(g, gpa))
        return 0;

    idx = ram_blk_index(g, gpa);
    if (idx < 0)
        return 0;

    t = g->l0_tbl[idx];
    if (!t)
        return 0;

    e = t[l0_index(gpa)];
    if (!(e & PTE_V) || !(e & (PTE_R | PTE_W | PTE_X)))
        return 0; /* 未映射，或不是叶项 */

    if (pa_out)
        *pa_out = PTE_TO_PA(e) | (gpa & 0xFFF);
    return 1;
}

/* ── TLB 维护 ─────────────────────────────────────────────── */

void rv_gstage_tlb_flush(const gstage_ctx_t *g)
{
    (void)g;

    /*
     * hfence.gvma（全刷）+ sfence.vma。两个都是**TLB 失效指令**本身，不是
     * 通用内存屏障，barrier API 里没有对应物，所以保留内联汇编。
     * （别再拿"统一屏障"去替换它们 —— 换掉就丢掉了失效动作。）
     *
     * hfence.gvma 的助记符在 -march=rv64gc 下不被汇编器识别，用 .insn 数值
     * 编码：SYSTEM(0x73), funct3=0, funct7(HFENCE.GVMA)=0x31,
     * rd=x0, rs1=x0(vaddr), rs2=x0(vmid) → 刷新全部 G-stage 映射。
     */
    __asm__ volatile(".insn r 0x73, 0, 0x31, x0, x0, x0\n" /* hfence.gvma */
                     "sfence.vma\n" ::
                         : "memory");
}
