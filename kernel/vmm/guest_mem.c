/*
 * kernel/vmm/guest_mem.c — 宿主访问 guest 内存的唯一入口
 *
 * 从 guest_loader.c 拆出来的。按需分页之后，guest RAM 在宿主侧不再是
 * identity 映射的一段连续内存，所以**任何**按 GPA 的读写都必须经这里现算
 * 宿主地址（必要时现分配 + 建 stage-2 映射）。
 *
 * 拆出来还顺带解开一个跨 TU 循环依赖：guest_loader.c 就地 extern 并调用
 * x86_guest_boot()，而 guest_boot.c 反过来依赖本文件的 guest_loader_write_guest()。
 *
 * 原型都在 include/guest_loader.h（公共头）。
 */

#include "guest_loader.h"
#include "klog.h"
#include "mm_vm.h" /* phys_to_virt */
#include "string.h"
#include "vmm/vmm.h"

#if ARCH_AARCH64
#include "aarch64/stage2.h"
#elif ARCH_RISCV64
#include "riscv64/gstage.h"
#elif ARCH_X86_64
#include "x86_64/ept.h"
#endif

/* ── GPA → 宿主可写地址（按需分页下**唯一**的入口）────────────────
 *
 * ⚠️ 从前这里到处是 `phys_to_virt(gpa)` —— 因为 guest RAM 是 identity 映射
 * （GPA == PA），随手一算就能写。按需分页之后这两条前提都没了：
 *   - 同一个 GPA 在不同 VM 里指向**不同的**物理页（隔离就靠这个）；
 *   - 页面可能**还没分配**（首次访问才由缺页处理补上）。
 * 所以宿主代码要碰 guest 内存，一律走这里。
 *
 * @alloc: 允许在未映射时现分配一页（加载映像时为 1；只想看看时为 0）。
 * 返回 NULL 表示"没映射且不允许分配"或"PMN 没页了"。
 */
void *guest_loader_gpa_ptr(vm_t *vm, uint64_t gpa, int alloc)
{
    /*
   * ⚠️ 两条路径都必须把**页内偏移**加回去。
   *
   * map_* 系列返回的是**页基址**（它们只负责把 gpa 所在的页映射好），而
   * lookup 系列返回的地址里已经带了偏移（`(entry & ~0xFFF) | (gpa & 0xFFF)`）。
   * 早先 map 那条路忘了加，症状极具误导性：
   *   - **顺序装载、且起点页对齐**时完全正常 —— 一页里第一次写 off==0，
   *     之后同一页的写都走 lookup 分支（已经映射了），偏移是对的；
   *   - 而**起点不在页边界**的一次性写入（比如 x86 的 MP 表，GPA 0x9F800）
   *     会把整段数据写到**页首**去，目标位置留下一片零，且没有任何报错。
   * aarch64/riscv 的 guest 镜像恰好都是页对齐顺序装载，所以一直没暴露。
   */
#if ARCH_AARCH64
    uint64_t pa = 0;

    if (stage2_lookup(&vm->s2, gpa, &pa))
        return phys_to_virt(pa);
    if (!alloc)
        return NULL;

    pa = stage2_map_page(&vm->s2, gpa, 1 /*zero*/);
    if (!pa)
        return NULL;
    vm->s2.nr_premap++; /* 加载期分配的页（与缺页驱动的 nr_fault 区分统计）*/
    return phys_to_virt(pa | (gpa & 0xFFF));
#elif ARCH_RISCV64
    uint64_t pa = 0;

    if (rv_gstage_lookup(&vm->gstage, gpa, &pa))
        return phys_to_virt(pa);
    if (!alloc)
        return NULL;

    pa = rv_gstage_map_page(&vm->gstage, gpa, 1 /*zero*/);
    if (!pa)
        return NULL;
    vm->gstage
        .nr_premap++; /* 加载期分配的页（与缺页驱动的 nr_fault 区分统计）*/
    return phys_to_virt(pa | (gpa & 0xFFF));
#elif ARCH_X86_64
    uint64_t hpa = 0;

    if (x86_ept_lookup(&vm->ept, gpa, &hpa))
        return phys_to_virt(hpa);
    if (!alloc)
        return NULL;

    hpa = x86_ept_map_page(&vm->ept, gpa, 1 /*zero*/);
    if (!hpa)
        return NULL;
    vm->ept.nr_premap++; /* 加载期分配的页（与缺页驱动的 nr_fault 区分统计）*/
    return phys_to_virt(hpa | (gpa & 0xFFF));
#else
    /* 还没有 stage-2 的架构（vmm_test 的玩具 guest）：identity 映射 */
    (void)vm;
    (void)alloc;
    return phys_to_virt(gpa);
#endif
}

