#ifndef RISCV64_RVPLATFORM_H
#define RISCV64_RVPLATFORM_H

#include "types.h"

/*
 * include/riscv64/rvplatform.h — RISC-V **平台上实际可用的 cache 管理指令（CMO）**
 *
 * 为什么只有 RISC-V 需要这一层：
 *   aarch64 的 `dc`/`ic` 与 x86_64 的 `clflush`/`wbinvd` 都是**架构标准指令**，
 *   缓存行大小也都能从 `CTR_EL0` / `CPUID` 读出来，所以那两套的 cache_impl.h
 *   自己就够了。RISC-V 不一样：基座 ISA 里**没有** CMO；标准扩展 Zicbom 是可选的；
 *   而 Zicbom 之前的厂商核（T-Head C906/C910…）用的是 CUSTOM-0 **私有编码**。
 *   同一份 cache_impl.h 要面对"标准扩展 / 厂商私有 / 什么都没有"三种情况，
 *   于是把"这台机器到底有什么 CMO"收敛到本文件，cache_impl.h 只做
 *   通用动词 → 本文件原语 的映射。
 *
 * 新增平台时在这里加一个 RV_CMO_* 类别与对应分支，cache_impl.h 不用动。
 */

#define RV_CMO_NONE    0   /* 没有任何 CMO（平台由硬件保证一致，如 QEMU virt） */
#define RV_CMO_ZICBOM  1   /* 标准 Zicbom 扩展：cbo.clean / cbo.flush / cbo.inval */
#define RV_CMO_THEAD   2   /* T-Head C906/C910 私有 CMO（CUSTOM-0 编码） */

#if defined(PLATFORM_SG2002)
#  define RV_CMO_KIND RV_CMO_THEAD
#elif defined(__riscv_zicbom)
#  define RV_CMO_KIND RV_CMO_ZICBOM
#else
#  define RV_CMO_KIND RV_CMO_NONE
#endif

/* 一个 cache block 的字节数（CMO 的最小粒度）。
 * C906 与 Zicbom 的常见取值都是 64；Zicbom 规范上应从设备树
 * `riscv,cbom-block-size` 读，本内核暂无设备树解析，先固定。 */
#define RV_CMO_BLOCK_SIZE 64u

static inline size_t rv_cmo_block_size(void);
static inline void   rv_cmo_clean(void *addr);          /* 写回，保留在缓存里 */
static inline void   rv_cmo_invalidate(void *addr);     /* 失效，不写回（脏数据会丢） */
static inline void   rv_cmo_clean_invalidate(void *addr);/* 写回并失效 */
static inline void   rv_cmo_invalidate_all(void);       /* 整 cache 失效（仅 T-Head 提供） */
static inline void   rv_cmo_sync(void);                 /* CMO 之后的完成屏障 */
static inline void   rv_cmo_icache_sync(void);          /* 让刚写入的指令可见 */

/* ══════════════════════════════════════════════════════════════════════
 * T-Head C906 / C910（CUSTOM-0 私有编码）
 *
 * ⚠️ 编码来源与**待确认项**：仓库没有附厂商手册，下面的编码取自公开的
 *    T-Head CMO 表。其中"按 VA"的三条与仓库原有代码、以及 I 侧在 .S 里
 *    早就用着的 0x0100000b/0x01a0000b 一致，可信度较高；但"整 cache"那三条
 *    与原代码的命名冲突，见 rv_cmo_invalidate_all() 的注释。
 *
 *      dcache.cva   rs1 = 0x025   按 VA 清理（写回）
 *      dcache.iva   rs1 = 0x026   按 VA 失效（不写回）
 *      dcache.civa  rs1 = 0x027   按 VA 清理+失效
 *      dcache.call     = 0x0010000b   整 cache 清理（写回）
 *      dcache.ciall    = 0x0020000b   整 cache 清理+失效
 *      dcache.iall     = 0x0030000b   整 cache 失效（**不写回**）
 *      icache.iall     = 0x0100000b   I-cache 全失效
 *      sync.i          = 0x01a0000b
 *
 *    CMO 之后必须跟一条 fence（sync.i / fence rw,rw），由 rv_cmo_sync() 负责。
 * ══════════════════════════════════════════════════════════════════════ */
#if RV_CMO_KIND == RV_CMO_THEAD

static inline size_t
rv_cmo_block_size(void)
{
    return RV_CMO_BLOCK_SIZE;
}

