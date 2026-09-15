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
 * 用 PD 级 2 MiB 大页建立 [0, 4GiB) 的 identity map。
 */
#ifndef X86_64_EPT_H
#define X86_64_EPT_H

#include "types.h"

/*
 * x86_ept_init — 建立 EPT identity map
 *
 * @mem_base: guest 物理内存（GPA）基址
 * @mem_size: guest 物理内存大小
 * @hpa_base: RAM 区对应的宿主物理地址基址（GPA=mem_base → HPA=hpa_base）
 *
 * RAM 区映射为 WB（write-back）；其余区域为 UC（uncacheable，设备内存语义）。
 */
void x86_ept_init(uint64_t mem_base, uint64_t mem_size, uint64_t hpa_base);

/*
 * x86_ept_eptp — 返回 EPTP 值（写入 VMCS 的 EPT_POINTER 字段）
 *
 * 格式：root_pa | (page_walk_length-1)<<3 | memory_type
 *   4 级页表 → 字段值 3；内存类型 6 (WB)。
 */
uint64_t x86_ept_eptp(void);

/*
 * x86_ept_gpa_to_hpa — GPA → HPA 软件翻译（MMIO 取指/解码用）
 * 返回 1 并写 *hpa_out；GPA 超出 RAM 范围时返回 0。
 */
int x86_ept_gpa_to_hpa(uint64_t gpa, uint64_t *hpa_out);

/*
 * x86_ept_enable_mmio_trap — 把 guest RAM 之外的 GPA 全部置为「无效」
 *
 * 对标 x-kernel kvmm mm/ept.rs：只映射 guest RAM，其余（设备 MMIO、
 * 未支持 GPA）保持无效 → guest 访问触发 EPT violation（VM-exit 48）
 * → VMM 的 MMIO 总线分发到虚拟设备（见 vmm_mmio.h）。
 *
 * 必须在 x86_ept_init() 之后调用；未调用时保持原有全映射行为。
 */
void x86_ept_enable_mmio_trap(void);

/*
 * x86_ept_invept_all — 刷新 EPT TLB（INVEPT all-contextensions）
 */
void x86_ept_invept_all(void);

#endif /* X86_64_EPT_H */
