/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/* driver/npu/npu_mmu.c

   RK3588 NPU 的 IOMMU(rockchip,iommu-v2)—— 宿主独占实现。
   为什么要有它, 以及为什么不能让 guest 碰, 见 include/npu/npu_mmu.h 顶上那段。

   寄存器与页表格式抄自 mainline drivers/iommu/rockchip-iommu.c(原生支持 v2)。
   这里只实现我们需要的那一小块：建表, 填表, 开/关翻译, 整表 TLB 失效 ——
   没有 domain 抽象, 没有缺页中断处理, 没有 stall。

   缺页为什么不处理：提交前我们会把整段 guest RAM 里已被 stage-2 映射的页
   全部填进去(npu_mmu_sync), 所以 NPU 会碰到的页都已经在了。真出现缺页说明
   它访问了一个 guest 压根没映射过的地址 —— 那是 guest 那边的问题, 宁可让它
   翻译失败(表里无效项 → 硬件报 BUS_ERROR), 也不要宿主替它猜。
*/

#include "npu/npu_mmu.h"
#include "npu/rknpu.h" /* RKNPU_MAX_CORES：从核基址推 MMU 块位置 */
#include "mmio.h"
#include "klog.h"
#include "mm_vm.h"   /* phys_to_virt / KERNEL_VMA */
#include "cache.h"   /* clean_dcache_range：表页必须冲缓存, 见 npu_mmu_sync */
#include "barrier.h" /* barrier_sync：冲完要 dsb, 否则 NPU 仍可能先读到 */
#include "pmm.h"
#include "platform_cfg.h"
#include "string.h"
#include "vmm/vmm.h" /* vm_t：sync 要查它的 stage-2 */
#include "aarch64/stage2.h"

#if ARCH_AARCH64

/* ── MMU 寄存器块的位置 ───────────────────────────────────────────────

   vendor DTS(imgs/…/linux-gicv3-npu.dts 里没写, 参考
   /tmp/rk3588s.dtsi 的 rknpu_mmu 节点)给的是四段：

       0xFDAB9000  0xFDABA000  0xFDACA000  0xFDADA000   各 0x100

   它们是同一个逻辑 MMU 的四组寄存器(mainline 里同一个 DTE_ADDR 要写进
   每一组), 不是四个独立的 MMU。相对三个核窗口的偏移固定：

       核0 + 0x9000, 核0 + 0xA000, 核1 + 0xA000, 核2 + 0xA000

   注意： 这四段落在我们的透传窗口 0xFDAB0000..0xFDAE0000 之内 —— 也就是说
      guest 现在够得着它们。要让宿主独占, vnpu.c 必须把它们从转发里摘掉
      (见那边的 vnpu_mmu_reg())。
*/
#define NPU_MMU_BANKS 4
static uintptr_t g_mmu_bank[NPU_MMU_BANKS];
static int g_mmu_ready;

/* ── 表 ───────────────────────────────────────────────────────────────

   目录：一张 4 KiB 页 / 1024 个 DTE, 每项管 4 MiB → 覆盖 4 GiB。
   DTE_ADDR 只存一个基址, 所以目录必须物理连续。放静态数组(在核镜像里,
   天然连续), 而不是从 PMM 要 —— 省掉一次"要 4 KiB 连续页"的失败可能。

   PT：每张 4 KiB 页 / 1024 个 PTE。各张不必连续, 所以按需单页从 PMM 拿,
   把 PA 填进 DTE 即可。
*/
#define NPU_MMU_DIR_ENTRIES 1024
static uint32_t g_mmu_dir[NPU_MMU_DIR_ENTRIES] __attribute__((aligned(4096)));

typedef struct {
    uint32_t *pt;    /* 宿主可直接写的虚拟地址；NULL = 这张还没分配 */
    uint64_t pa;     /* 它的物理地址(填 DTE 用) */
    uint32_t filled; /* 里面有多少有效项(诊断用) */
} npu_pt_t;

static npu_pt_t g_mmu_pt[NPU_MMU_DIR_ENTRIES];

/* 本次 enable 覆盖的 IPA 区间 */
static uint64_t g_mmu_ipa_base;
static uint64_t g_mmu_ipa_end;
static int g_mmu_dirty; /* 有改动, 还没发 ZAP_CACHE */
static int g_mmu_sync_logs;

