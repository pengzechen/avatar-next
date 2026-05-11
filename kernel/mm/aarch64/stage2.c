/*
 * kernel/mm/aarch64/stage2.c — AArch64 Stage-2 MMU 实现
 *
 * 移植自 ref/bare-vm/arch/aarch64/stage2.c，适配 Avatar OS：
 *   - printf → KLOG_INFO / KLOG_ERROR
 *   - flush_tlb() 使用 VMALLS12E1IS（刷新所有 VMID Stage-1/2 TLB）
 *
 * 页表布局（identity map，IPA = PA）：
 *   L1: 4 entries × 1 GiB   (covers [0, 4 GiB))
 *   L2: L1_ENTRIES × 512 entries × 2 MiB blocks
 *   L3: 一个 512-entry 表，用于 4KB 精细控制（测试只读页）
 */

#include "aarch64/stage2.h"
#include "mm_vm.h"
#include "klog.h"
#include "string.h"

/* ── 静态页表（BSS，4KB 对齐）────────────────────────────── */
uint64_t s2_l1[S2_L1_ENTRIES]                  __attribute__((aligned(4096)));
uint64_t s2_l2[S2_L1_ENTRIES][S2_L2_ENTRIES]   __attribute__((aligned(4096)));
uint64_t s2_l3_ro[S2_L3_ENTRIES]               __attribute__((aligned(4096)));

/* 当前被拆分为 L3 的 2MB 对齐 IPA（-1 = 无）*/
static uint64_t l3_split_ipa = (uint64_t)-1;

/* guest RAM 范围（由 stage2_init 设置）*/
static uint64_t s_ram_base;
static uint64_t s_ram_end;

/* ── 内部工具 ─────────────────────────────────────────────── */

static void flush_tlb(void)
{
    /* 刷新所有 VMID 的 Stage-1 + Stage-2 TLB（EL1&0 regime）*/
    __asm__ volatile(
        "tlbi vmalls12e1is\n"
        "dsb  ish\n"
        "isb\n"
        ::: "memory"
    );
}

static void flush_ept(void *addr, uint64_t size)
{
    uint64_t p = (uint64_t)addr & ~63ULL;
    uint64_t e = ((uint64_t)addr + size + 63ULL) & ~63ULL;
    for (; p < e; p += 64)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    flush_tlb();
    __asm__ volatile("isb" ::: "memory");
}

static int ipa_is_ram(uint64_t ipa)
{
    return (ipa >= s_ram_base && ipa < s_ram_end);
}

/* ── 将 2MB block 拆为 L3 4KB 精细页表 ───────────────────── */
static void split_l2_to_l3(uint64_t ipa_2mb)
{
    int i1, i2, i3;

    if (l3_split_ipa == ipa_2mb)
        return;

    i1 = (ipa_2mb >> 30) & 0x3;
    i2 = (ipa_2mb >> 21) & 0x1FF;

    for (i3 = 0; i3 < S2_L3_ENTRIES; i3++) {
        uint64_t pa = ipa_2mb + (uint64_t)i3 * 4096;
        s2_l3_ro[i3] = pa | LPAE_PAGE | LPAE_AF | LPAE_SH_IS |
                        LPAE_MATTR_NORM | LPAE_S2AP_RW;
    }
    flush_ept(s2_l3_ro, sizeof(s2_l3_ro));

    s2_l2[i1][i2] = virt_to_phys(s2_l3_ro) | LPAE_VALID | LPAE_TABLE;
    flush_ept(&s2_l2[i1][i2], 8);

    l3_split_ipa = ipa_2mb;
}

/* ── 公开 API ─────────────────────────────────────────────── */

void stage2_init(uint64_t mem_base, uint64_t mem_size)
{
    int i1, i2;

    s_ram_base = mem_base;
    s_ram_end  = mem_base + mem_size;

    KLOG_INFO("[stage2] init mem=0x%llx+0x%llx l1=%p l2=%p\n",
              mem_base, mem_size, s2_l1, s2_l2);

    /* 构建 identity map: IPA = PA，RAM 用普通可缓存属性，其余用 Device */
    for (i1 = 0; i1 < S2_L1_ENTRIES; i1++) {
        s2_l1[i1] = virt_to_phys(s2_l2[i1]) | LPAE_VALID | LPAE_TABLE;
        for (i2 = 0; i2 < S2_L2_ENTRIES; i2++) {
            uint64_t ipa  = ((uint64_t)i1 << 30) | ((uint64_t)i2 << 21);
            uint64_t attr = ipa_is_ram(ipa)
                ? (LPAE_AF | LPAE_SH_IS | LPAE_MATTR_NORM | LPAE_S2AP_RW)
                : (LPAE_AF | LPAE_MATTR_DEV | LPAE_S2AP_RW | LPAE_XN);
            /* L2 block entry: bit[0]=1(valid), bit[1]=0(block, not table) */
            s2_l2[i1][i2] = ipa | attr | LPAE_VALID;
        }
    }
    flush_ept(s2_l1, sizeof(s2_l1));
    flush_ept(s2_l2, sizeof(s2_l2));

    /* 配置 VTCR_EL2 */
    uint64_t vtcr = VTCR_T0SZ(32) | VTCR_SL0(1) | VTCR_TG0_4K |
                    VTCR_SH0_IS | VTCR_IRGN0_WBWA | VTCR_ORGN0_WBWA |
                    VTCR_PS_36BITS;
    __asm__ volatile("msr vtcr_el2, %0" :: "r"(vtcr) : "memory");

    /* 配置 VTTBR_EL2：VMID=1，根页表 = s2_l1 物理地址 */
    uint64_t vttbr = virt_to_phys(s2_l1) | (1ULL << VTTBR_VMID_SHIFT);
    __asm__ volatile("msr vttbr_el2, %0\nisb" :: "r"(vttbr) : "memory");

    KLOG_INFO("[stage2] VTCR=0x%llx VTTBR=0x%llx\n", vtcr, vttbr);
}

uint64_t *stage2_get_l3entry(uint64_t ipa)
{
    uint64_t ipa_2mb = ipa & ~((2ULL << 20) - 1);
    split_l2_to_l3(ipa_2mb);
    return &s2_l3_ro[(ipa >> 12) & 0x1FF];
}

void stage2_set_ro(uint64_t ipa)
{
    uint64_t *e = stage2_get_l3entry(ipa);
    *e = (*e & ~LPAE_S2AP_RW) | LPAE_S2AP_RO;
    flush_ept(e, 8);
}

void stage2_restore(uint64_t ipa)
{
    if (l3_split_ipa == (ipa & ~((2ULL << 20) - 1))) {
        uint64_t *e = stage2_get_l3entry(ipa);
        *e |= LPAE_S2AP_RW;
        flush_ept(e, 8);
    } else {
        int i1 = (ipa >> 30) & 0x3;
        int i2 = (ipa >> 21) & 0x1FF;
        s2_l2[i1][i2] |= LPAE_S2AP_RW;
        flush_ept(&s2_l2[i1][i2], 8);
    }
}
