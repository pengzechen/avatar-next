/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/riscv64/gstage.h — RISC-V H-extension G-stage (Stage-2) MMU

   移植自 x-kernel: virt/kvmm/src/mm/gstage.rs

   G-stage 提供第二级地址翻译 GPA → HPA, 通过 hgatp CSR 生效。
   PTE 格式与 Sv39 完全相同(V/R/W/X/U/G/A/D), 只是地址含义从
   VA→PA 变成 GPA→HPA。

   采用 Sv39x4 模式：根页表 16 KiB(2048 项, 每项覆盖 1 GiB), 共 2 TiB
   guest 物理地址空间。QEMU virt 上 guest 只用到低 4 GiB, 所以 root[0..3]
   指向四张 L1 表, 每张覆盖 1 GiB。

   ── 从「全局单份 + identity map」改成「每 VM 一份 + 按需分页」 ──────

   老版本是一组全局页表(g_gstage_root/g_gstage_l1)+ identity map, 加上
   一句 rv_gstage_enable_mmio_trap() 把非 RAM 区间清成无效。那套只能有一个
   VM：第二份会覆盖第一份的映射, 而且 guest RAM 必须预先从 PMM 里预留一整段
   连续物理内存(platform.conf 的 guest_ram reserve)。

   现在：
     - 每个 VM 一个 gstage_ctx_t, 静态表按 slot 索引；
     - init 之后表是空的("空表就是全 trap", 不再需要 enable_mmio_trap)；
     - guest RAM 不再预映射, 首次访问才分配物理页并装映射(见
       hext_run.c 里 scause 20/21/23 的 RAM 缺页分支)。宿主侧要写的那几块
       (内核映像/DTB/initrd/初始栈)在加载期显式 rv_gstage_map_range()。

   GPA 空间在两个 VM 里是相同的(guest 的设备树写死了 memory@A0000000),
   但落到不同的物理页 —— 隔离由此而来。

   注意： 与 AArch64 的 stage2.h 是本文件的对偶(那边用 VTTBR_EL2, 这边用 hgatp)。
   改一边时请对照另一边, 两边的手势应当一致。
*/
#ifndef RISCV64_GSTAGE_H
#define RISCV64_GSTAGE_H

#include "guest_loader.h" /* GUEST_LINUX_MEM_BASE/SIZE */
#include "pte.h"
#include "spinlock.h"
#include "types.h"

/* ── Sv39x4 页表几何 ──────────────────────────────────────── */
/* hgatp 要求根表 16 KiB 对齐；PPN[1:0] 被硬件忽略, 表必须真是 16 KiB。 */
#define GSTAGE_ROOT_SIZE    (PAGE_SIZE * 4)
#define GSTAGE_ROOT_ENTRIES (GSTAGE_ROOT_SIZE / 8) /* 每项覆盖 1 GiB */
#define GSTAGE_L1_ENTRIES   RV_PT_ENTRIES          /* 每项覆盖 2 MiB */
#define GSTAGE_L0_ENTRIES   RV_PT_ENTRIES          /* 每项覆盖 4 KiB */

/* 低 4GiB 需要 4 张 L1 表；root[0..GSTAGE_L1_TABLES-1]。 */
#define GSTAGE_L1_TABLES 4

#define GSTAGE_PAGE_SIZE  PAGE_SIZE
#define GSTAGE_BLOCK_SIZE RV_LEVEL_SIZE(1) /* 一张 L0 表覆盖 2 MiB */

/* hgatp 字段(MODE=8 即 Sv39x4；CSR 访问必须用数值, 见 riscv64/csr.h) */
#define HGATP_MODE_SV39X4 RV_HGATP_MODE_SV39X4
#define HGATP_VMID_SHIFT  RV_HGATP_VMID_SHIFT
#define HGATP_PPN_MASK    RV_HGATP_PPN_MASK

/* ── 多 VM 上限 ───────────────────────────────────────────── */
/* 静态表按 slot 索引；include/vmm/vmm.h 里有 _Static_assert(MAX_VMS <= GSTAGE_MAX_VMS) */
#define GSTAGE_MAX_VMS 32