static inline volatile void *bank_reg(int bank, uint32_t off)
{
    return (volatile void *)(g_mmu_bank[bank] + off);
}

static inline void mmu_write(int bank, uint32_t off, uint32_t val)
{
    write32(val, bank_reg(bank, off));
}

static inline uint32_t mmu_read(int bank, uint32_t off)
{
    return read32(bank_reg(bank, off));
}

/* 命令要写进每一组寄存器 —— 否则另外三组不知道表换了 */
static void mmu_cmd_all(uint32_t cmd)
{
    for (int i = 0; i < NPU_MMU_BANKS; i++)
        mmu_write(i, NPU_MMU_COMMAND, cmd);
}

int npu_mmu_init(void)
{
    static const struct {
        int core;
        uint32_t off;
    } spec[NPU_MMU_BANKS] = {
        { 0, 0x9000 }, /* FDAB9000 */
        { 0, 0xA000 }, /* FDABA000 */
        { 1, 0xA000 }, /* FDACA000 */
        { 2, 0xA000 }, /* FDADA000 */
    };

    g_mmu_ready = 0;
    for (int i = 0; i < NPU_MMU_BANKS; i++) {
        char key[8];

        key[0] = 'b';
        key[1] = 'a';
        key[2] = 's';
        key[3] = 'e';
        key[4] = (char)('0' + spec[i].core);
        key[5] = 0;

        uintptr_t base = platform_get_mmio("npu", key);

        if (base == 0)
            return -1; /* 本平台没有 NPU */
        g_mmu_bank[i] = base + spec[i].off;
    }
    g_mmu_ready = 1;
    KLOG_INFO("npu_mmu: %d register banks ready (core0+0x9000/0xa000, core1+0xa000, core2+0xa000)\n", NPU_MMU_BANKS);
    return 0;
}

/* 取(必要时分配)ipa 所在的那张 PT, 返回它的下标；失败返回 -1 */
static int pt_for(uint64_t ipa, int alloc)
{
    uint32_t idx = (uint32_t)(ipa >> 22);
    npu_pt_t *t;

    if (idx >= NPU_MMU_DIR_ENTRIES)
        return -1;
    t = &g_mmu_pt[idx];
    if (t->pt)
        return (int)idx;
    if (!alloc)
        return -1;

    uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
    if (pa == 0)
        return -1;

    t->pt = (uint32_t *)phys_to_virt(pa);
    t->pa = pa;
    t->filled = 0;
    memset(t->pt, 0, 4096);
    g_mmu_dir[idx] = npu_mmu_mk_dte(pa);
    return (int)idx;
}

static inline void pte_set(npu_pt_t *t, uint64_t ipa, uint64_t pa)
{
    uint32_t i = (uint32_t)((ipa >> 12) & 0x3FF);

    if (t->pt[i] == 0)
        t->filled++;
    t->pt[i] = npu_mmu_mk_pte(pa, 1 /* 可写：NPU 要把结果写回去 */);
}

