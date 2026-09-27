/*
 * include/riscv64/gstage.h — RISC-V H-extension G-stage (Stage-2) MMU
 *
 * 移植自 x-kernel: virt/kvmm/src/mm/gstage.rs
 *
 * G-stage 提供第二级地址翻译 GPA → HPA，通过 hgatp CSR 生效。
 * PTE 格式与 Sv39 完全相同（V/R/W/X/U/G/A/D），只是地址含义从
 * VA→PA 变成 GPA→HPA。
 *
 * 采用 Sv39x4 模式：根页表 16 KiB（2048 项，每项覆盖 1 GiB），共 2 TiB
 * guest 物理地址空间。QEMU virt 上 guest 只用到低 4 GiB，所以 root[0..3]
 * 指向四张 L1 表，每张覆盖 1 GiB。
 *
 * ── 从「全局单份 + identity map」改成「每 VM 一份 + 按需分页」 ──────
 *
 * 老版本是一组全局页表（g_gstage_root/g_gstage_l1）+ identity map，加上
 * 一句 rv_gstage_enable_mmio_trap() 把非 RAM 区间清成无效。那套只能有一个
 * VM：第二份会覆盖第一份的映射，而且 guest RAM 必须预先从 PMM 里预留一整段
 * 连续物理内存（platform.conf 的 guest_ram reserve）。
 *
 * 现在：
 *   - 每个 VM 一个 gstage_ctx_t，静态表按 slot 索引；
 *   - **init 之后表是空的**（"空表就是全 trap"，不再需要 enable_mmio_trap）；
 *   - guest RAM 不再预映射，**首次访问才分配物理页并装映射**（见
 *     hext_run.c 里 scause 20/21/23 的 RAM 缺页分支）。宿主侧要写的那几块
 *     （内核映像/DTB/initrd/初始栈）在加载期显式 rv_gstage_map_range()。
 *
 * GPA 空间在两个 VM 里是**相同**的（guest 的设备树写死了 memory@A0000000），
 * 但落到不同的物理页 —— 隔离由此而来。
 *
 * ⚠️ 与 AArch64 的 stage2.h 是本文件的对偶（那边用 VTTBR_EL2，这边用 hgatp）。
 * 改一边时请对照另一边，两边的手势应当一致。
 */
#ifndef RISCV64_GSTAGE_H
#define RISCV64_GSTAGE_H

#include "types.h"

/* ── Sv39x4 页表几何 ──────────────────────────────────────── */
#define GSTAGE_ROOT_ENTRIES   2048   /* 16 KiB / 8：每项覆盖 1 GiB  */
#define GSTAGE_L1_ENTRIES     512    /* 每项覆盖 2 MiB              */
#define GSTAGE_L0_ENTRIES     512    /* 每项覆盖 4 KiB              */

#define GSTAGE_PAGE_SIZE      4096ULL
#define GSTAGE_BLOCK_SIZE     (2ULL * 1024 * 1024)   /* 一张 L0 表覆盖的粒度 */

/* hgatp 字段（MODE=8 即 Sv39x4；数值形式，见下方警告）*/
#define HGATP_MODE_SV39X4     (8ULL << 60)
#define HGATP_VMID_SHIFT      44
#define HGATP_PPN_MASK        ((1ULL << 44) - 1)

/* ── 多 VM 上限 ───────────────────────────────────────────── */
/* 静态表按 slot 索引；vmm.c 里有 _Static_assert(MAX_VMS <= GSTAGE_MAX_VMS) */
#define GSTAGE_MAX_VMS        4

/*
 * 每 VM 最多几张 L0 表 = guest RAM 窗口 / 2 MiB。
 * 192 MiB / 2 MiB = 96 —— 与 GUEST_LINUX_MEM_SIZE 对应，
 * guest_loader 那边有 _Static_assert 钉住这个关系（与 aarch64 同款）。
 */
#define GSTAGE_MAX_L0_TABLES  96

/*
 * ⚠️ hgatp 一律用**数值** CSR 地址 0x680 访问（本文件只定义常量，实际读写
 * 在 gstage.c 里用内联汇编）。原因见 hext.h 顶部：-march=rv64gc 下汇编器
 * 会把 `hgatp` 这个名字静默映射到 VS 级的编号，编译全绿、运行期才炸。
 */
#define CSR_HGATP_NUM         0x680

/* ── 每 VM 的 G-stage 上下文 ───────────────────────────────── */
typedef struct gstage_ctx {
    uint32_t  slot;                        /* 0..GSTAGE_MAX_VMS-1（索引静态表）*/
    uint32_t  vmid;                        /* 1..255，写进 hgatp.VMID         */
    uint64_t  ram_base;                    /* guest RAM 窗口（GPA）           */
    uint64_t  ram_size;
    uint64_t  ram_end;                     /* = ram_base + ram_size           */
    uint64_t  hgatp;                       /* hgatp 的值（MODE|VMID|root_ppn）*/
    uint64_t *root;                        /* → 静态 g_gstage_root[slot]      */
    uint64_t *l1[4];                       /* → 静态 g_gstage_l1[slot][i]     */
    uint64_t *l0_tbl[GSTAGE_MAX_L0_TABLES];/* +1 = 该 2MiB 块已有表；动态分配 */
    uint64_t  nr_premap;                   /* 统计：加载期预映射页数           */
    uint64_t  nr_fault;                    /* 统计：按需缺页分配页数           */
} gstage_ctx_t;

