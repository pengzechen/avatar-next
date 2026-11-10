/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/fs/pseudofs/rtctl_dev.c — /dev/rtctl

   用户态控制面：创建/启动/停止/销毁 RT worker，更新输入、读取输出与统计。
   数据面在 kernel/rt/rt_worker.c。ABI 见 include/rt_worker.h。

   权限：V1 只允许 root（uid 0）。RT 优先级和模板能力将来再细化为
   capability，不在这一版里假装已经做好。
*/

#include "pseudofs.h"
#include "pseudofs_internal.h"
#include "rt_worker.h"
#include "task/task.h"
#include "syscall/syscall_internal.h" /* copy_from/to_user_bytes */

#define RTCTL_EPERM  1
#define RTCTL_EINVAL 22

static bool rtctl_is_root(void)
{
    task_t *t = task_current();
    return t && t->uid == 0;
}

int rtctl_dev_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid;

    if (!rtctl_is_root())
        return -RTCTL_EPERM;

    rt_worker_init();

    switch ((uint32_t)req) {
    case RTW_IOC_CREATE: {
        struct rtw_create_req r;
        if (!argp || copy_from_user_bytes(argp, &r, sizeof(r)) < 0)
            return -RTCTL_EINVAL;

        int id = rt_worker_create(&r);
        if (id < 0)
            return id;

        r.id = (uint32_t)id;
        if (copy_to_user_bytes(&r, argp, sizeof(r)) < 0) {
            (void)rt_worker_destroy((uint32_t)id);
            return -RTCTL_EINVAL;
        }
        return 0;
    }
    case RTW_IOC_DESTROY:
    case RTW_IOC_START:
    case RTW_IOC_STOP: {
        struct rtw_id_req r;
        if (!argp || copy_from_user_bytes(argp, &r, sizeof(r)) < 0)
            return -RTCTL_EINVAL;
        if ((uint32_t)req == RTW_IOC_DESTROY)
            return rt_worker_destroy(r.id);
        if ((uint32_t)req == RTW_IOC_START)
            return rt_worker_start(r.id);
        return rt_worker_stop(r.id);
    }
    case RTW_IOC_SET_IN: {
        struct rtw_io_req r;
        if (!argp || copy_from_user_bytes(argp, &r, sizeof(r)) < 0)
            return -RTCTL_EINVAL;
        return rt_worker_set_input(r.id, r.value);
    }
    case RTW_IOC_GET_OUT: {
        struct rtw_io_req r;
        if (!argp || copy_from_user_bytes(argp, &r, sizeof(r)) < 0)
            return -RTCTL_EINVAL;
        uint64_t value = 0, seq = 0;
        int rc = rt_worker_get_output(r.id, &value, &seq);
        if (rc < 0)
            return rc;
        r.value = value;
        r.seq = seq;
        if (copy_to_user_bytes(&r, argp, sizeof(r)) < 0)
            return -RTCTL_EINVAL;
        return 0;
    }
    case RTW_IOC_STATS: {
        struct rtw_stats st;
        if (!argp || copy_from_user_bytes(argp, &st, sizeof(st)) < 0)
            return -RTCTL_EINVAL;
        int rc = rt_worker_get_stats(st.id, &st);
        if (rc < 0)
            return rc;
        if (copy_to_user_bytes(&st, argp, sizeof(st)) < 0)
            return -RTCTL_EINVAL;
        return 0;
    }
    case RTW_IOC_LIST: {
        struct rtw_list_req lst;
        if (!argp || copy_from_user_bytes(argp, &lst, sizeof(lst)) < 0)
            return -RTCTL_EINVAL;
        int rc = rt_worker_list(&lst);
        if (rc < 0)
            return rc;
        if (copy_to_user_bytes(&lst, argp, sizeof(lst)) < 0)
            return -RTCTL_EINVAL;
        return 0;
    }
    default:
        return -RTCTL_EINVAL;
    }
}