int npu_mmu_enable(uint64_t ipa_base, uint64_t ipa_size)
{
    if (!g_mmu_ready)
        return -1;

    /* 换区间 = 换表：把目录和 PT 全部作废重来。别想着增量复用 ——
       上一轮的 PT 留着会把已经不属于本 VM 的物理页继续映射着。
    */
    for (int i = 0; i < NPU_MMU_DIR_ENTRIES; i++) {
        if (g_mmu_pt[i].pt) {
            pmm_free_pages(&g_pmm, g_mmu_pt[i].pa, 1);
            g_mmu_pt[i].pt = NULL;
        }
        g_mmu_dir[i] = 0;
    }

    g_mmu_ipa_base = ipa_base;
    g_mmu_ipa_end = ipa_base + ipa_size;
    g_mmu_dirty = 1;

    uint64_t dir_pa = virt_to_phys(&g_mmu_dir);

    /* AUTO_GATING 必须关：空闲时它会自作主张省电, 翻译随机失效 ——
       症状是"大部分时候对, 偶尔读到别人的页", 比彻底坏掉难查得多。
    */
    for (int i = 0; i < NPU_MMU_BANKS; i++)
        mmu_write(i, NPU_MMU_AUTO_GATING, 0);

    for (int i = 0; i < NPU_MMU_BANKS; i++) {
        mmu_write(i, NPU_MMU_DTE_ADDR, (uint32_t)dir_pa);

        /*
           回读确认。mainline 里就有这个检查 —— 说明硬件会不认账(写入被
           丢弃), 而"表地址没生效"的症状是全部翻译失败或翻到旧表,
           从别处完全看不出来。
        */
        if (mmu_read(i, NPU_MMU_DTE_ADDR) != (uint32_t)dir_pa) {
            KLOG_ERROR("npu_mmu: bank%d DTE_ADDR readback mismatch (wrote 0x%x, read 0x%x) - address translation "
                       "unavailable\n",
                       i, (uint32_t)dir_pa, mmu_read(i, NPU_MMU_DTE_ADDR));
            return -1;
        }
    }

    mmu_cmd_all(NPU_MMU_CMD_ENABLE_PAGING);
    mmu_cmd_all(NPU_MMU_CMD_ZAP_CACHE);
    g_mmu_dirty = 0;

    /*
       注意： 必须回读 STATUS 确认翻译真的开了。

       mainline 的 rk_iommu_enable_paging() 发完命令就 `readx_poll_timeout`
       等 PAGING_ENABLED 置位, 超时报错 —— 说明这个命令可能不生效。
       而"没生效"和"生效了"从外面看一模一样：MMU 关着时 NPU 按物理地址
       访问, 我们把 IPA→PA 的表建得再对也没用, 它照样什么都读不到
       (真机实测 raw status = 0xc0000000, 和"NPU 没开工"完全一致)。

       第一版就是发完命令就走, 于是连"到底是表错了还是根本没开翻译"
       都分不出来。
    */
    {
        int all_on = 0;

        for (int t = 0; t < 100000 && !all_on; t++) {
            all_on = 1;
            for (int i = 0; i < NPU_MMU_BANKS; i++) {
                if (!(mmu_read(i, NPU_MMU_STATUS) & NPU_MMU_STATUS_PAGING_ENABLED)) {
                    all_on = 0;
                    break;
                }
            }
        }

        if (!all_on)
            KLOG_ERROR(
                "npu_mmu: ENABLE_PAGING did not take effect - PAGING_ENABLED never sets in STATUS. NPU will access "
                "physical addresses and everything will mismatch.\n");
    }

    KLOG_INFO("npu_mmu: translation enabled ipa=[0x%llx, 0x%llx) dir=0x%llx\n", (unsigned long long)ipa_base,
              (unsigned long long)g_mmu_ipa_end, (unsigned long long)dir_pa);

    /* 把四组寄存器的实际状态打出来 —— 这一行本来就有, 只是一直没被调用过。
       它就是"翻译到底开没开, 有没有缺页"的直接判据。
    */
    npu_mmu_dump();
    return 0;
}

void npu_mmu_disable(void)
{
    if (!g_mmu_ready || !g_mmu_ipa_end)
        return;

    /* 注意： 必须真的关掉：宿主自己驱动 NPU 时用的是物理地址,
       翻译还开着的话, 它写的每一个地址都会被再翻译一次 → 全错。
    */
    mmu_cmd_all(NPU_MMU_CMD_DISABLE_PAGING);

    for (int i = 0; i < NPU_MMU_DIR_ENTRIES; i++) {
        if (g_mmu_pt[i].pt) {
            pmm_free_pages(&g_pmm, g_mmu_pt[i].pa, 1);
            g_mmu_pt[i].pt = NULL;
        }
        g_mmu_dir[i] = 0;
    }
    g_mmu_ipa_base = g_mmu_ipa_end = 0;

    KLOG_INFO("npu_mmu: translation disabled (host NPU accesses use physical addresses)\n");
}