/* ── 生命周期 ─────────────────────────────────────────────── */

/*
 * rv_gstage_vm_init — 为一个 VM 建立**空**的 G-stage 表
 *
 * init 之后表里没有任何有效映射（"空表即全 trap"）：guest 访问任何 GPA 都会
 * 触发 guest-page fault（scause 20/21/23），由 VMM 的缺页处理决定是模拟
 * MMIO 还是分配一页 RAM。调用方接着用 rv_gstage_map_range() 把必须要有的
 * 那几块（内核映像/DTB/initrd/初始栈）显式映射进去。
 *
 * @slot: 0..GSTAGE_MAX_VMS-1，决定用哪组静态 root/L1
 * @vmid: 1..255，同一个 VMID 同时只能有一个 VM 在用
 *
 * 注：**没有 hpa_base 参数**。按需分页下每页都是各自从 PMM 分配的，GPA 与
 * HPA 之间不再有固定偏移（老版本那句 identity 换算就是这么消失的）。
 */
void rv_gstage_vm_init(gstage_ctx_t *g, uint32_t slot, uint32_t vmid,
                       uint64_t ram_base, uint64_t ram_size);

/*
 * rv_gstage_vm_destroy — 释放该 VM 的所有按需页与 L0 表
 *
 * 遍历 l0_tbl[]：每个有效表项对应一个按需分配的 4 KiB 页，连同表页本身
 * 一起还回 PMM。**调用前必须确保该 VM 的 vCPU 任务已经退出**，否则它可能
 * 正在用这些页。
 */
void rv_gstage_vm_destroy(gstage_ctx_t *g);

/* 把本 VM 的 hgatp 写进当前 hart 的寄存器（每轮进 guest 前都要）。*/
void rv_gstage_activate(const gstage_ctx_t *g);

/* ── 映射 ─────────────────────────────────────────────────── */

/* GPA 是否落在本 VM 的 RAM 窗口内（缺页处理用它区分 RAM 与 MMIO）。*/
int rv_gstage_ipa_is_ram(const gstage_ctx_t *g, uint64_t gpa);

/*
 * rv_gstage_map_page — 为 gpa 分配一个宿主物理页并装上 4 KiB 映射
 *
 * @zero: 非 0 时把新页清零（防跨 VM 数据泄漏）。
 * 返回映射到的 PA；返回 0 表示失败（PMM 没页了）—— **调用方必须处理失败**，
 * 不能当成功继续（那会映射到 PA 0）。
 */
uint64_t rv_gstage_map_page(gstage_ctx_t *g, uint64_t gpa, int zero);

/*
 * rv_gstage_map_block — 一次装好 gpa 所在的整个 2 MiB 块（512 页）
 *
 * 缺页处理用它替代单页映射：把陷入 HS-mode 的往返次数从"每页一次"降到
 * "每块一次"（aarch64 那边实测这一步把 1447 次缺页压到 6 次）。
 * 返回本次装上的页数（PMM 不足时会少装）。
 */
uint64_t rv_gstage_map_block(gstage_ctx_t *g, uint64_t gpa, int zero);

/* 批量版：从 gpa 起 size 字节逐页映射，末尾只做一次 TLB 刷新。*/
uint64_t rv_gstage_map_range(gstage_ctx_t *g, uint64_t gpa, uint64_t size,
                             int zero);

/* 查映射：命中返回 1 并填 *pa_out，未映射返回 0。*/
int rv_gstage_lookup(const gstage_ctx_t *g, uint64_t gpa, uint64_t *pa_out);

/* ── TLB 维护 ─────────────────────────────────────────────── */

/*
 * rv_gstage_tlb_flush — 刷新 G-stage 地址翻译缓存
 *
 * ⚠️ 这里是**整机全刷**（hfence.gvma 两个操作数都是 x0），不是按 VMID 或
 * GPA 定向刷。定向形式存在（rs1=GPA、rs2=VMID），但 VMID 操作数在寄存器里
 * 的编码位置容易记错，而记错的表现是"映射改了但硬件还用旧的" —— 与 aarch64
 * 那条「改了映射但不生效，偶尔又生效」同属最难查的一类。全刷在 QEMU/TCG 下
 * 只是一次 softmmu TLB 失效，代价可忽略，先用它。
 *
 * 注意：hfence.gvma **不依赖**当前 hart 的 hgatp（要刷哪个 VM 由操作数给出，
 * 给 x0 则是全部）—— 这一点与 aarch64 的 TLBI 不同，那边必须先重写
 * VTTBR_EL2，因为 TLBI 是按"当前 VTTBR 指定的 VM"生效的
 * （见 kernel/mm/aarch64/stage2.c 的 stage2_tlb_flush_vm）。
 */
void rv_gstage_tlb_flush(const gstage_ctx_t *g);

#endif /* RISCV64_GSTAGE_H */
