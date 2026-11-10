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

/*
   周期下限 1ms。x86_64 用 TSC-deadline 时可到 1ms；没有 oneshot 的
   架构（aarch64/riscv64 当前）仍要求 10ms 的整数倍，由内核在 CREATE 时拒绝。
   event 模板例外：period_ns=0 表示无限等待事件。
*/
#define RTW_MIN_PERIOD_NS 1000000ULL

enum rtw_template_id {
    RTW_TPL_PERIODIC_IO = 1, /* 增益/偏置/限幅 */
    RTW_TPL_PID = 2,         /* 定点 PID */
    RTW_TPL_WATCHDOG = 3,    /* 监控另一个 worker 的进度 */
    RTW_TPL_EVENT = 4,       /* 事件驱动：阻塞等事件，每个事件执行一次变换 */
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

    uint64_t period_ns; /* event: 0 = 无限等待，>0 = 最长等待/超时 */
    uint64_t budget_ns; /* 0 = 不做超预算统计 */

    /*
       模板参数，定点整数（Q16 表示 1.0 = 65536）：
         periodic_io: [0]=gain_q16 [1]=offset [2]=min [3]=max
         event:       同 periodic_io，输入来自事件环
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
    uint64_t value; /* SET_IN 入参 / POST 事件值 / GET_OUT 出参 */
};

struct rtw_params_req {
    uint32_t id;
    uint32_t _pad;
    uint64_t params[RTW_PARAM_MAX];
};

/*
   共享内存页（每个 worker 槽位一页，mmap offset = worker_id << PAGE_SHIFT）。

   用户态 supervisor 可以 mmap 这块页，直接读写 input/output/params/stats，
   控制环不需要 ioctl。内核与用户都只使用 64-bit 对齐的原子访问。
   这是“每槽位持久页”：worker destroy 时只清空内容，不释放物理页，
   所以旧映射不会指向已归还 PMM 的内存；重新 create 同槽位会重新初始化。
*/
#define RTSHM_MAGIC   0x52545753U /* "RTWS" */
#define RTSHM_VERSION 2U          /* v2: event ring at RTSHM_EVENT_OFFSET */

struct rtw_shm {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t state;
    uint32_t _pad0;

    volatile uint64_t input;
    volatile uint64_t output;
    volatile uint64_t out_seq;

    volatile uint64_t params[RTW_PARAM_MAX];
    volatile uint32_t params_seq; /* 奇数 = 正在更新 */
    uint32_t _pad1;

    volatile uint64_t ticks;
    volatile uint64_t misses;
    volatile uint64_t overruns;
    volatile uint64_t exec_last_ns;
    volatile uint64_t exec_max_ns;
    volatile uint64_t wake_late_last_ns;
    volatile uint64_t wake_late_max_ns;
    volatile uint64_t dev_writes;
};

_Static_assert(sizeof(struct rtw_shm) == 176, "rtw_shm ABI mismatch");

/*
   事件环放在共享页的固定偏移，紧跟 rtw_shm，避免改动已有字段的偏移。
   SPSC：生产者（控制面 / 驱动 ISR，内核侧用 ev_lock 串行化，未来也允许
   用户写）递增 head；消费者（RT worker）递增 tail。容量固定 64，索引
   自由回绕，用 head-tail 判满/空。
*/
#define RTSHM_EVENT_OFFSET 192U
#define RTW_EVENT_RING_CAP 64U

struct rtw_event_ring {
    volatile uint32_t head; /* 生产者索引 */
    volatile uint32_t tail; /* 消费者索引 */
    uint32_t cap;           /* = RTW_EVENT_RING_CAP */
    uint32_t _pad;

    volatile uint64_t posted;    /* 成功入环总数 */
    volatile uint64_t dropped;   /* 环满丢弃总数 */
    volatile uint64_t processed; /* RT 侧已处理总数 */
    volatile uint64_t last_value;

    volatile uint64_t ev[RTW_EVENT_RING_CAP];
};

_Static_assert(sizeof(struct rtw_event_ring) == 560, "rtw_event_ring ABI mismatch");

static inline struct rtw_event_ring *rtw_shm_events(struct rtw_shm *s)
{
    return (struct rtw_event_ring *)((uint8_t *)s + RTSHM_EVENT_OFFSET);
}

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
    uint64_t wake_late_last_ns; /* 本次唤醒比 deadline 晚多少 */
    uint64_t wake_late_max_ns;
    uint64_t last_input;
    uint64_t last_output;
    uint64_t out_seq;
    uint64_t dev_writes;
    uint64_t events_posted;
    uint64_t events_processed;
    uint64_t events_dropped;
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

#define RTW_IOC_CREATE     _IOWR('R', 1, struct rtw_create_req)
#define RTW_IOC_DESTROY    _IOWR('R', 2, struct rtw_id_req)
#define RTW_IOC_START      _IOWR('R', 3, struct rtw_id_req)
#define RTW_IOC_STOP       _IOWR('R', 4, struct rtw_id_req)
#define RTW_IOC_SET_IN     _IOWR('R', 5, struct rtw_io_req)
#define RTW_IOC_GET_OUT    _IOWR('R', 6, struct rtw_io_req)
#define RTW_IOC_STATS      _IOWR('R', 7, struct rtw_stats)
#define RTW_IOC_LIST       _IOWR('R', 8, struct rtw_list_req)
#define RTW_IOC_SET_PARAMS _IOWR('R', 9, struct rtw_params_req)
#define RTW_IOC_POST       _IOWR('R', 10, struct rtw_io_req)

/* ── 内核内部 API ──────────────────────────────────────────── */

void rt_worker_init(void); /* 幂等；注册内置 rtdev */

int rt_worker_create(const struct rtw_create_req *cfg);
int rt_worker_destroy(uint32_t id);
int rt_worker_start(uint32_t id);
int rt_worker_stop(uint32_t id);
int rt_worker_set_input(uint32_t id, uint64_t value);
int rt_worker_set_params(uint32_t id, const uint64_t params[RTW_PARAM_MAX]);
int rt_worker_get_output(uint32_t id, uint64_t *value, uint64_t *seq);
int rt_worker_get_stats(uint32_t id, struct rtw_stats *out);
int rt_worker_list(struct rtw_list_req *list);

/* 供 watchdog 模板读目标 worker 的进度；返回 false 表示目标不存在。 */
bool rt_worker_peek_progress(uint32_t id, uint64_t *seq, uint32_t *state);

/* 共享页物理地址，供 /dev/rtctl mmap（offset = worker_id << PAGE_SHIFT）使用。 */
int rt_worker_shm_lookup(uint32_t id, uint64_t *paddr, uint64_t *size);

/*
   向 worker 的事件环投递一个 u64 事件并唤醒它。
   环满返回 -ENOSPC（事件被丢弃，dropped 计数 +1）。
   可在 ISR 中调用（只做短临界区 + rt_event_set）。
*/
int rt_worker_post_event(uint32_t id, uint64_t value);

#endif /* RT_WORKER_H */
