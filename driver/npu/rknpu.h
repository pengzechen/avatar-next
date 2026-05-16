/* driver/npu/rknpu.h
 *
 * Rockchip RK3588 NPU 驱动头文件
 *
 * 硬件信息（来自 RK3588 设备树）：
 *   npu@fdab0000 — 三核心 NPU，每核 64 KB MMIO 空间
 *   中断号: SPI 110/111/112 (GICv3 线号 142/143/144)
 *
 * 参考: testos-reflector/src/npu/
 */

#ifndef __RKNPU_H__
#define __RKNPU_H__

#include "types.h"

/* ── 硬件基地址（与 platform.lua 的 npu.base0/1/2 一致）─────────────────── */
#define RKNPU_NPU0_BASE  0xFDAB0000UL
#define RKNPU_NPU1_BASE  0xFDAC0000UL
#define RKNPU_NPU2_BASE  0xFDAD0000UL

/* GIC SPI 中断号 = DT interrupt number + 32 */
#define RKNPU_NPU0_IRQ   142   /* SPI 110 + 32 */
#define RKNPU_NPU1_IRQ   143
#define RKNPU_NPU2_IRQ   144

/* PMU1（电源域控制）基地址 */
#define RKNPU_PMU1_BASE  0xFD8D8000UL

/* ── 寄存器偏移量（每个核心内相对偏移）──────────────────────────────────── */
#define RKNPU_VERSION           0x0000  /* 版本魔数，应为 0x46495245 */
#define RKNPU_VERSION_NUM       0x0004  /* 子版本号 */
#define RKNPU_PC_OP_EN          0x0008  /* 写 1 提交，写 0 清除 */
#define RKNPU_PC_DATA_ADDR      0x0010  /* DMA 描述符地址 */
#define RKNPU_PC_DATA_AMOUNT    0x0014  /* 描述符数量 */
#define RKNPU_INT_MASK          0x0020  /* 中断掩码 */
#define RKNPU_INT_CLEAR         0x0024  /* 写 1 清除中断 */
#define RKNPU_INT_STATUS        0x0028  /* 中断状态（含任务计数） */
#define RKNPU_INT_RAW_STATUS    0x002C  /* 原始中断状态 */
#define RKNPU_PC_TASK_CONTROL   0x0030  /* 任务控制寄存器 */
#define RKNPU_PC_DMA_BASE_ADDR  0x0034  /* DMA 基址 */
#define RKNPU_PC_TASK_STATUS    0x003C  /* 任务执行状态 */

/* 版本魔数 "FIRE" */
#define RKNPU_VERSION_MAGIC     0x46495245UL

/* 中断状态清除值（清除所有 17 位）*/
#define RKNPU_INT_CLEAR_ALL     0x1FFFFU

/* ── 任务描述符 ──────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t flags;          /* 任务标志 */
    uint32_t op_idx;         /* 操作索引 */
    uint32_t enable_mask;    /* 核心使能掩码 */
    uint32_t int_mask;       /* 完成中断掩码 */
    uint32_t int_clear;      /* 中断清除值 */
    uint32_t int_status;     /* 预期中断状态 */
    uint32_t regcfg_amount;  /* 寄存器配置数量 */
    uint32_t regcfg_offset;  /* 寄存器配置偏移 */
    uint64_t regcmd_addr;    /* 寄存器命令物理地址 */
} rknpu_task_t;

/* ── 驱动配置（RK3588 特化参数）────────────────────────────────────────── */
typedef struct {
    uint32_t pc_data_amount_scale; /* 数据量对齐因子 = 2 */
    uint32_t pc_task_number_bits;  /* 任务编号位宽 = 12 */
    uint32_t pc_task_number_mask;  /* = (1<<12)-1 = 0xFFF */
    uint32_t pc_task_status_offset;/* 任务状态字段偏移 */
    uint32_t core_mask;            /* 使能核心掩码 = 0x7 (3核) */
} rknpu_config_t;

/* ── 公开 API ────────────────────────────────────────────────────────────── */

/**
 * rknpu_init - 初始化 NPU0
 *
 * 流程：
 *   1. 上电 NPU 电源域（PMU）
 *   2. 清除遗留中断
 *   3. 读取并验证版本寄存器
 *   4. 注册 GICv3 中断处理程序
 */
void rknpu_init(void);

/**
 * rknpu_validate_version - 打印并校验 NPU 版本寄存器
 * 返回 true 表示版本匹配（0x46495245 = "FIRE"）
 */
bool rknpu_validate_version(void);

#endif /* __RKNPU_H__ */
