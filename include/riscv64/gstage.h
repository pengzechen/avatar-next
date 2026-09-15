/*
 * include/riscv64/gstage.h — RISC-V H-extension G-stage (Stage-2) MMU
 *
 * 移植自 x-kernel: virt/kvmm/src/mm/gstage.rs
 *
 * G-stage 提供第二级地址翻译 GPA → HPA，通过 hgatp CSR 生效。
 * PTE 格式与 Sv39 完全相同（V/R/W/X/U/G/A/D），只是地址含义从
 * VA→PA 变成 GPA→HPA。
 *
 * 采用 Sv39x4 模式：根页表 16 KiB（4 个连续 4KiB 页 = 2048 项），
 * 每项覆盖 1 GiB，共 2 TiB guest 物理地址空间。对 [0,4GiB) 的
 * identity map 只需前 4 项，每项指向一个用 2 MiB 大页填充的 L1 表。
 *
 * 与 AArch64 的 stage2.h 对称：aarch64 用 VTTBR_EL2，riscv 用 hgatp。
 */
#ifndef RISCV64_GSTAGE_H
#define RISCV64_GSTAGE_H

#include "types.h"

/*
 * rv_gstage_init — 建立 Sv39x4 identity map 并准备 hgatp 值
 *
 * @mem_base: guest 物理内存（GPA）基址
 * @mem_size: guest 物理内存大小
 * @hpa_base: RAM 区对应的宿主物理地址基址（GPA=mem_base 映射到 HPA=hpa_base）
 * @vmid:     虚拟机 ID（写入 hgatp.VMID）
 *
 * RAM 区 [mem_base, mem_base+mem_size) 映射到 [hpa_base, ...)，
 * 其余按 device 属性 identity 映射（GPA==HPA）。
 */
void rv_gstage_init(uint64_t mem_base, uint64_t mem_size,
                    uint64_t hpa_base, uint32_t vmid);

/*
 * rv_gstage_activate — 将根页表写入 hgatp 并刷新 G-stage TLB
 */
void rv_gstage_activate(void);

/*
 * rv_gstage_gpa_to_hpa — GPA → HPA 翻译（软件侧，用于 MMIO 取指等）
 * 返回 1 并写 *hpa_out；GPA 超出 RAM 范围时返回 0。
 */
int rv_gstage_gpa_to_hpa(uint64_t gpa, uint64_t *hpa_out);

#endif /* RISCV64_GSTAGE_H */