/*
   把 stage-2 的映射镜像进来。

   遍历的是整段 guest RAM(不是 NPU 窗口)—— 因为要修的正是"缓冲落在
   窗口外"这件事：现在 NPU 能到哪儿由这张表决定, 不再由恒等窗口决定。

   只填 stage-2 里确实存在的项：guest 没碰过的页本来就不该让 NPU 访问到。
   一次 192 MiB 的 guest RAM 是 49152 次 stage2_lookup, 微秒量级, 可以接受。
*/
int npu_mmu_sync(vm_t *vm)
{
    int added = 0;
    int alloced = 0;
    int mapped = 0; /* 为 NPU 补进 stage-2 的页数 */

    if (!g_mmu_ready || !g_mmu_ipa_end || !vm)
        return 0;

    for (uint64_t ipa = g_mmu_ipa_base; ipa < g_mmu_ipa_end; ipa += 4096) {
        uint64_t pa = 0;
        int idx;

        /*
           注意： stage2_lookup 命中返回 1, 未映射返回 0(见它的声明)——
              不是"0 表示成功"。我按后者写过一版, 结果把所有已映射的页
              全跳过了, 表一个项都没填, NPU 连 regcmd 都读不到
              (raw status 从"跑完了"的 0xc0000151 掉回"什么都没干"的
              0xc0000000)。而日志里只是少了一行, 从别处完全看不出来。
              el2_run.c 那边不看返回值, 只查 pa, 所以没暴露过这个问题。
        */
        /*
           NPU 要读的页, guest 的 CPU 未必碰过：没写过的输出缓冲,
           由 DMA 填进去的权重, 都是驱动分配完就直接交给硬件的。

           这种页在 stage-2 里还没有映射。MMU 关着时这不成问题 —— NPU 按
           物理地址硬读, 读到垃圾但访问"成功", 作业照跑(这也是为什么
           之前 raw status 是 0xc0000151)。开了 MMU 之后同样的访问变成
           硬缺页, 作业当场挂(0xc0000000)。真机实测抓到了：

               bank1 STATUS=0x0000018b  翻译开 缺页(读)

           所以这里给它补上。注意： 补进 stage-2, 不是补进我这张表 ——
           stage-2 是唯一的真值：我这张表要是自己分配页, guest 之后自己
           缺页时会拿到另一个页, 两边永久不一致, 而且两边都不报错。
        */
        if (!stage2_lookup(&vm->s2, ipa, &pa) || pa == 0) {
            (void)stage2_map_page(&vm->s2, ipa, 1 /* 清零：NPU 读之前不该有内容 */);

            /* 重新查一次拿 PA —— 不看 stage2_map_page 的返回值,
               它的约定和上面 stage2_lookup 那个坑是同一类。
            */
            pa = 0;
            if (!stage2_lookup(&vm->s2, ipa, &pa) || pa == 0)
                continue; /* 补不上(PMM 不够), 这一页只能让 NPU 缺页 */
            mapped++;
        }

        int had = (g_mmu_pt[ipa >> 22].pt != NULL);
        idx = pt_for(ipa, 1);
        if (idx < 0)
            continue;
        if (!had)
            alloced++;

        npu_pt_t *t = &g_mmu_pt[idx];

        if (t->pt[(ipa >> 12) & 0x3FF] == 0) {
            pte_set(t, ipa, pa);
            added++;
        }
    }

    /*
       注意：注意： 表页必须冲缓存 —— 这是整件事最关键的一步。

       `g_mmu_dir` 和 PT 都是普通可缓存内存, CPU 写它们走的是 cache。
       而 NPU 的 MMU 是从内存里取表的(设备的总线读不会来看 CPU 的
       cache)—— 不冲的话它看到的是写之前的内容。

       这正是 dma_alloc_coherent 存在的理由, 也是这类设备驱动里最容易漏的
       一步。真机症状极具迷惑性：

         · 缺页地址在同一页内漂移(0x79711540 → 0x79711840 → 0x79711640,
           三次是三个不同的值)。缓存里哪些行碰巧被回写了, 每次不同 →
           MMU 能解出来的项也不同 → 缺的地址就乱跳。
         · 探针说"表是对的", 硬件说"缺页" —— 因为探针读的是缓存里的新值,
           硬件读的是内存里的旧值。两边都没撒谎, 它们看的不是同一份东西。

       顺序：先冲表(让 MMU 能看到), 再 ZAP(让 IOTLB 忘掉旧的), 后清故障。
    */
    if (added) {
        clean_dcache_range(g_mmu_dir, sizeof(g_mmu_dir));
        for (int i = 0; i < NPU_MMU_DIR_ENTRIES; i++) {
            if (g_mmu_pt[i].pt)
                clean_dcache_range(g_mmu_pt[i].pt, 4096);
        }
        /* dsb：确保真的落到内存 —— 只 clean 不屏障, NPU 仍可能先读到 */
        barrier_sync();
    }

    if (added) {
        /* 只在真有改动时失效 —— 一次作业几千页, 每页一次 TLB 失效不可接受,
           整表失效一次则几乎无代价。
        */
        mmu_cmd_all(NPU_MMU_CMD_ZAP_CACHE);
        g_mmu_dirty = 0;
    }

    /*
       每次都报(前两次), 而不是"有变化才报"。

       上面那个反向判断的 bug, 症状就是"日志里少一行" —— 而"少一行"和
       "还没跑到"在日志上长得一模一样。把计数打出来, 0 就是 0。
    */
    if (g_mmu_sync_logs < 2) {
        g_mmu_sync_logs++;
        KLOG_INFO("npu_mmu: synced +%d page(s) (%d new tables, %d pages mapped for NPU), scanned %llu pages\n", added,
                  alloced, mapped, (unsigned long long)((g_mmu_ipa_end - g_mmu_ipa_base) >> 12));
    } else if (added || alloced) {
        KLOG_INFO("npu_mmu: synced +%d page(s) (%d new tables)\n", added, alloced);
    }
    return added;
}

