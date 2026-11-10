/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef RT_WORKER_H
#define RT_WORKER_H

#include "types.h"

/*
   rt_worker.h — 用户可配置的 RT worker（模板）控制面 ABI

   设计：
     - 数据面：内核 RT 任务，按 period_ns 周期调用一个模板的 step()。
       step 有界、无分配、无睡眠、无 VFS，只读写预分配状态和参数。
     - 控制面：用户态通过 /dev/rtctl 的 ioctl 创建/启动/停止/喂输入/取输出。
     - 设备：模板的输出可选绑定到一个 rtdev（见 include/rtdev.h）。
       设备在 CREATE 时独占绑定，STOP/FAULT 时调用 safe_state()。

   注意：apps/c/rtctl.c 里有一份逐字节相同的 ABI 副本。改这里必须同步改
   用户态那份（结构体尺寸也被编进 ioctl 号，尺寸错了就是另一个命令）。
*/

#define RTW_ABI_VERSION  1
#define RTW_NAME_LEN     16
#define RTW_DEV_NAME_LEN 16
#define RTW_MAX_WORKERS  8
#define RTW_PARAM_MAX    8

/* V1 的周期精度由 100Hz tick 决定；必须是 10ms 的整数倍。 */
#define RTW_MIN_PERIOD_NS 10000000ULL

enum rtw_template_id {
    RTW_TPL_PERIODIC_IO = 1, /* 增益/偏置/限幅 */
    RTW_TPL_PID = 2,         /* 定点 PID */
};

enum rtw_state {
    RTW_STATE_FREE = 0,
    RTW_STATE_CREATED, /* 已配置，尚未创建 RT 任务 */
    RTW_STATE_RUNNING,
    RTW_STATE_STOPPED,
    RTW_STATE_FAULTED,
};

/* ── ioctl ABI ─────────────────────────────────────────────── */

struct rtw_create_req {
    uint32_t abi_version;
    uint32_t template_id;

    char name[RTW_NAME_LEN];
    /* 可选：输出绑定的 rtdev 名称；全 0 表示不绑定设备 */
    char dev[RTW_DEV_NAME_LEN];

    uint8_t priority; /* 1..99 */
    uint8_t _pad0[3];
    uint32_t cpu; /* 0..g_num_cpus-1 */

    uint64_t period_ns;
    uint64_t budget_ns; /* 0 = 不做超预算统计 */

    /*
       模板参数，定点整数（Q16 表示 1.0 = 65536）：
         periodic_io: [0]=gain_q16 [1]=offset [2]=min [3]=max
         pid:         [0]=kp_q16 [1]=ki_q16 [2]=kd_q16
                      [3]=out_min [4]=out_max [5]=setpoint
                      [6]=integral_limit
    */
    uint64_t params[RTW_PARAM_MAX];

    uint32_t id; /* [out] worker id（1..RTW_MAX_WORKERS） */
    uint32_t _pad1;
};

struct rtw_id_req {
    uint32_t id;
    uint32_t _pad;
};

struct rtw_io_req {
    uint32_t id;
    uint32_t _pad;
    uint64_t seq;   /* GET_OUT 出参：输出序号 */
    uint64_t value; /* SET_IN 入参 / GET_OUT 出参 */
};

struct rtw_stats {
    uint32_t id;
    uint32_t state;
    uint32_t template_id;
    uint32_t priority;
    uint32_t cpu;
    uint32_t _pad;
    uint64_t period_ns;
    uint64_t budget_ns;
    uint64_t ticks;
    uint64_t misses;   /* 周期错过的次数（超过半个周期） */
    uint64_t overruns; /* step 执行超过 budget_ns 的次数 */
    uint64_t exec_last_ns;
    uint64_t exec_max_ns;
    uint64_t last_input;
    uint64_t last_output;
    uint64_t out_seq;
    uint64_t dev_writes;
    char name[RTW_NAME_LEN];
    char dev[RTW_DEV_NAME_LEN];
};

struct rtw_list_entry {
    uint32_t id;
    uint32_t state;
    uint32_t template_id;
    uint32_t priority;
    uint32_t cpu;
    uint32_t _pad;
    uint64_t period_ns;
    uint64_t ticks;
    uint64_t misses;
    char name[RTW_NAME_LEN];
};

struct rtw_list_req {
    uint32_t count;    /* [out] 实际写入的条目数 */
    uint32_t capacity; /* [in] entries[] 容量 */
    struct rtw_list_entry entries[RTW_MAX_WORKERS];
};

/* _IOC 宏：与 dma_buf.h/pseudofs.h 里那份相同，`#ifndef` 保证不重定义。 */
#ifndef _IOC
#define _IOC(dir, t, nr, sz) (((uint32_t)(dir) << 30) | ((uint32_t)(t) << 8) | (uint32_t)(nr) | ((uint32_t)(sz) << 16))
#define _IOC_WRITE           1U
#define _IOC_READ            2U
#define _IO(t, nr)           _IOC(0, (t), (nr), 0)
#define _IOW(t, nr, T)       _IOC(_IOC_WRITE, (t), (nr), sizeof(T))
#define _IOR(t, nr, T)       _IOC(_IOC_READ, (t), (nr), sizeof(T))
#define _IOWR(t, nr, T)      _IOC(_IOC_READ | _IOC_WRITE, (t), (nr), sizeof(T))
#endif

#define RTW_IOC_CREATE  _IOWR('R', 1, struct rtw_create_req)
#define RTW_IOC_DESTROY _IOWR('R', 2, struct rtw_id_req)
#define RTW_IOC_START   _IOWR('R', 3, struct rtw_id_req)
#define RTW_IOC_STOP    _IOWR('R', 4, struct rtw_id_req)
#define RTW_IOC_SET_IN  _IOWR('R', 5, struct rtw_io_req)
#define RTW_IOC_GET_OUT _IOWR('R', 6, struct rtw_io_req)
#define RTW_IOC_STATS   _IOWR('R', 7, struct rtw_stats)
#define RTW_IOC_LIST    _IOWR('R', 8, struct rtw_list_req)

/* ── 内核内部 API ──────────────────────────────────────────── */

void rt_worker_init(void); /* 幂等；注册内置 rtdev */

int rt_worker_create(const struct rtw_create_req *cfg);
int rt_worker_destroy(uint32_t id);
int rt_worker_start(uint32_t id);
int rt_worker_stop(uint32_t id);
int rt_worker_set_input(uint32_t id, uint64_t value);
int rt_worker_get_output(uint32_t id, uint64_t *value, uint64_t *seq);
int rt_worker_get_stats(uint32_t id, struct rtw_stats *out);
int rt_worker_list(struct rtw_list_req *list);

#endif /* RT_WORKER_H */
