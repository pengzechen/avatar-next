/*
   include/vmm/vmm_vnpu.h — RK3588 NPU 寄存器透传给 guest

   guest 访问 0xFDAB0000..0xFDAE0000 时, VMM 把这些访问原样转发到真实的
   NPU 寄存器上 —— 不模拟, 不拦截, 不解释。guest 里的 rknpu 驱动看到的就是
   真硬件。

   为什么是"转发"而不是"映射进 stage-2 直通"：
     两条路都能让 guest 摸到寄存器(见 stage2_map_device_region), 但走 MMIO
     总线有个直通没有的好处 —— 可观测。NPU 出问题时能在 VMM 侧看到
     guest 到底读写过哪些寄存器；直通的话 host 侧一个观测点都没有。
     代价是每次访问一次 EL2 往返, 而 rknpu 跑一个作业只写十来个寄存器
     (regcmd 是 DMA 走内存的, 不经过 MMIO), 这点开销可以忽略。

   注意： 设备窗口在 stage-2 里必须保持无映射, 否则访问不会陷入 EL2,
      也就走不到这里。见 include/vmm/vmm_mmio.h 顶部的说明。
*/
#ifndef VMM_VNPU_H
#define VMM_VNPU_H

#include "vmm/vmm.h"
#include "vmm/vmm_mmio.h"

/*
   三个 NPU 核的窗口恰好连续：0xFDAB0000 / 0xFDAC0000 / 0xFDAD0000,
   每个 64 KiB, 所以一个设备就能覆盖(offset >> 16 = 核号)。
   与 platforms/rk3588-aarch64/platform.conf 的 npu.base0/1/2 一致。
*/
#define VNPU_BASE      0xFDAB0000ULL
#define VNPU_SIZE      0x00030000ULL
#define VNPU_CORE_SIZE 0x00010000ULL
#define VNPU_CORES     3

/*
   vnpu_init — 注册 NPU 透传设备到 MMIO 总线。

   返回 0 成功；-1 失败(调用方应当让建 VM 失败, 而不是警告后继续 ——
   静默死设备的症状是 guest 里驱动 probe 到一半就读到全 0, 极难查)。

   host_base 是宿主侧 NPU 寄存器基址(platform_get_mmio 的返回值, 已含
   KERNEL_VMA)；为 0 时说明这个平台没有 NPU, 本函数直接返回 0 且不注册设备。
*/
int vnpu_init(vm_t *vm, mmio_bus_t *bus, uintptr_t host_base);

/*
   vnpu_destroy — 与 vnpu_init 逆序：把 NPU 还给宿主驱动。

   只有当前持有者调用才有效(别的 VM 调是 no-op)。VM 销毁时调用,
   这样停掉 VM 之后还能再起一个, 宿主自己也能接着跑 NPU 作业。
*/
void vnpu_destroy(vm_t *vm);

/*
   vnpu_irq_asserted — 某个核的中断线此刻是否有效(电平语义)。

   判据是 `(INT_STATUS & INT_MASK) != 0`(不是 `& ~INT_MASK` —— 那个恒为 0,
   见 vnpu.c 实现处那段：真机上就是这么错的, NPU 一直正常完成, 中断一次没投)。
   NPU 的中断是电平
   触发, 状态位一直有效到 guest 写 INT_CLEAR —— 所以调用方必须在每次
   进入 guest 前重拉, 不能只在某一次事件上置一次 pending(那是边沿的做法)。
   与 vmm_console.h 的 vpl011_rx_irq_asserted() 完全同构, 理由也一样。

   注意： 只读, 不 ack。这里绝不能顺手清 INT_STATUS：清掉的是 guest 的中断,
      它醒来会发现状态没了, 任务也找不到, 症状是奇怪的超时。

   未注册(平台无 NPU)或核号越界时返回 false。
*/
bool vnpu_irq_asserted(vm_t *vm, uint32_t core);

#endif /* VMM_VNPU_H */