/*
   npu_mmu_probe — 拿一个具体地址对账：我的表翻到哪 vs stage-2 翻到哪。

   为什么需要它：池那块内存本来就是恒等映射的(PA == IPA), 所以 NPU 的表里
   那条翻译应当和 stage-2 一模一样, 开 MMU 对 regcmd 应当是个 no-op。
   真机上开了之后反而从"能跑"(0xc0000151)掉回"什么都没干"(0xc0000000)——
   说明这条翻译不是恒等的, 我表里那个地址翻到别处去了。

     stage2 与 PTE 指向同一个页  → 表是对的, NPU 的访问没走这个 MMU
     两者不同                    → 表错了, 对着两个 PA 就能定位是哪一步
*/
void npu_mmu_probe(vm_t *vm, uint64_t ipa)
{
    uint64_t s2pa = 0;
    uint32_t di, pi, dte, pte;

    if (!g_mmu_ready || !vm || !g_mmu_ipa_end)
        return;
    if (ipa < g_mmu_ipa_base || ipa >= g_mmu_ipa_end) {
        KLOG_WARN("npu_mmu: probe ipa=0x%llx is outside table range [0x%llx, 0x%llx)\n", (unsigned long long)ipa,
                  (unsigned long long)g_mmu_ipa_base, (unsigned long long)g_mmu_ipa_end);
        return;
    }

    int hit = stage2_lookup(&vm->s2, ipa, &s2pa);
    di = (uint32_t)(ipa >> 22);
    pi = (uint32_t)((ipa >> 12) & 0x3FF);
    dte = g_mmu_dir[di];
    pte = g_mmu_pt[di].pt ? g_mmu_pt[di].pt[pi] : 0;

    KLOG_INFO("npu_mmu probe: ipa=0x%llx\n", (unsigned long long)ipa);
    KLOG_INFO("npu_mmu   stage-2: %s pa=0x%llx\n", hit ? "hit" : "unmapped", (unsigned long long)s2pa);
    KLOG_INFO("npu_mmu   my table: DTE[%u]=0x%08x  PTE[%u]=0x%08x  -> pa=0x%llx%s\n", di, dte, pi, pte,
              (unsigned long long)(pte & 0xFFFFF000ULL), (pte & 1U) ? "" : "  (invalid entry)");
    KLOG_INFO("npu_mmu   conclusion: %s\n",
              (pte & 1U) && (pte & 0xFFFFF000ULL) == (s2pa & 0xFFFFF000ULL)
                  ? "both match - table is correct, so NPU accesses did not go through this MMU"
                  : "mismatch - table is wrong");
}

