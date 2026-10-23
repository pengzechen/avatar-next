/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/* driver/npu/rknpu.h

   Rockchip RK3588 NPU 驱动头文件

   硬件信息(来自 RK3588 设备树)：
     npu@fdab0000 — 三核心 NPU, 每核 64 KB MMIO 空间
     中断号: SPI 110/111/112 (GICv3 线号 142/143/144)

   参考: testos-reflector/src/npu/
*/

#ifndef __RKNPU_H__
#define __RKNPU_H__

#include "types.h"
#include "driver/npu.h" /* RKNPU_MAX_CORES / 寄存器偏移 / 公共 API */

/* ── 硬件基地址(与平台配置的 npu.base0/1/2 一致)──────────────────────── */
#define RKNPU_NPU0_BASE 0xFDAB0000UL
#define RKNPU_NPU1_BASE 0xFDAC0000UL
#define RKNPU_NPU2_BASE 0xFDAD0000UL

/* GIC SPI 中断号 = DT interrupt number + 32 */
#define RKNPU_NPU0_IRQ 142 /* SPI 110 + 32 */
#define RKNPU_NPU1_IRQ 143
#define RKNPU_NPU2_IRQ 144

/* PMU1(电源域控制)基地址 */
#define RKNPU_PMU1_BASE 0xFD8D8000UL

/*
   各计算单元的"单寄存器组指针"(S_POINTER)。提交前要写 0xe 切到第 0 组。
   注意： vendor 驱动(rknpu-0.9.8)把 0xe 当基址去算偏移, 参考项目
   testos-reflector 实测是把 0xe 写进这两个偏移才对的 —— 后者在
   RK3588 真机上跑通过 conv2d/matmul/pool 一批用例。
*/
#define RKNPU_CNA_S_POINTER   0x1004 /* CNA 单寄存器组指针 */
#define RKNPU_CORE_S_POINTER  0x3004 /* CORE 单寄存器组指针 */
#define RKNPU_S_POINTER_GROUP 0xE    /* 指向第 0 组 */

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

/* ── 驱动配置(RK3588 特化参数)────────────────────────────────────────── */
typedef struct {
    uint32_t pc_data_amount_scale;  /* 数据量对齐因子 = 2 */
    uint32_t pc_task_number_bits;   /* 任务编号位宽 = 12 */
    uint32_t pc_task_number_mask;   /* = (1<<12)-1 = 0xFFF */
    uint32_t pc_task_status_offset; /* 任务状态字段偏移 */
    uint32_t core_mask;             /* 使能核心掩码 = 0x7 (3核) */
} rknpu_config_t;

/* ── 公开 API ────────────────────────────────────────────────────────────── */

/*
   rknpu_init - 初始化 NPU0

   流程：
     1. 上电 NPU 电源域(PMU)
     2. 清除遗留中断
     3. 读取并验证版本寄存器
     4. 注册 GICv3 中断处理程序
*/
void rknpu_init(void);

/*
   rknpu_validate_version - 打印并校验某个核的版本寄存器
   @core: 核号 0..RKNPU_MAX_CORES-1
   返回 true 表示版本匹配(0x46495245 = "FIRE")
*/
bool rknpu_validate_version(int core);

/* 完整定义见 rknpu_drm.h(那是用户态 ABI 头) */
struct rknpu_submit;

/*
   rknpu_submit_task - 把一条作业交给 NPU0 并等它完成(阻塞)

   @task_base:  任务描述符数组的内核虚拟地址
   @task_bytes: 该缓冲区的大小(用来卡 task_start/task_number 越界)
   @s:          用户态提交参数(见 rknpu_drm.h 的 struct rknpu_submit)

   返回 0 成功；负 errno 失败(-EINVAL 参数/越界, -EIO 超时或中断状态不匹配,
   -ENODEV 驱动未初始化)。

   注意是阻塞的：内部按参考项目那样 1us 步进轮询完成标志, 上限
   s->timeout 毫秒。作业在微秒级完成, 正常路径几乎不占时间。
*/
int rknpu_submit_task(void *task_base, size_t task_bytes, const struct rknpu_submit *s);

#endif /* __RKNPU_H__ */