static inline void
rv_cmo_clean(void *addr)
{
    __asm__ volatile(".insn i 0x0b, 0, x0, %0, 0x025" : : "r"(addr) : "memory");
}

static inline void
rv_cmo_invalidate(void *addr)
{
    __asm__ volatile(".insn i 0x0b, 0, x0, %0, 0x026" : : "r"(addr) : "memory");
}

static inline void
rv_cmo_clean_invalidate(void *addr)
{
    /* civa：一条指令完成"写回 + 失效"。原来的代码发 cva 再发 iva，
     * 两次访问之间别的 agent 可能把行弄脏，语义上不等价。 */
    __asm__ volatile(".insn i 0x0b, 0, x0, %0, 0x027" : : "r"(addr) : "memory");
}

/*
 * ⚠️ 整 cache 失效：编码 0x0030000b。
 *
 * 原代码把这个编码命名为 `__c906_dcache_ciall`（清理+失效）并当作"清理"用在
 * clean_dcache_range() 上。但按上表的 0x0030000b 是 **iall（失效、不写回）** ——
 * 若表正确，那条路径会**丢弃脏数据**。
 *
 * 目前**没有任何调用者**：SG2002 的 range 操作已改为按 VA 的 cva/iva/civa。
 * 在查手册或上板确认之前：函数名按表（invalidate），不要把它接到 clean 语义上；
 * 若确认表有误，改这里的编码/命名，并考虑是否需要 *_all 形式的清理。
 */
static inline void
rv_cmo_invalidate_all(void)
{
    __asm__ volatile(".long 0x0030000b" ::: "memory");
}

static inline void
rv_cmo_sync(void)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
}

static inline void
rv_cmo_icache_sync(void)
{
    /* fence.i 在 C906 上**不足以**让新写入的指令可见（这正是下面两条厂商指令
     * 存在的原因，.S 的 exec/fork 返回路径里也是这么写的）。 */
    __asm__ volatile(
        "fence.i            \n"
        ".long 0x0100000b   \n"   /* icache.iall */
        ".long 0x01a0000b   \n"   /* sync.i      */
        ::: "memory");
}

/* ═══════════════════════════════════════════ Zicbom（标准扩展）══ */
#elif RV_CMO_KIND == RV_CMO_ZICBOM

static inline size_t
rv_cmo_block_size(void)
{
    return RV_CMO_BLOCK_SIZE;
}

static inline void
rv_cmo_clean(void *addr)
{
    __asm__ volatile("cbo.clean %0" : : "r"(addr) : "memory");
}

static inline void
rv_cmo_invalidate(void *addr)
{
    __asm__ volatile("cbo.inval %0" : : "r"(addr) : "memory");
}

static inline void
rv_cmo_clean_invalidate(void *addr)
{
    __asm__ volatile("cbo.flush %0" : : "r"(addr) : "memory");
}

static inline void
rv_cmo_invalidate_all(void)
{
    /* Zicbom 只有按 block 的形式，没有"整 cache"指令；真需要时按内存区间循环。
     * 这里显式不做（平台层另有 *_all 需求时再实现）。 */
}

static inline void
rv_cmo_sync(void)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
}

static inline void
rv_cmo_icache_sync(void)
{
    __asm__ volatile("fence.i" ::: "memory");
}

/* ═══════════════════════════════ 没有 CMO（平台保证一致）══════ */
#else

static inline size_t
rv_cmo_block_size(void)
{
    return RV_CMO_BLOCK_SIZE;
}

/* 没有 CMO 的平台（QEMU virt）：DMA 由模拟器/硬件保证一致，
 * 这里全部是空操作。以前 clean 分支会发一条 `fence ow, ow` —— 那是访存顺序
 * 屏障，不是 cache 操作，对"清理"没有任何作用，已去掉。 */
static inline void rv_cmo_clean(void *addr)            { (void)addr; }
static inline void rv_cmo_invalidate(void *addr)       { (void)addr; }
static inline void rv_cmo_clean_invalidate(void *addr) { (void)addr; }
static inline void rv_cmo_invalidate_all(void)         { }

static inline void
rv_cmo_sync(void)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
}

static inline void
rv_cmo_icache_sync(void)
{
    __asm__ volatile("fence.i" ::: "memory");
}

#endif /* RV_CMO_KIND */

#endif /* RISCV64_RVPLATFORM_H */
