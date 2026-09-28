/*
 * include/x86_64/ept.h — x86_64 EPT (Extended Page Table) 二级地址翻译
 *
 * 移植自 x-kernel: virt/kvmm/src/mm/ept.rs
 *
 * EPT 提供 GPA → HPA 的第二级翻译，是 x86_64 上对应
 * AArch64 Stage-2 / RISC-V G-stage 的机制。
 *
 * 页表格式（与常规 x86_64 分页不同）：
 *   位 0   Read
 *   位 1   Write
 *   位 2   Execute
 *   位 3-5 内存类型（0=UC, 6=WB）
 *   位 6   Ignore PAT
 *   位 7   Large page（PD 级 = 2 MiB）
 *   位 12-51 下一级/页物理地址
 *
 * ── 从「全局单份 + identity map」改成「每 VM 一份 + 按需分页」 ──────
 *
 * 老版本是一组全局页表（g_ept_pml4/g_ept_pdpt/g_ept_pd）+ 全 4 GiB 预建
 * 映射，加上一句 x86_ept_enable_mmio_trap() 事后把非 RAM 项清成无效。
 * 那套只能有一个 VM：第二个 VM 的 init 会把第一份页表整个覆盖；而且 guest
 * RAM 必须先从 PMM 里预留一整段连续物理内存并整片清零
 * （guest_boot.c 的 pmm_mark_allocated + memset）。
 *
 * 现在与 aarch64/riscv 两侧同构：
 *   - 每个 VM 一个 ept_ctx_t，静态表按 slot 索引；
 *   - **init 之后表是空的**（"空表即全 trap"，不再需要 enable_mmio_trap）；
 *   - guest RAM 不再预映射，**首次访问才分配物理页并装映射**（见 vmx.c 的
 *     EPT violation 分支）；
 *   - 宿主自己要写的那几块（bzImage/initrd/boot_params/初始页表）在加载期
 *     显式 x86_ept_map_range()。
 *
 * ⚠️ 与 aarch64 的 stage2.h / riscv64 的 gstage.h 是同一个模板的三个实例。
 * 改一边时请对照另外两边，手势应当一致。
 */
#ifndef X86_64_EPT_H
#define X86_64_EPT_H

#include "types.h"

/* ── 页表层级尺寸（4 级：PML4 → PDPT → PD → PT）────────────── */
#define EPT_PDPT_ENTRIES 512
#define EPT_PD_ENTRIES   512
#define EPT_PT_ENTRIES   512

/* 覆盖 [0, 4 GiB) 需要的 PDPT/PD 项数（每项 1 GiB）*/
#define EPT_L1_ENTRIES 4

#define EPT_PAGE_SIZE  4096ULL
#define EPT_BLOCK_SIZE (2ULL * 1024 * 1024) /* 一张 PT 覆盖的粒度 */

/* ── 多 VM 上限 ───────────────────────────────────────────── */
/* 静态表按 slot 索引；vmm.h 里有 _Static_assert(MAX_VMS <= EPT_MAX_VMS) */
#define EPT_MAX_VMS 4

/*
 * 每 VM 最多几张 PT = guest RAM 窗口 / 2 MiB。
 * 192 MiB / 2 MiB = 96 —— 与 GUEST_LINUX_MEM_SIZE 对应，
 * guest_loader 那边有 _Static_assert 钉住这个关系（与 aarch64/riscv 同款）。
 */
#define EPT_MAX_PT_TABLES 96

/* ── 每 VM 的 EPT 上下文 ──────────────────────────────────── */
typedef struct ept_ctx {
    uint32_t slot;     /* 0..EPT_MAX_VMS-1（索引静态表）*/
    uint64_t ram_base; /* guest RAM 窗口（GPA）          */
    uint64_t ram_size;
    uint64_t ram_end;                    /* = ram_base + ram_size          */
    uint64_t eptp;                       /* 写进 VMCS 的 EPT_POINTER 值    */
    uint64_t *pml4;                      /* → 静态 g_ept_pml4[slot]        */
    uint64_t *pdpt;                      /* → 静态 g_ept_pdpt[slot]        */
    uint64_t *pd[EPT_L1_ENTRIES];        /* → 静态 g_ept_pd[slot][i]       */
    uint64_t *pt_tbl[EPT_MAX_PT_TABLES]; /* +1 = 该 2MiB 块已有表；动态分配 */
    uint64_t nr_premap;                  /* 统计：加载期预映射页数          */
    uint64_t nr_fault;                   /* 统计：按需缺页分配页数          */
} ept_ctx_t;

