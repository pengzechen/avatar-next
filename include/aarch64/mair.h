#ifndef  __MAIR_H__
#define __MAIR_H__

//  ===================== MAIR 寄存器 ===========================
// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
/* Memory Attributes 控制这个页表项对应的内存区域的内存类型,缓存策略 */

// 设备内存 禁止聚集(non Gathering) 禁止重排(non re-order) 禁止提前的写入ACK(Early Write Acknowledgement)
#define MA_DEVICE_nGnRnE_Flags 0x00
// 普通内存 使用写回写分配 读分配 (最快的形式,各种缓存buff拉满)
#define MA_MEMORY_Flags 0xFF
// 普通内存 禁止所有缓存策略
#define MA_MEMORY_NoCache_Flags 0x44

// MAIR_ELx 可以放置8种Memory Attributes,但是我们只需要这三种就够了
#define MA_DEVICE_nGnRnE  0
#define MA_MEMORY         1
#define MA_MEMORY_NoCache 2

#define PTE_AIDX_DEVICE_nGnRn   (MA_DEVICE_nGnRnE << 2)
#define PTE_AIDX_MEMORY         (MA_MEMORY << 2)
#define PTE_AIDX_MEMORY_NOCACHE (MA_MEMORY_NoCache << 2)

// 这个值 我们一会放到MAIR_EL1 寄存器中
#define MAIR_VALUE                                                                                 \
    (MA_DEVICE_nGnRnE_Flags << (8 * MA_DEVICE_nGnRnE)) | (MA_MEMORY_Flags << (8 * MA_MEMORY)) |    \
        (MA_MEMORY_NoCache_Flags << (8 * MA_MEMORY_NoCache))


#endif /* __MAIR_H__ */