/*
   每 VM 最多几张 L0 表 = guest RAM 窗口 / 2 MiB。
   直接从 guest 布局算(192 MiB / 2 MiB = 96), guest 尺寸变了不用再手改。
   转 int 是为了让 gstage.c 里的 `for (int i = 0; i < ...; i++)` 不触发
   -Wsign-compare。
*/
#define GSTAGE_MAX_L0_TABLES ((int)(GUEST_LINUX_MEM_SIZE / GSTAGE_BLOCK_SIZE))

_Static_assert(GSTAGE_ROOT_SIZE % PAGE_SIZE == 0, "Sv39x4 根表必须按 4KiB 页对齐");
_Static_assert(GSTAGE_ROOT_ENTRIES == 2048, "Sv39x4 根表应为 16KiB / 8B = 2048 项");
_Static_assert(GSTAGE_MAX_L0_TABLES *GSTAGE_BLOCK_SIZE == GUEST_LINUX_MEM_SIZE,
               "G-stage L0 表数量必须正好覆盖 guest RAM 窗口");
_Static_assert(GUEST_LINUX_MEM_BASE % GSTAGE_BLOCK_SIZE == 0, "guest RAM 基址必须按 2MiB 对齐（ram_blk_index 的前提）");
_Static_assert(GUEST_LINUX_MEM_SIZE % GSTAGE_BLOCK_SIZE == 0, "guest RAM 大小必须是 2MiB 的整数倍");
_Static_assert(GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE <= (uint64_t)GSTAGE_L1_TABLES * RV_LEVEL_SIZE(2),
               "guest RAM 必须落在 G-stage 覆盖的低 4GiB 内");

/* ── 每 VM 的 G-stage 上下文 ───────────────────────────────── */
typedef struct gstage_ctx {
    uint32_t slot;     /* 0..GSTAGE_MAX_VMS-1(索引静态表) */
    uint32_t vmid;     /* 1..255, 写进 hgatp.VMID */
    uint64_t ram_base; /* guest RAM 窗口(GPA) */
    uint64_t ram_size;
    uint64_t ram_end;                       /* = ram_base + ram_size */
    uint64_t hgatp;                         /* hgatp 的值(MODE|VMID|root_ppn) */
    uint64_t *root;                         /* → 静态 g_gstage_root[slot] */
    uint64_t *l1[GSTAGE_L1_TABLES];         /* → 静态 g_gstage_l1[slot][i] */
    uint64_t *l0_tbl[GSTAGE_MAX_L0_TABLES]; /* +1 = 该 2MiB 块已有表；动态分配 */
    uint64_t nr_premap;                     /* 统计：加载期预映射页数 */
    uint64_t nr_fault;                      /* 统计：按需缺页分配页数 */

    /*
       ── 多 vCPU 的并发 ──────────────────────────────────────────

       同一 VM 的多个 vCPU 会在不同宿主 hart 上同时缺页, 装的是同一套
       G-stage 表, 所以表操作必须串行化 —— 否则两个 vCPU 可能同时给
       同一个 2 MiB 块建 L0 表(后写的覆盖先写的, 先写的那张表和其中
       已分配的 512 个页全部泄漏), 或同时给同一个 GPA 分配两个不同的
       HPA(两边都查表命中自己那页, guest 看到的内存取决于谁最后写)。

       远端 hart 的 G-stage TLB 不需要额外的「代际刷」机制：RISC-V 允许
       缓存「不可翻译」的结果, 那种缓存只会让访问再次 fault, 而缺页处理
       在本 hart 上装映射时会顺手 rv_gstage_tlb_flush()。映射一旦建立就
       不会改指向(销毁发生在本 VM 的所有 vCPU 退出之后), 因此不存在
       「远端缓存着一个仍然有效但已经过时的正翻译」这种需要主动失效的
       情况。曾经在每次入口按 map_gen 全刷, 4 个 vCPU 并行启动时等于
       每轮都刷掉所有翻译, 启动时间从秒级退化到几十秒。
    */
    spinlock_noirq_t map_lock;
} gstage_ctx_t;

/* ── 生命周期 ─────────────────────────────────────────────── */