/* ── 生命周期 ─────────────────────────────────────────────── */

/*
 * x86_ept_vm_init — 为一个 VM 建立**空**的 EPT
 *
 * init 之后表里没有任何有效映射（"空表即全 trap"）：guest 访问任何 GPA 都会
 * 触发 EPT violation（VM-exit 48），由 VMM 判断是模拟 MMIO 还是分配一页 RAM。
 * 调用方接着用 x86_ept_map_range() 把必须要有的那几块显式映射进去。
 *
 * 注：**没有 hpa_base 参数**。按需分页下每页都是各自从 PMM 分配的，GPA 与
 * HPA 之间不再有固定偏移（老版本那句 `hpa_base + (gpa - mem_base)` 就是这么
 * 消失的）。x86 尤其要注意：guest 的物理地址必须从 0 开始（Linux 假定有常规
 * 内存、内核在 1 MiB），而宿主的物理 0 不能给它 —— 以前靠 EPT 的线性偏移，
 * 现在靠"每个 GPA 各自映射到一个现分配的宿主页"。
 */
void x86_ept_vm_init(ept_ctx_t *e, uint32_t slot, uint64_t ram_base,
                     uint64_t ram_size);

/*
 * x86_ept_vm_destroy — 释放该 VM 的所有按需页与 PT 表
 *
 * 遍历 pt_tbl[]：每个有效表项对应一个按需分配的 4 KiB 页，连同表页本身一起
 * 还回 PMM。**调用前必须确保该 VM 的 vCPU 任务已经退出**。
 */
void x86_ept_vm_destroy(ept_ctx_t *e);

/* 返回 EPTP 值（写进 VMCS 的 EPT_POINTER 字段）：
 * root_pa | (page_walk_length-1)<<3 | memory_type（4 级 → 3；WB → 6）。*/
uint64_t x86_ept_eptp(const ept_ctx_t *e);

/* ── 映射 ─────────────────────────────────────────────────── */

/* GPA 是否落在本 VM 的 RAM 窗口内（缺页处理用它区分 RAM 与 MMIO）。*/
int x86_ept_ipa_is_ram(const ept_ctx_t *e, uint64_t gpa);

/*
 * x86_ept_map_page — 为 gpa 分配一个宿主物理页并装上 4 KiB 映射
 *
 * @zero: 非 0 时把新页清零（防跨 VM 数据泄漏）。
 * 返回映射到的 HPA；返回 0 表示失败（PMM 没页了）—— **调用方必须处理失败**。
 */
uint64_t x86_ept_map_page(ept_ctx_t *e, uint64_t gpa, int zero);

/*
 * x86_ept_map_block — 一次装好 gpa 所在的整个 2 MiB 块（512 页）
 *
 * 缺页处理用它替代单页映射：把 VM-exit 的往返次数从"每页一次"降到"每块一次"
 * （aarch64/riscv 实测把上千次缺页压到个位数）。返回本次装上的页数。
 */
uint64_t x86_ept_map_block(ept_ctx_t *e, uint64_t gpa, int zero);

/* 批量版：从 gpa 起 size 字节逐页映射，末尾只做一次 INVEPT。*/
uint64_t x86_ept_map_range(ept_ctx_t *e, uint64_t gpa, uint64_t size, int zero);

/* 查映射（软件侧 GPA → HPA，MMIO 取指/解码用）：
 * 命中返回 1 并填 *hpa_out，未映射返回 0。*/
int x86_ept_lookup(const ept_ctx_t *e, uint64_t gpa, uint64_t *hpa_out);

/* ── TLB 维护 ─────────────────────────────────────────────── */

/*
 * x86_ept_invept_all — 刷新本 VM 的 EPT 派生 TLB（INVEPT single-context）
 *
 * ⚠️ INVEPT 只作用于**当前逻辑处理器**。调用点必须落在真正跑这个 vCPU 的
 * 核上 —— 缺页路径天然满足（VM-exit 就在那颗核上），加载期的批量映射则发生在
 * vCPU 任务创建之前、这个 EPTP 还没进过任何 TLB，所以也不需要跨核 shootdown。
 */
void x86_ept_invept_all(const ept_ctx_t *e);

#endif /* X86_64_EPT_H */
