/*
 * driver/blk/sdblk.h — SG2002 / CV1811 SDMMC (SDHCI) 块设备公开接口
 *
 * 支持：
 *   - 单/多块 PIO 读写（轮询模式，无中断）
 *   - SDSC（CSD v1.0）/ SDHC / SDXC（CSD v2.0）/ SDUC（CSD v3.0）
 *   - 4 位总线宽度，UHS-I 1.8V 初始化路径
 *
 * 平台硬件地址由 platforms/<platform>/sdblk_cfg.h 提供；
 * 通过 Makefile 的 -Iplatforms/$(PLATFORM) 自动引入。
 */
#ifndef DRIVER_SDBLK_H
#define DRIVER_SDBLK_H

#include "types.h"

#define SDBLK_TOP_OFF_PWRSW_CTRL  0x1F4U         /* sd_pwrsw_ctrl 寄存器偏移 */

/* ── 块大小 ───────────────────────────────────────────────────── */
#define SDBLK_BLOCK_SIZE  512U

/* ── 错误码 ───────────────────────────────────────────────────── */
#define SDBLK_OK       0
#define SDBLK_NOCARD  (-2)   /* 卡未插入 */
#define SDBLK_ERR     (-1)   /* 通用错误（命令/数据传输失败） */

/* ── 公开 API ─────────────────────────────────────────────────── */

/**
 * sdblk_init — 初始化 SDMMC 控制器及 SD 卡
 *
 * 执行完整初始化序列：检测插卡 → 复位 → 设置电源/时钟 →
 * CMD0/CMD8/ACMD41 → CMD2/CMD3/CMD9 → CMD7 → ACMD6（4位宽）。
 *
 * 返回 SDBLK_OK，SDBLK_NOCARD（未插卡），或 SDBLK_ERR（失败）。
 */
int sdblk_init(void);

/**
 * sdblk_read_blocks — 读取连续块
 *
 * @block_id: 起始 LBA
 * @buf:      输出缓冲区，必须 >= count * SDBLK_BLOCK_SIZE 字节
 * @count:    块数（1 → CMD17；>1 → CMD18 + AutoCMD12）
 */
int sdblk_read_blocks(uint32_t block_id, void *buf, size_t count);

/**
 * sdblk_write_blocks — 写入连续块
 *
 * @block_id: 起始 LBA
 * @buf:      输入数据，必须 >= count * SDBLK_BLOCK_SIZE 字节
 * @count:    块数（1 → CMD24；>1 → CMD25 + AutoCMD12）
 */
int sdblk_write_blocks(uint32_t block_id, const void *buf, size_t count);

/**
 * sdblk_capacity_bytes  — 返回 SD 卡容量（字节），未初始化时返回 0
 * sdblk_capacity_blocks — 返回 SD 卡容量（512B 块数），未初始化时返回 0
 */
uint64_t sdblk_capacity_bytes(void);
uint64_t sdblk_capacity_blocks(void);

/* ── lwext4 块设备接口 ───────────────────────────────────────── */
struct ext4_blockdev;
struct ext4_blockdev *sdblk_get_bdev(void);

#endif /* DRIVER_SDBLK_H */
