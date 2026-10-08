/* include/npu/npu_mmu.h
 *
 * RK3588 NPU 的 IOMMU（`rockchip,iommu-v2`）—— **宿主独占**的那一半。
 *
 * ── 为什么要有这个 ────────────────────────────────────────────────────
 *
 * NPU 是没有 IOMMU 的物理设备，只认物理地址。guest 的 RAM 是按需分页、
 * 物理不连续，所以透传给 guest 时只有恒等窗口里 IPA 才等于 PA —— 落在
 * 窗口外的缓冲，NPU 读到的就是别人的页（真机实测 regcmd[126] = 0x78015044）。
 *
 * RK3588 这块 MMU 是 Rockchip 自研的 **单级** IOMMU（不是 ARM SMMU）：
 * `rknpu_iommu.c` 全文没有 stage 概念 —— 一张表，谁写谁说了算。所以
 * **不能让 guest 碰它**（否则它能把任意物理地址映射进来，隔离为零），
 * 只能宿主写：
 *
 *     guest 驱动（原样，仍以为没有 IOMMU）：写"物理地址" = IPA
 *            │
 *       宿主把 IPA 填进这张表，指向真实的宿主页
 *            │
 *       NPU ──发出 IPA──▶ NPU MMU ──IPA→PA──▶ 宿主物理页（散着也行）
 *
 * 于是 guest 一个字不用改、恒等窗口不再必要、隔离也回来了。
 *
 * ── 表的真值来源 ──────────────────────────────────────────────────────
 *
 * **镜像 stage-2，不另建一套。** 宿主已经有一份权威的 IPA→PA，就是 stage-2；
 * 这张表跟着它走，就不会出现"CPU 看得到、NPU 看不到"的错位。
 * 同步时机放在**提交时**（guest 写 PC_OP_EN）而不是每次 stage-2 建映射时 ——
 * 后者要给每页发一次 TLB 失效，一次作业几千页，代价不可接受。
 *
 * ── 寄存器与页表格式 ──────────────────────────────────────────────────
 *
 * 全部抄自 mainline `drivers/iommu/rockchip-iommu.c`（它原生支持 v2）。
 * ⚠️ v2 的地址位域**不是连续的**，见下面 npu_mmu_mk_dte() 的说明。
 */
#ifndef NPU_MMU_H
#define NPU_MMU_H

#include "types.h"

#if ARCH_AARCH64

/* ── 寄存器（每个 MMU 块 0x100 字节）────────────────────────────────── */
#define NPU_MMU_DTE_ADDR       0x00 /* 目录表物理地址 */
#define NPU_MMU_STATUS         0x04
#define NPU_MMU_COMMAND        0x08 /* 写命令，不是写数据 */
#define NPU_MMU_PAGE_FAULT_ADDR 0x0C
#define NPU_MMU_ZAP_ONE_LINE   0x10
#define NPU_MMU_INT_RAWSTAT    0x14
#define NPU_MMU_INT_CLEAR      0x18
#define NPU_MMU_INT_MASK       0x1C
#define NPU_MMU_INT_STATUS     0x20
#define NPU_MMU_AUTO_GATING    0x24

/* STATUS */
#define NPU_MMU_STATUS_PAGING_ENABLED      (1U << 0)
#define NPU_MMU_STATUS_PAGE_FAULT_ACTIVE   (1U << 1)
#define NPU_MMU_STATUS_STALL_ACTIVE        (1U << 2)
#define NPU_MMU_STATUS_IDLE                (1U << 3)
#define NPU_MMU_STATUS_REPLAY_BUFFER_EMPTY (1U << 4)
#define NPU_MMU_STATUS_PAGE_FAULT_IS_WRITE (1U << 5)
#define NPU_MMU_STATUS_STALL_NOT_ACTIVE    (1U << 31)

/* COMMAND */
#define NPU_MMU_CMD_ENABLE_PAGING   0
#define NPU_MMU_CMD_DISABLE_PAGING  1
#define NPU_MMU_CMD_ENABLE_STALL    2
#define NPU_MMU_CMD_DISABLE_STALL   3
#define NPU_MMU_CMD_ZAP_CACHE       4 /* 整表 TLB 失效 */
#define NPU_MMU_CMD_PAGE_FAULT_DONE 5
#define NPU_MMU_CMD_FORCE_RESET     6