/*
 * ── 页感知的 guest 内存访问族 ───────────────────────────────
 *
 * ⚠️ **不要对 guest_loader_gpa_ptr() 的返回值做跨页的指针算术。**
 *
 * identity 映射时代 `phys_to_virt(gpa)` 是线性的，于是 `p + off`（off 超过
 * 一页）恰好就是"gpa + off 那一页"—— 很多代码靠这个跨页 memcpy/memmove。
 * 改成按需分页之后那条不变量没了：每个 guest 页都是各自从 PMM 分配的、
 * **物理上不连续**，`p + off` 走到的是宿主物理内存里的下一页，而不是 guest
 * 的下一页。表现是"数据搬过去了一部分，另一部分是宿主的随机内容"，且没有
 * 任何报错。
 *
 * x86 的 bzImage 装载就栽在这里：11 MB 的保护模式内核用一句 memmove 搬运，
 * 搬完 guest 的 GDT（在 payload 末尾 0xAA7B40）是垃圾，`lgdt` 之后
 * `mov %ax,%ds` 直接 #GP → IDT 还没建 → **triple fault**。
 *
 * 所以跨页的一律走下面这几个：它们逐页翻译、逐页搬。
 */
int guest_loader_write_guest(vm_t *vm, uint64_t gpa, const void *src, size_t n)
{
    const uint8_t *s = (const uint8_t *)src;

    while (n > 0) {
        uint64_t off_in_page = gpa & 0xFFFULL;
        size_t chunk = (size_t)(4096 - off_in_page);
        void *dst;

        if (chunk > n)
            chunk = n;

        dst = guest_loader_gpa_ptr(vm, gpa, 1);
        if (!dst) {
            KLOG_ERROR("[guest] write_guest: cannot map gpa=0x%llx\n",
                       (unsigned long long)gpa);
            return -1;
        }
        memcpy(dst, s, chunk);

        gpa += chunk;
        s += chunk;
        n -= chunk;
    }
    return 0;
}

/* 从 guest 内存读一段到宿主缓冲（逐页）。*/
int guest_loader_read_guest(vm_t *vm, uint64_t gpa, void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;

    while (n > 0) {
        uint64_t off_in_page = gpa & 0xFFFULL;
        size_t chunk = (size_t)(4096 - off_in_page);
        const void *src;

        if (chunk > n)
            chunk = n;

        src = guest_loader_gpa_ptr(vm, gpa, 0 /*只读，不分配*/);
        if (!src) {
            KLOG_ERROR("[guest] read_guest: gpa=0x%llx unmapped\n",
                       (unsigned long long)gpa);
            return -1;
        }
        memcpy(d, src, chunk);

        gpa += chunk;
        d += chunk;
        n -= chunk;
    }
    return 0;
}

/* 往 guest 内存填一段字节（逐页）。*/
int guest_loader_fill_guest(vm_t *vm, uint64_t gpa, int byte, size_t n)
{
    while (n > 0) {
        uint64_t off_in_page = gpa & 0xFFFULL;
        size_t chunk = (size_t)(4096 - off_in_page);
        void *dst;

        if (chunk > n)
            chunk = n;

        dst = guest_loader_gpa_ptr(vm, gpa, 1);
        if (!dst) {
            KLOG_ERROR("[guest] fill_guest: cannot map gpa=0x%llx\n",
                       (unsigned long long)gpa);
            return -1;
        }
        memset(dst, byte, chunk);

        gpa += chunk;
        n -= chunk;
    }
    return 0;
}
