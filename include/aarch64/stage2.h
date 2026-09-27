/*
 * include/aarch64/stage2.h — AArch64 Stage-2 MMU（IPA→PA）API
 *
 * 移植自 ref/bare-vm/arch/aarch64/include/stage2.h，适配 Avatar OS。
 * 三级 LPAE 页表（VTCR SL0=1，T0SZ=32）。
 *
 * ── 从「全局单份 + identity map」改成「每 VM 一份 + 按需分页」 ──────
 *
 * 老版本是一组全局页表（s2_l1/s2_l2/s2_l3_ro）+ identity map（IPA=PA），
 * 加上一句 stage2_enable_mmio_trap() 把非 RAM 区间清成无效。那套只能有一个
 * VM：第二份会覆盖第一份的映射，而且 guest RAM 必须预先分配、预先清零。
 *
 * 现在：
 *   - 每个 VM 一个 s2_ctx_t，静态表按 slot 索引；
 *   - **init 之后表是空的**（"空表就是全 trap"，不再需要 enable_mmio_trap）；
 *   - guest RAM 不再预映射，**首次访问才分配物理页并装映射**（见 VMM 的
 *     stage-2 缺页处理）。宿主侧要写的那几块（内核映像/DTB/initrd/初始栈）
 *     在加载期显式 stage2_map_range()。
 *
 * IPA 空间在两个 VM 里是**相同**的（guest 的设备树写死了 memory@70000000），
 * 但落到不同的物理页 —— 隔离由此而来。
 */
#ifndef AARCH64_STAGE2_H
#define AARCH64_STAGE2_H

#include "types.h"

/* ── Guest 物理内存默认布局（QEMU virt）──────────────────────
 * 历史遗留：实际用的是 guest_loader.h 的 GUEST_LINUX_MEM_BASE/SIZE。
 * 保留是因为别处可能还在引用。*/
#define GUEST_RAM_BASE   0x40000000ULL   /* 1 GiB（QEMU virt 默认 RAM 起始）*/
#define GUEST_RAM_SIZE   0x08000000ULL   /* 128 MiB                         */

/* ── LPAE 页表项格式 ─────────────────────────────────────────
 * Stage-2 三级页表: VTCR SL0=1 → L1 → L2 → L3
 * T0SZ=32 → IPA[31:0]: L1[1:0]=2bit, L2[8:0]=9bit, L3[8:0]=9bit
 * ─────────────────────────────────────────────────────────── */
#define LPAE_VALID       (1ULL << 0)
#define LPAE_TABLE       (1ULL << 1)   /* L1/L2: 指向下一级表       */
#define LPAE_PAGE        (3ULL << 0)   /* L3 page: bits[1:0]=11     */
#define LPAE_AF          (1ULL << 10)  /* Access Flag               */
#define LPAE_SH_IS       (3ULL << 8)   /* Inner Shareable           */
#define LPAE_MATTR_NORM  (0xFULL << 2) /* Normal WB cacheable       */
#define LPAE_MATTR_DEV   (0x1ULL << 2) /* Device-nGnRE              */
#define LPAE_XN          (1ULL << 54)  /* Execute-never             */
#define LPAE_S2AP_RW     (3ULL << 6)   /* Stage-2 AP: read/write    */
#define LPAE_S2AP_RO     (1ULL << 6)   /* Stage-2 AP: read-only     */

/* ── 页表层级尺寸 ─────────────────────────────────────────── */
/* T0SZ=32: IPA[31:30]=L1(4 entry), IPA[29:21]=L2(512 entry/L1) */
#define S2_L1_ENTRIES   4
#define S2_L2_ENTRIES   512
#define S2_L3_ENTRIES   512

#define S2_PAGE_SIZE    4096ULL
#define S2_BLOCK_SIZE   (2ULL * 1024 * 1024)   /* 一个 L3 表覆盖的粒度 */

/* ── VTCR_EL2 构造宏 ─────────────────────────────────────── */
#define VTCR_T0SZ(n)      ((n) & 0x3f)
#define VTCR_SL0(n)       (((n) & 0x3) << 6)
#define VTCR_IRGN0_WBWA   (1ULL << 8)
#define VTCR_ORGN0_WBWA   (1ULL << 10)
#define VTCR_SH0_IS       (3ULL << 12)
#define VTCR_TG0_4K       (0ULL << 14)
#define VTCR_PS_36BITS    (1ULL << 16)

/* ── VTTBR_EL2 VMID 编码 ────────────────────────────────── */
#define VTTBR_VMID_SHIFT  48

/* ── 多 VM 上限 ──────────────────────────────────────────── */
/* 静态表按 slot 索引；vmm.c 里有 _Static_assert(MAX_VMS <= STAGE2_MAX_VMS) */
#define STAGE2_MAX_VMS   4

/*
 * 每 VM 最多几张 L3 表 = guest RAM 窗口 / 2 MiB。
 * 192 MiB / 2 MiB = 96 —— 与 GUEST_LINUX_MEM_SIZE 对应，
 * guest_loader 那边有 _Static_assert 钉住这个关系。
 */
#define S2_MAX_L3_TABLES 96

