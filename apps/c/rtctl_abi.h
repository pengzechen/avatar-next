/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef AVATAR_RTCTL_ABI_H
#define AVATAR_RTCTL_ABI_H

/*
   /dev/rtctl 用户态 ABI（权威定义在 include/rt_worker.h）。
   结构体尺寸被编进 ioctl 号，改了这里必须同步内核那份。
*/

#include <stdint.h>

#define RTW_ABI_VERSION    1
#define RTW_NAME_LEN       16
#define RTW_DEV_NAME_LEN   16
#define RTW_MAX_WORKERS    8
#define RTW_PARAM_MAX      8
#define RTSHM_MAGIC        0x52545753U
#define RTSHM_VERSION      2U
#define RTSHM_EVENT_OFFSET 192U
#define RTW_EVENT_RING_CAP 64U

enum {
    RTW_TPL_PERIODIC_IO = 1,
    RTW_TPL_PID = 2,
    RTW_TPL_WATCHDOG = 3,
    RTW_TPL_EVENT = 4
};

enum {
    RTW_STATE_FREE = 0,
    RTW_STATE_CREATED,
    RTW_STATE_RUNNING,
    RTW_STATE_STOPPED,
    RTW_STATE_FAULTED
};

struct rtw_create_req {
    uint32_t abi_version;
    uint32_t template_id;
    char name[RTW_NAME_LEN];
    char dev[RTW_DEV_NAME_LEN];
    uint8_t priority;
    uint8_t _pad0[3];
    uint32_t cpu;
    uint64_t period_ns;
    uint64_t budget_ns;
    uint64_t params[RTW_PARAM_MAX];
    uint32_t id;
    uint32_t _pad1;
};

struct rtw_id_req {
    uint32_t id;
    uint32_t _pad;
};

struct rtw_io_req {
    uint32_t id;
    uint32_t _pad;
    uint64_t seq;
    uint64_t value;
};

struct rtw_params_req {
    uint32_t id;
    uint32_t _pad;
    uint64_t params[RTW_PARAM_MAX];
};

struct rtw_shm {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t state;
    uint32_t _pad0;

    volatile uint64_t input;
    volatile uint64_t output;
    volatile uint64_t out_seq;

    volatile uint64_t params[RTW_PARAM_MAX];
    volatile uint32_t params_seq;
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
    uint64_t misses;
    uint64_t overruns;
    uint64_t exec_last_ns;
    uint64_t exec_max_ns;
    uint64_t wake_late_last_ns;
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

struct rtw_event_ring {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t cap;
    uint32_t _pad;
    volatile uint64_t posted;
    volatile uint64_t dropped;
    volatile uint64_t processed;
    volatile uint64_t last_value;
    volatile uint64_t ev[RTW_EVENT_RING_CAP];
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
    uint32_t count;
    uint32_t capacity;
    struct rtw_list_entry entries[RTW_MAX_WORKERS];
};

_Static_assert(sizeof(struct rtw_create_req) == 136, "rtw_create_req ABI mismatch");
_Static_assert(sizeof(struct rtw_io_req) == 24, "rtw_io_req ABI mismatch");
_Static_assert(sizeof(struct rtw_params_req) == 72, "rtw_params_req ABI mismatch");
_Static_assert(sizeof(struct rtw_stats) == 184, "rtw_stats ABI mismatch");
_Static_assert(sizeof(struct rtw_shm) == 176, "rtw_shm ABI mismatch");
_Static_assert(sizeof(struct rtw_event_ring) == 560, "rtw_event_ring ABI mismatch");
_Static_assert(sizeof(struct rtw_list_entry) == 64, "rtw_list_entry ABI mismatch");
_Static_assert(sizeof(struct rtw_list_req) == 520, "rtw_list_req ABI mismatch");

#ifndef _IOC
#define _IOC(dir, t, nr, sz) (((uint32_t)(dir) << 30) | ((uint32_t)(t) << 8) | (uint32_t)(nr) | ((uint32_t)(sz) << 16))
#define _IOC_WRITE           1U
#define _IOC_READ            2U
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

static inline struct rtw_event_ring *rtw_shm_events(struct rtw_shm *s)
{
    return (struct rtw_event_ring *)((char *)s + RTSHM_EVENT_OFFSET);
}

#endif /* AVATAR_RTCTL_ABI_H */