/*
   清掉粘住的缺页(写 PAGE_FAULT_DONE)。四组都要清 —— 缺页是按 bank 记的。
   mainline 在缺页中断处理里就是这么收尾的。
*/
void npu_mmu_clear_fault(void)
{
    if (!g_mmu_ready)
        return;

    /*
       注意： 顺序不能反：先冲 IOTLB, 再清故障。

       mainline 在缺页中断里就是这两行, 这个顺序(rockchip-iommu.c:655-656),
       上面那句注释写得很直白：
           "we always zap cache and clear the page fault anyway"

       因为 IOTLB 里缓存着那次失败的翻译。只清故障状态的话, 那条失败的
       表项还在, MMU 立刻照着它重新报同一个缺页 —— 表里明明已经补好了也没用。

       真机实测就是这个症状：缺页地址始终是同一个 0x79711540,
       而我的表里那条翻译是对的(PTE=0x79711007 -> pa=0x79711000,
       和 stage-2 一致)。清一次, 冲一次, 它才肯重新查表。

       我第一版只写了 PAGE_FAULT_DONE, 而且放在同步之前 —— 顺序正好颠倒。
    */
    mmu_cmd_all(NPU_MMU_CMD_ZAP_CACHE);
    mmu_cmd_all(NPU_MMU_CMD_PAGE_FAULT_DONE);

    /*
       注意： 还要解除 stall：缺页会把 MMU 置于 stall, 而分页在 stall 期间
          是停着的。mainline 的 DISABLE_STALL 注释就是 "Stop stall re-enables
          paging" —— 清了故障不解除 stall, 分页仍然不动。

          真机实测的两个现象合起来指向这里：把清故障修对之后, 缺页地址动了
          (0x79711540 → 0x79711840), 说明 MMU 确实往前走了；但新地址仍然缺页,
          而它和旧地址在同一个 4 KiB 页里, 那一页探针刚确认映射正确。
          → 缺的不是翻译, 是"它还在 stall 里没恢复"。
    */
    mmu_cmd_all(NPU_MMU_CMD_DISABLE_STALL);
}

bool npu_mmu_any_fault(void)
{
    if (!g_mmu_ready)
        return false;

    for (int i = 0; i < NPU_MMU_BANKS; i++) {
        if (mmu_read(i, NPU_MMU_STATUS) & NPU_MMU_STATUS_PAGE_FAULT_ACTIVE)
            return true;
    }
    return false;
}

void npu_mmu_dump(void)
{
    if (!g_mmu_ready) {
        KLOG_INFO("npu_mmu: not initialized\n");
        return;
    }

    KLOG_INFO("npu_mmu: ipa=[0x%llx, 0x%llx) dirty=%d\n", (unsigned long long)g_mmu_ipa_base,
              (unsigned long long)g_mmu_ipa_end, g_mmu_dirty);
    for (int bank = 0; bank < NPU_MMU_BANKS; bank++) {
        uint32_t st = mmu_read(bank, NPU_MMU_STATUS);
        uint32_t dte = mmu_read(bank, NPU_MMU_DTE_ADDR);

        KLOG_INFO("npu_mmu:   bank%d DTE=0x%08x STATUS=0x%08x%s%s%s\n", bank, dte, st,
                  (st & NPU_MMU_STATUS_PAGING_ENABLED) ? " translation on" : " translation off",
                  (st & NPU_MMU_STATUS_PAGE_FAULT_ACTIVE) ? " page fault" : "",
                  (st & NPU_MMU_STATUS_PAGE_FAULT_ACTIVE)
                      ? ((st & NPU_MMU_STATUS_PAGE_FAULT_IS_WRITE) ? "(write)" : "(read)")
                      : "");

        /*
           注意： 这个寄存器一直只定义, 没读过 —— 而它直接记着出错的那个地址。
           表全填满之后仍然报缺页, 那缺的地址就一定在 guest RAM 之外,
           读出来就知道 NPU 到底在要什么。
        */
        if (st & NPU_MMU_STATUS_PAGE_FAULT_ACTIVE)
            KLOG_ERROR(
                "npu_mmu:   bank%d page fault address = 0x%08x %s guest RAM [0x70000000, 0x7c000000) - that is not "
                "in any buffer we provided\n",
                bank, mmu_read(bank, NPU_MMU_PAGE_FAULT_ADDR),
                (mmu_read(bank, NPU_MMU_PAGE_FAULT_ADDR) < 0x70000000U ||
                 mmu_read(bank, NPU_MMU_PAGE_FAULT_ADDR) >= 0x7C000000U)
                    ? "outside"
                    : "still inside");
    }
}

#endif /* ARCH_AARCH64 */