/* ── 每 VM 的 stage-2 上下文 ───────────────────────────────── */
typedef struct s2_ctx {
    uint32_t  slot;                     /* 0..STAGE2_MAX_VMS-1（索引静态表）*/
    uint32_t  vmid;                     /* 1..255，写进 VTTBR_EL2          */
    uint64_t  ram_base;                 /* guest RAM 窗口（IPA）           */
    uint64_t  ram_size;
    uint64_t  ram_end;                  /* = ram_base + ram_size           */
    uint64_t  vtcr;                     /* VTCR_EL2 的值                   */
    uint64_t  vttbr;                    /* VTTBR_EL2 的值（含 VMID）       */
    uint64_t *l1;                       /* → 静态 g_s2_l1[slot]            */
    uint64_t *l2;                       /* → 静态 g_s2_l2[slot]            */
    uint64_t *l3_tbl[S2_MAX_L3_TABLES]; /* +1 = 该 2MiB 块已有表；动态分配 */
    uint64_t  nr_premap;                /* 统计：加载期预映射页数           */
    uint64_t  nr_fault;                 /* 统计：按需缺页分配页数           */
} s2_ctx_t;

/* ── 生命周期 ───────────────────────────────────────────── */

/*
 * stage2_vm_init — 为一个 VM 建立**空**的 stage-2 表
 *
 * init 之后表里没有任何有效映射（"空表即全 trap"）：guest 访问任何 IPA 都会
 * 触发 stage-2 fault，由 VMM 的缺页处理决定是模拟 MMIO 还是分配一页 RAM。
 * 调用方接着用 stage2_map_range() 把必须要有的几块（内核映像/DTB/initrd/
 * 初始栈）显式映射进去。
 *
 * @slot: 0..STAGE2_MAX_VMS-1，决定用哪组静态 L1/L2
 * @vmid: 1..255，同一个 VMID 同时只能有一个 VM 在用
 */
void stage2_vm_init(s2_ctx_t *s2, uint32_t slot, uint32_t vmid,
                    uint64_t ram_base, uint64_t ram_size);

/*
 * stage2_vm_destroy — 释放该 VM 的所有按需页与 L3 表
 *
 * 遍历 l3_tbl[]：每个有效表项对应一个按需分配的 4 KiB 页，连同表页本身
 * 一起还回 PMM。**调用前必须确保该 VM 的 vCPU 任务已经退出**，否则它可能
 * 正在用这些页。
 */
void stage2_vm_destroy(s2_ctx_t *s2);

/* 把本 VM 的 VTCR/VTTBR 写进当前核的寄存器（每轮进 guest 前都要）。*/
void stage2_activate(const s2_ctx_t *s2);

/* ── 映射 ───────────────────────────────────────────────── */

/* IPA 是否落在本 VM 的 RAM 窗口内（缺页处理用它区分 RAM 与 MMIO）。*/
int stage2_ipa_is_ram(const s2_ctx_t *s2, uint64_t ipa);

/*
 * stage2_map_page — 为 ipa 分配一个宿主物理页并装上 4 KiB 映射
 *
 * @zero: 非 0 时把新页清零（防跨 VM 数据泄漏 —— 等价于老版本那句
 *        "整片 memset 192 MiB"，只是粒度从"整片"变成"每页"）。
 * 返回映射到的 PA；返回 0 表示失败（PMM 没页了）—— **调用方必须处理失败**，
 * 不能当成功继续（那会映射到 PA 0）。
 */
uint64_t stage2_map_page(s2_ctx_t *s2, uint64_t ipa, int zero);

/*
 * stage2_map_block — 一次装好 ipa 所在的整个 2 MiB 块（512 页）
 *
 * 缺页处理用它替代单页映射：把 EL2 往返次数从"每页一次"降到"每块一次"。
 * 返回本次装上的页数（PMM 不足时会少装）。
 */
uint64_t stage2_map_block(s2_ctx_t *s2, uint64_t ipa, int zero);

/* 批量版：从 ipa 起 size 字节逐页映射，末尾只做一次 TLB 刷新。*/
uint64_t stage2_map_range(s2_ctx_t *s2, uint64_t ipa, uint64_t size, int zero);

/* 查映射：命中返回 1 并填 *pa_out，未映射返回 0。*/
int stage2_lookup(const s2_ctx_t *s2, uint64_t ipa, uint64_t *pa_out);

/*
 * 把一段 IPA 映射到指定 PA（设备 MMIO 用，属性为 Device-nGnRE + XN）。
 * GICv2 后端会用它把 GICD/GICC 直接映射进 guest。
 */
void stage2_map_device_region(s2_ctx_t *s2, uint64_t ipa, uint64_t pa, uint64_t size);

/* ── TLB 维护 ───────────────────────────────────────────────
 *
 * ⚠️ 这两个函数**都会先重写 VTTBR_EL2 再发 TLBI**。原因：TLBI 是"对当前
 * VTTBR 指定的 VM"生效的，而本核可能刚被定时器抢占去跑了另一个 VM 的 vCPU
 * 任务（它一进循环就改 VTTBR_EL2）。不先拉回来的话，会出现"改了映射但不
 * 生效，偶尔又生效"这种最难查的现象。
 */
void stage2_tlb_flush_vm(const s2_ctx_t *s2);            /* 整个 VM */
void stage2_tlb_flush_ipa(const s2_ctx_t *s2, uint64_t ipa);  /* 单个 IPA */

/* ── 精细权限（Stage-2 权限故障测试用）───────────────────── */
void stage2_set_ro(s2_ctx_t *s2, uint64_t ipa);
void stage2_restore(s2_ctx_t *s2, uint64_t ipa);

#endif /* AARCH64_STAGE2_H */
