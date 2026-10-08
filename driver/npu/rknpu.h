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

/*
 * NPU 核数。rk3588 是 3 —— 三个核各有独立的 MMIO 窗口和中断线，
 * 但**共享同一套 PC 侧寄存器偏移**（每个核内相对偏移都一样）。
 */
#define RKNPU_MAX_CORES 3

/* ── 硬件基地址（与平台配置的 npu.base0/1/2 一致）──────────────────────── */
#define RKNPU_NPU0_BASE 0xFDAB0000UL
#define RKNPU_NPU1_BASE 0xFDAC0000UL
#define RKNPU_NPU2_BASE 0xFDAD0000UL

/* GIC SPI 中断号 = DT interrupt number + 32 */
#define RKNPU_NPU0_IRQ 142 /* SPI 110 + 32 */
#define RKNPU_NPU1_IRQ 143
#define RKNPU_NPU2_IRQ 144

/* PMU1（电源域控制）基地址 */
#define RKNPU_PMU1_BASE 0xFD8D8000UL

/* ── 寄存器偏移量（每个核心内相对偏移）──────────────────────────────────── */
#define RKNPU_VERSION          0x0000 /* 版本魔数，应为 0x46495245 */
#define RKNPU_VERSION_NUM      0x0004 /* 子版本号 */
#define RKNPU_PC_OP_EN         0x0008 /* 写 1 提交，写 0 清除 */
#define RKNPU_PC_DATA_ADDR     0x0010 /* DMA 描述符地址 */
#define RKNPU_PC_DATA_AMOUNT   0x0014 /* 描述符数量 */
#define RKNPU_INT_MASK         0x0020 /* 中断掩码 */
#define RKNPU_INT_CLEAR        0x0024 /* 写 1 清除中断 */
#define RKNPU_INT_STATUS       0x0028 /* 中断状态（含任务计数） */
#define RKNPU_INT_RAW_STATUS   0x002C /* 原始中断状态 */
#define RKNPU_PC_TASK_CONTROL  0x0030 /* 任务控制寄存器 */
#define RKNPU_PC_DMA_BASE_ADDR 0x0034 /* DMA 基址 */
#define RKNPU_PC_TASK_STATUS   0x003C /* 任务执行状态 */

/*
 * 各计算单元的"单寄存器组指针"（S_POINTER）。提交前要写 0xe 切到第 0 组。
 * ⚠️ vendor 驱动（rknpu-0.9.8）把 0xe 当**基址**去算偏移，参考项目
 * testos-reflector 实测是把 0xe 写进**这两个偏移**才对的 —— 后者在
 * RK3588 真机上跑通过 conv2d/matmul/pool 一批用例。
 */
#define RKNPU_CNA_S_POINTER   0x1004 /* CNA 单寄存器组指针 */
#define RKNPU_CORE_S_POINTER  0x3004 /* CORE 单寄存器组指针 */
#define RKNPU_S_POINTER_GROUP 0xE    /* 指向第 0 组 */

/* 版本魔数 "FIRE" */
#define RKNPU_VERSION_MAGIC 0x46495245UL

/* 中断状态清除值（清除所有 17 位）*/
#define RKNPU_INT_CLEAR_ALL 0x1FFFFU

/*
 * PC_DATA_AMOUNT 的固定附加量。算法（来自 vendor 驱动）：
 *     amount = (regcfg_amount + EXTRA + scale - 1) / scale - 1
 * 其中 scale = rknpu_config_t.pc_data_amount_scale = 2。
 */
#define RKNPU_PC_DATA_EXTRA_AMOUNT 4U

/* ── 任务描述符 ──────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t flags;         /* 任务标志 */
    uint32_t op_idx;        /* 操作索引 */
    uint32_t enable_mask;   /* 核心使能掩码 */
    uint32_t int_mask;      /* 完成中断掩码 */
    uint32_t int_clear;     /* 中断清除值 */
    uint32_t int_status;    /* 预期中断状态 */
    uint32_t regcfg_amount; /* 寄存器配置数量 */
    uint32_t regcfg_offset; /* 寄存器配置偏移 */
    uint64_t regcmd_addr;   /* 寄存器命令物理地址 */
} rknpu_task_t;

/* ── 驱动配置（RK3588 特化参数）────────────────────────────────────────── */
typedef struct {
    uint32_t pc_data_amount_scale;  /* 数据量对齐因子 = 2 */
    uint32_t pc_task_number_bits;   /* 任务编号位宽 = 12 */
    uint32_t pc_task_number_mask;   /* = (1<<12)-1 = 0xFFF */
    uint32_t pc_task_status_offset; /* 任务状态字段偏移 */
    uint32_t core_mask;             /* 使能核心掩码 = 0x7 (3核) */
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
 * rknpu_host_release - 把 NPU 让给 guest（VMM 透传用）
 *
 * 关掉宿主 GIC 上 NPU 的三条中断线，并让 rknpu_submit_task 从此拒绝作业。
 * 不这么做的话，宿主 ISR 会抢在 VMM 注入之前把 INT_STATUS 清掉，guest
 * 永远等不到中断 —— 而且宿主侧一行错误日志都不会有。
 *
 * 幂等；无 NPU 或未初始化时是空操作。由 vnpu_init() 在建 VM 时调用。
 */
void rknpu_host_release(void);

/**
 * rknpu_host_reclaim - 从 guest 手里收回 NPU
 *
 * 与 rknpu_host_release 严格逆序（先清状态位再开中断线），VM 销毁时调用。
 * 幂等；无 NPU 时是空操作。
 */
void rknpu_host_reclaim(void);

/**
 * rknpu_validate_version - 打印并校验某个核的版本寄存器
 * @core: 核号 0..RKNPU_MAX_CORES-1
 * 返回 true 表示版本匹配（0x46495245 = "FIRE"）
 */
bool rknpu_validate_version(int core);

/* 完整定义见 rknpu_drm.h（那是用户态 ABI 头）*/
struct rknpu_submit;

/**
 * rknpu_submit_task - 把一条作业交给 NPU0 并等它完成（阻塞）
 *
 * @task_base:  任务描述符数组的**内核虚拟地址**
 * @task_bytes: 该缓冲区的大小（用来卡 task_start/task_number 越界）
 * @s:          用户态提交参数（见 rknpu_drm.h 的 struct rknpu_submit）
 *
 * 返回 0 成功；负 errno 失败（-EINVAL 参数/越界，-EIO 超时或中断状态不匹配，
 * -ENODEV 驱动未初始化）。
 *
 * 注意是**阻塞**的：内部按参考项目那样 1us 步进轮询完成标志，上限
 * s->timeout 毫秒。作业在微秒级完成，正常路径几乎不占时间。
 */
int rknpu_submit_task(void *task_base, size_t task_bytes,
                      const struct rknpu_submit *s);

#endif /* __RKNPU_H__ */
