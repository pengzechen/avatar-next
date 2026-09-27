/*
 * include/vmm_vplic.h — 虚拟 RISC-V PLIC（中断控制器）
 *
 * 移植自 x-kernel: virt/kvmm/src/vdev/riscv64/irq.rs
 *
 * 遵循 PLIC 内存映射（priority / pending / enable / threshold / claim-complete），
 * 但仅支持前 64 个中断源，并把 PLIC context N 直接映射到 vCPU N。
 *
 * 寄存器布局（QEMU virt，基址 0x0c000000）：
 *   priority  : +0x000000，每源 4 字节
 *   pending   : +0x001000，每 32 源 1 字
 *   enable    : +0x002000，每 context 步长 0x80，每 32 源 1 字
 *   context N : +0x200000 + N*0x1000
 *       +0x00 threshold
 *       +0x04 claim/complete
 *
 * 投递路径：`vplic_next_deliverable()` 返回最高优先级可投递中断，
 * 由 VMM 在进入 guest 前经 `hvip.VSEIP` 注入（见 hext_run.c）。
 */
#ifndef VMM_VPLIC_H
#define VMM_VPLIC_H

#include "vmm_mmio.h"

/* QEMU virt PLIC 基址与大小 */
#define VPLIC_BASE   0x0c000000ULL
#define VPLIC_SIZE   0x400000ULL

#define VPLIC_MAX_IRQS    64
#define VPLIC_MAX_VCPUS   8

/* ── 设备私有状态 ─────────────────────────────────────────────
 *
 * ⚠️ 从前这是一个**文件级 static**（`static vplic_state_t g_vplic;`），
 * 于是整个内核只有一份 PLIC 状态 —— 第二个 VM 的 vplic_init() 里那句 memset
 * 会把第一个 VM 的 priority/enable/threshold 全清掉，两个 VM 从此共用同一个
 * 中断控制器（而且没有任何报错）。
 *
 * 并发：**从前它连锁都没有**，因为单 VM 时所有调用点都在同一个 vCPU 任务里
 * （入口注入 + MMIO 模拟）。多 VM 之后状态分开了，但要防的是"设备注入路径
 * 与 MMIO 路径分属不同上下文"这一类未来改动，所以手边就把它锁上 —— 代价
 * 是每个入口多一次无竞争的加解锁（见 vm_t 的 vplic_lock）。
 */
typedef struct vplic_state {
    struct vm *owner;

    uint32_t priority[VPLIC_MAX_IRQS];
    uint64_t pending[VPLIC_MAX_VCPUS];
    uint64_t active[VPLIC_MAX_VCPUS];
    uint64_t enable[VPLIC_MAX_VCPUS];
    uint32_t threshold[VPLIC_MAX_VCPUS];
    uint32_t nr_vcpus;
} vplic_state_t;

struct vm;
typedef struct vm vm_t;

/*
 * vplic_init — 初始化虚拟 PLIC 并注册到 MMIO 总线
 * 状态取自 vplic.c 的静态池（按 vm->slot 索引；dev->priv 会指过去）。返回 0 成功。
 */
int vplic_init(vm_t *vm, mmio_bus_t *bus, uint32_t nr_vcpus);

/* 归还本 VM 的槽位（由 vmm_arch_vm_destroy → vm_free 调用）。幂等。*/
void vplic_destroy(vm_t *vm);

/* 使某中断源挂起（设备注入入口）*/
void vplic_set_pending(vm_t *vm, uint32_t irq);

/*
 * vplic_next_deliverable — 返回 context(vcpu) 当前最高优先级的可投递中断
 * （已使能、未 active、优先级高于阈值）。无可投递时返回 0。
 */
uint32_t vplic_next_deliverable(vm_t *vm, uint32_t vcpu_id);

/* 认领（claim）一个中断：清挂起、置 active，返回中断号（0=无）*/
uint32_t vplic_claim(vm_t *vm, uint32_t vcpu_id);

/* 完成（complete）一个中断：清 active */
void vplic_complete(vm_t *vm, uint32_t vcpu_id, uint32_t irq);

#endif /* VMM_VPLIC_H */