/* ── 页表 ─────────────────────────────────────────────────────────────
 *
 * 4 KiB 页。目录是一串 32 位 DTE，每项指向一张 PT；PT 是一串 32 位 PTE，
 * 每项覆盖 4 KiB。索引：DTE = ipa >> 22（每张 PT 管 4 MiB），
 * PTE = (ipa >> 12) & 0x3FF。
 */

/*
 * ⚠️ **v2 的地址位域不是连续的** —— 这是 v2 和 v1 唯一的、也是最容易写错的
 *    区别。物理地址的高 8 位被拆成两段，塞进 DTE 的中间：
 *
 *        31:12  PT 地址 bit 31:0
 *        11: 8  PT 地址 bit 35:32     ← 注意是从 bit 32 挪下来的
 *         7: 4  PT 地址 bit 39:36
 *         3: 1  保留
 *            0  有效位
 *
 *    写错的话，表会被放在错误的物理地址上 —— 症状是"翻译到随机的页"，
 *    和这个文件顶上描述的那个 bug 长得一模一样，极难分辨。
 */
#define NPU_MMU_DTE_VALID (1U << 0)

#define NPU_MMU_PTE_VALID    (1U << 0)
#define NPU_MMU_PTE_READABLE (1U << 1)
#define NPU_MMU_PTE_WRITABLE (1U << 2)

static inline uint32_t npu_mmu_mk_dte(uint64_t pa)
{
    return (uint32_t)((pa & 0xFFFFF000ULL) |          /* bit 31:12 ← pa[31:12] */
                      ((pa >> 24) & 0x00000F00ULL) |  /* bit 11:8  ← pa[35:32] */
                      ((pa >> 32) & 0x000000F0ULL) |  /* bit  7:4  ← pa[39:36] */
                      NPU_MMU_DTE_VALID);
}

static inline uint32_t npu_mmu_mk_pte(uint64_t pa, int writable)
{
    uint32_t pte = npu_mmu_mk_dte(pa) | NPU_MMU_PTE_READABLE;

    if (writable)
        pte |= NPU_MMU_PTE_WRITABLE;
    return pte;
}

/* ── 宿主侧 API ──────────────────────────────────────────────────────── */

/*
 * npu_mmu_init — 上电时调用一次：记下四个 MMU 寄存器块的位置。
 *
 * 四个块不是四个 MMU，而是**一个逻辑 MMU 的四组寄存器**（mainline 里同一个
 * DTE_ADDR 要写进每一组）。位置从 npu.base0/1/2 推出来，见 .c 里的说明。
 *
 * 返回 0 成功；平台没有 NPU 时返回 -1（调用方忽略即可）。
 */
int npu_mmu_init(void);

/*
 * npu_mmu_enable — 把 NPU 的地址翻译接管过来（guest 拿到 NPU 时调用）。
 *
 * 建一张覆盖 guest RAM 的表、写 DTE_ADDR、开 ENABLE_PAGING。
 * 表一开始是空的 —— 具体映射由 npu_mmu_sync() 在提交时填。
 */
int npu_mmu_enable(uint64_t ipa_base, uint64_t ipa_size);

/*
 * npu_mmu_disable — 关掉翻译（guest 交还 NPU 时调用）。
 *
 * ⚠️ 必须关：宿主自己驱动 NPU 时用的是**物理地址**，翻译开着会全部对不上。
 */
void npu_mmu_disable(void);

/*
 * npu_mmu_sync — 把 stage-2 的 IPA→PA 镜像进这张表（提交前调用）。
 *
 * 传 vm 指针（不透明，避免这个头依赖 vmm.h 的完整定义）。
 * 有变化才发 ZAP_CACHE。返回本次新填的页数。
 */
struct vm;
int npu_mmu_sync(struct vm *vm);

/*
 * npu_mmu_clear_fault — 清掉粘住的缺页。
 *
 * ⚠️ 缺页是**粘的**，而且清它必须两步、顺序不能反
 * PAGE_FAULT_DONE。我们不处理缺页中断，所以早期某次撞击会让它永远停在那儿 ——
 * 表填得再全也没用，因为翻译根本不发生了。每次提交前先清一遍。
 */
void npu_mmu_clear_fault(void);

/* 诊断：翻译开了没有 / 表里填了多少项 */
void npu_mmu_dump(void);

/* 诊断：拿一个具体 IPA 对账 —— 我的表 vs stage-2 各翻到哪个物理页 */
void npu_mmu_probe(struct vm *vm, uint64_t ipa);

#endif /* ARCH_AARCH64 */
#endif /* NPU_MMU_H */