/*
   rv_gstage_vm_init — 为一个 VM 建立空的 G-stage 表

   init 之后表里没有任何有效映射("空表即全 trap")：guest 访问任何 GPA 都会
   触发 guest-page fault(scause 20/21/23), 由 VMM 的缺页处理决定是模拟
   MMIO 还是分配一页 RAM。调用方接着用 rv_gstage_map_range() 把必须要有的
   那几块(内核映像/DTB/initrd/初始栈)显式映射进去。

   @slot: 0..GSTAGE_MAX_VMS-1, 决定用哪组静态 root/L1
   @vmid: 1..255, 同一个 VMID 同时只能有一个 VM 在用

   注：没有 hpa_base 参数。按需分页下每页都是各自从 PMM 分配的, GPA 与
   HPA 之间不再有固定偏移(老版本那句 identity 换算就是这么消失的)。
*/
void rv_gstage_vm_init(gstage_ctx_t *g, uint32_t slot, uint32_t vmid, uint64_t ram_base, uint64_t ram_size);

/*
   rv_gstage_vm_destroy — 释放该 VM 的所有按需页与 L0 表

   遍历 l0_tbl[]：每个有效表项对应一个按需分配的 4 KiB 页, 连同表页本身
   一起还回 PMM。调用前必须确保该 VM 的 vCPU 任务已经退出, 否则它可能
   正在用这些页。
*/
void rv_gstage_vm_destroy(gstage_ctx_t *g);

/* 把本 VM 的 hgatp 写进当前 hart 的寄存器(每轮进 guest 前都要)。 */
void rv_gstage_activate(const gstage_ctx_t *g);

/* ── 映射 ─────────────────────────────────────────────────── */

/* GPA 是否落在本 VM 的 RAM 窗口内(缺页处理用它区分 RAM 与 MMIO)。 */
int rv_gstage_ipa_is_ram(const gstage_ctx_t *g, uint64_t gpa);

/*
   rv_gstage_map_page — 为 gpa 分配一个宿主物理页并装上 4 KiB 映射

   @zero: 非 0 时把新页清零(防跨 VM 数据泄漏)。
   返回映射到的 PA；返回 0 表示失败(PMM 没页了)—— 调用方必须处理失败,
   不能当成功继续(那会映射到 PA 0)。
*/
uint64_t rv_gstage_map_page(gstage_ctx_t *g, uint64_t gpa, int zero);

/*
   rv_gstage_map_block — 一次装好 gpa 所在的整个 2 MiB 块(512 页)

   缺页处理用它替代单页映射：把陷入 HS-mode 的往返次数从"每页一次"降到
   "每块一次"(aarch64 那边实测这一步把 1447 次缺页压到 6 次)。
   返回本次装上的页数(PMM 不足时会少装)。
*/
uint64_t rv_gstage_map_block(gstage_ctx_t *g, uint64_t gpa, int zero);

/* 批量版：从 gpa 起 size 字节逐页映射, 末尾只做一次 TLB 刷新。 */
uint64_t rv_gstage_map_range(gstage_ctx_t *g, uint64_t gpa, uint64_t size, int zero);

/*
   查映射：命中返回 1 并填 *pa_out, 未映射返回 0。

   内部持 map_lock：多 vCPU 下查表可能与另一颗 hart 上的装表并发, 而
   L0 表的发布顺序在弱序架构上没有保证 —— 不能靠「指针非空就万事大吉」。
   @g 因此不再是 const。
*/
int rv_gstage_lookup(gstage_ctx_t *g, uint64_t gpa, uint64_t *pa_out);

/* ── TLB 维护 ─────────────────────────────────────────────── */

/*
   rv_gstage_tlb_flush — 刷新 G-stage 地址翻译缓存

   注意： 这里是整机全刷(hfence.gvma 两个操作数都是 x0), 不是按 VMID 或
   GPA 定向刷。定向形式存在(rs1=GPA, rs2=VMID), 但 VMID 操作数在寄存器里
   的编码位置容易记错, 而记错的表现是"映射改了但硬件还用旧的" —— 与 aarch64
   那条「改了映射但不生效, 偶尔又生效」同属最难查的一类。全刷在 QEMU/TCG 下
   只是一次 softmmu TLB 失效, 代价可忽略, 先用它。

   注意：hfence.gvma 不依赖当前 hart 的 hgatp(要刷哪个 VM 由操作数给出,
   给 x0 则是全部)—— 这一点与 aarch64 的 TLBI 不同, 那边必须先重写
   VTTBR_EL2, 因为 TLBI 是按"当前 VTTBR 指定的 VM"生效的
   (见 kernel/mm/aarch64/stage2.c 的 stage2_tlb_flush_vm)。
*/
void rv_gstage_tlb_flush(const gstage_ctx_t *g);

#endif /* RISCV64_GSTAGE_H */
