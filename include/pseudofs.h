/*
 * include/pseudofs.h — 虚拟文件系统 API
 *
 * 为 /dev、/proc、/sys 提供内核内建的虚拟路径。
 * syscall.c 在 openat/read/write/ioctl/stat/readlinkat/getdents64 中调用。
 *
 * 设备节点：
 *   /dev/null    — 丢弃写，读返回 EOF
 *   /dev/zero    — 读全零，写丢弃
 *   /dev/tty     — UART 读写
 *   /dev/console — 同 tty
 *   /dev/urandom — 伪随机字节流（LFSR）
 *   /dev/cvi-tpu0 — CVI TPU 驱动（ioctl 接口，SOPHGO CVITPU SDK ABI）
 *   /dev/ion      — ION DMA 分配器（ioctl 接口）
 *   /dev/npu     — NPU stub（保留）
 *   /dev/vmm     — guest 控制设备（ioctl 接口，见下方 VMM_IOC_*）
 *
 * proc 条目：
 *   /proc/self/exe      — 当前进程可执行路径（readlink）
 *   /proc/self/maps     — 空 stub
 *   /proc/self/status   — 简短 stub
 *   /proc/self/fd/N     — readlink → /dev/stdin 等
 *   /proc/mounts        — rootfs 挂载表
 *   /proc/meminfo       — 从 PMM 动态生成
 *   /proc/version       — 静态内核版本字符串
 *   /proc/uptime        — 真实 uptime + 各 CPU 累计 idle（timer_get_ns / idle_ticks）
 *   /proc/cpuinfo       — 架构信息
 */
#ifndef PSEUDOFS_H
#define PSEUDOFS_H

#include "types.h"
#include "kernel_stat.h"

/* ── _IOC 宏（freestanding 环境无 glibc） ────────────────────── */
#ifndef _IOC
#define _IOC(dir, t, nr, sz) \
    (((uint32_t)(dir) << 30) | ((uint32_t)(t) << 8) | (uint32_t)(nr) | \
     ((uint32_t)(sz) << 16))
#define _IOC_WRITE      1U
#define _IOC_READ       2U
#define _IO(t, nr)      _IOC(0, (t), (nr), 0)
#define _IOW(t, nr, T)  _IOC(_IOC_WRITE, (t), (nr), sizeof(T))
#define _IOR(t, nr, T)  _IOC(_IOC_READ, (t), (nr), sizeof(T))
#define _IOWR(t, nr, T) _IOC(_IOC_READ | _IOC_WRITE, (t), (nr), sizeof(T))
#endif

/* ── /dev/vmm ioctl（guest 控制设备）────────────────────────────
 *
 * ⚠️ 这套号必须与用户态 helper apps/c/vmm_run.c 里的一致。那边不能包含
 * 本头文件（这是内核头），所以用 musl 的 <sys/ioctl.h> 宏独立定义了一遍；
 * 两边的 _IOC 编码都是标准 Linux 布局，只要 type/nr/方向 相同就对得上。
 *
 * 为什么**所有**控制（含启动）都走 ioctl，而不是「首次写是命令」：
 * /dev/vmm 的 write() 是 guest 的串口输入。如果命令也走 write，设备就得分
 * 辨「这次写是命令还是数据」，而它只有一个全局的 booted 标志 —— 重新接入
 * 一个已在跑的 guest 时，booted 已经是 1，于是 helper 发的 "bootlinux"
 * 会被当成输入推进 guest，guest 把这个词回显出来（实测踩过）。
 * 全用 ioctl 之后，write 永远只是数据，不存在二义性。
 */
#define VMM_IOC_GET_STATUS _IOR('V', 0, uint32_t) /* 出参：1=guest 正在跑 */
#define VMM_IOC_DETACH     _IO('V', 1)            /* 本次 close 不停 guest */
#define VMM_IOC_STOP       _IO('V', 2)            /* 停止 guest */
#define VMM_IOC_BOOT       _IO('V', 3)            /* 启动 guest；已在跑则接入 */
/*
 * VMM_IOC_BOOT_EX — 强制**新建**一个 VM（多 VM 的入口）
 * 入参/出参：uint32_t *vmid。入参 0 = 自动分配；出参回填实际分到的 vmid。
 * 与 BOOT 的区别：BOOT 见到有 VM 在跑就接入它，本号总是新建一个。
 */
#define VMM_IOC_BOOT_EX _IOWR('V', 4, uint32_t)

/*
 * ── 多 VM 的按 vmid 操作（VMM_IOC_LIST / ATTACH / STOP_VM）─────
 *
 * 动机：在此之前，要停掉某个 VM **必须先接入它**（只有 VMM_IOC_STOP，而它
 * 打在"前台"那一个上），而接入路径又只挑最小 vmid —— 于是 vm1/vm2 同时在跑
 * 时，`vmm-run` 永远接到 vm1，你想停的 vm2 反而碰不到。这三个号把"按 vmid
 * 寻址"补齐：列出、接入指定、停指定，都不要求先接管控制台。
 *
 * ⚠️ struct 的定义必须与用户态 helper apps/c/vmm_run.c 里那份**逐字节一致**。
 *    helper 不能包含本头（会拖进 types.h / kernel_stat.h），只能手抄一份。
 *    _IOC 把 sizeof 编进了操作码（见上面 _IOC 宏），所以两边尺寸不同不是
 *    "错位"而是**换了个 ioctl 号** → 内核 default 分支回 -PFS_ENOSYS ——
 *    是响亮的失败，不是静默损坏。下面 _Static_assert 再钉一道，谁改字段
 *    谁在编译期就被拦住。改这里请同步改 vmm_run.c 的同名结构体。
 *
 * 为什么 info[] 是**固定容量 8**而不是 MAX_VMS：helper 看不到 MAX_VMS
 * （那是 include/vmm/vmm.h 的编译期常量），一旦把数组开成 MAX_VMS，
 * 改 MAX_VMS 就会悄悄改掉 ABI。固定 8 之后内核只填 min(max, MAX_VMS) 个，
 * 容量与池大小解耦（MAX_VMS 当前是 4）。
 */
struct vmm_vm_info {
    uint32_t vmid;
    uint32_t state; /* vm.c 的状态机数值：1=LOADING 2=RUNNING 4=DYING */
};
struct vmm_list_req {
    uint32_t max;     /* [in]  info[] 的容量（helper 传 8）      */
    uint32_t count;   /* [out] 实际写入的条目数                  */
    uint32_t fg_vmid; /* [out] 前台 vmid；0 = 没有前台           */
    uint32_t _rsv;    /* 对齐填充，保持 16 字节头                */
    struct vmm_vm_info info[8];
};

_Static_assert(sizeof(struct vmm_vm_info) == 8,
               "vmm_vm_info 尺寸变了 —— apps/c/vmm_run.c 里那份要同步");
_Static_assert(sizeof(struct vmm_list_req) == 80,
               "vmm_list_req 尺寸变了 —— apps/c/vmm_run.c 里那份要同步");

#define VMM_IOC_LIST    _IOWR('V', 5, struct vmm_list_req) /* 出参：VM 列表 */
#define VMM_IOC_ATTACH  _IOW('V', 6, uint32_t) /* 入参 vmid：设为前台 */
#define VMM_IOC_STOP_VM _IOW('V', 7, uint32_t) /* 入参 vmid：停指定 VM */

/* ── /dev/ion ioctl 结构体 & 请求码 ──────────────────────────── */
struct ion_alloc_req {
    uint64_t size;         /* [in/out] requested/actual allocation size */
    uint32_t heap_id_mask; /* [in] heap mask, accepted for ABI compat   */
    uint32_t flags;        /* [in] ION flags, accepted for ABI compat   */
    int32_t fd;            /* [out] dmabuf-like fd                      */
    uint32_t unused;
    uint64_t paddr; /* [out] physical address                    */
};
struct ion_get_req {
    uint32_t handle; /* [in]  句柄                           */
    uint32_t _pad;
    uint64_t paddr; /* [out] 物理地址                       */
    uint64_t vaddr; /* [out] 内核虚拟地址                   */
};
struct ion_size_req {
    uint32_t handle; /* [in]  句柄                           */
    uint32_t _pad;
    uint64_t size; /* [out] 分配大小（字节）               */
};

/* CVITEK/SOPHGO runtime 64-byte ION allocation ABI (ioctl 0xc0404900).
 * This extends ion_alloc_req with a 32-byte buffer name. */
struct ion_cvi_alloc_data {
    uint64_t size; /* [in/out] requested/actual allocation size */
    uint32_t heap_id_mask;
    uint32_t flags;
    int32_t fd; /* [out] ion handle, used as dmabuf fd       */
    uint32_t unused;
    uint64_t paddr; /* [out] physical address                    */
    char heap_name[32];
};

#define ION_IOC_ALLOC     _IOWR('I', 0, struct ion_alloc_req)
#define ION_IOC_FREE      _IOW('I', 1, uint32_t)
#define ION_IOC_GET       _IOWR('I', 2, struct ion_get_req)
#define ION_IOC_SIZE      _IOWR('I', 3, struct ion_size_req)
#define ION_IOC_CVI_ALLOC _IOWR('I', 0, struct ion_cvi_alloc_data)

/* Android ION ABI 标准命令（nr=5 IMPORT, nr=8 HEAP_QUERY） */
struct ion_fd_data {
    int32_t fd;      /* [in]  ion buffer fd（= ion handle）  */
    uint32_t handle; /* [out] ion 句柄                       */
};
struct ion_heap_data {
    char name[32];    /* 堆名称                               */
    uint32_t type;    /* 0=System 1=DmaCoherent 2=Carveout    */
    uint32_t heap_id; /* 堆 ID                                */
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
};
struct ion_heap_query {
    uint32_t cnt; /* [in/out] 堆数量                      */
    uint32_t reserved0;
    uint64_t heaps; /* [in]  ion_heap_data 数组用户空间指针 */
    uint32_t reserved1;
    uint32_t reserved2;
};

#define ION_IOC_IMPORT     _IOWR('I', 5, struct ion_fd_data)
#define ION_IOC_HEAP_QUERY _IOWR('I', 8, struct ion_heap_query)

/* ── /dev/cvi-tpu0 ioctl（SOPHGO CVITPU SDK ABI） ───────────── */
struct cvitpu_submit_dma_arg {
    int32_t fd;      /* [in]  Ion buffer fd（= ion handle） */
    uint32_t seq_no; /* [in]  序列号（同步驱动中忽略）      */
};
struct cvitpu_wait_dma_arg {
    uint32_t seq_no; /* [in]  序列号                        */
    int32_t ret;     /* [out] 结果（同步时恒为 0）          */
};
struct cvitpu_cache_op_arg {
    uint64_t paddr; /* 物理地址                            */
    uint64_t size;  /* 字节数                              */
};

struct cvitpu_legacy_cache_op_arg {
    uint64_t paddr; /* 64-byte aligned physical address     */
    uint64_t size;  /* 64-byte aligned length               */
    int32_t fd;     /* Ion buffer fd                        */
};

#define CVITPU_SUBMIT_DMABUF   _IOW('T', 1, struct cvitpu_submit_dma_arg)
#define CVITPU_WAIT_DMABUF     _IOWR('T', 2, struct cvitpu_wait_dma_arg)
#define CVITPU_LOAD_TEE        _IOW('T', 3, uint64_t)
#define CVITPU_SUBMIT_TEE      _IOW('T', 4, uint64_t)
#define CVITPU_UNLOAD_TEE      _IOW('T', 5, uint64_t)
#define CVITPU_PIO_MODE        _IO('T', 6)
#define CVITPU_DMABUF_FLUSH    _IOW('T', 7, struct cvitpu_cache_op_arg)
#define CVITPU_DMABUF_INVLD    _IOW('T', 8, struct cvitpu_cache_op_arg)
#define CVITPU_DMABUF_FLUSH_FD _IOW('T', 9, int32_t)
#define CVITPU_DMABUF_INVLD_FD _IOW('T', 10, int32_t)

/* CVITEK runtime on SG2002 also uses legacy 'p' requests. */
#define CVITPU_LEGACY_SUBMIT_DMABUF   _IOW('p', 1, uint64_t)
#define CVITPU_LEGACY_DMABUF_FLUSH_FD _IOW('p', 2, uint64_t)
#define CVITPU_LEGACY_DMABUF_INVLD_FD _IOW('p', 3, uint64_t)
#define CVITPU_LEGACY_DMABUF_FLUSH    _IOW('p', 4, uint64_t)
#define CVITPU_LEGACY_DMABUF_INVLD    _IOW('p', 5, uint64_t)
#define CVITPU_LEGACY_WAIT_DMABUF     _IOWR('p', 6, uint64_t)

/* ── /dev/npu ioctl（保留，暂无实现） ────────────────────────── */
/* 未来在此定义 NPU_IOC_* */

/* ── API ─────────────────────────────────────────────────────── */

/*
 * pseudo_open: 如果 abspath 是虚拟路径返回节点 ID（≥0），否则返回 -1。
 * 调用者收到 ≥0 时应通过 VFS 创建文件对象。
 */
int pseudo_open(const char *abspath);

/*
 * pseudo_read: 从节点 nid 的 *off 处读取 len 字节到 buf。
 * 成功时推进 *off，返回读取字节数；EOF 返回 0；错误返回负 errno。
 */
int pseudo_read(int nid, uint64_t *off, void *buf, size_t len);

/*
 * pseudo_write: 向节点 nid 写入 len 字节。
 * 返回写入字节数，或负 errno。
 */
int pseudo_write(int nid, const void *buf, size_t len);

/*
 * pseudo_ioctl: 向设备节点 nid 发送 ioctl 请求。
 * 返回 0 或负 errno。
 */
int pseudo_ioctl(int nid, uint64_t req, void *argp);

/*
 * pseudo_poll: 查询节点 nid 的就绪状态，返回 EPOLL* 位掩码。
 * 未实现 poll_fn 的节点按老行为返回 EPOLLIN|EPOLLOUT（永远就绪）。
 */
uint32_t pseudo_poll(int nid);

/*
 * pseudo_close: 节点 nid 的最后一个引用被关闭时调用（如 /dev/vmm 借此
 * 停止 guest）。未实现 close_fn 的节点是无操作。
 */
int pseudo_close(int nid);

/*
 * pseudo_stat_path: 对路径 abspath 填充 *st。
 * 返回 0 表示成功；-ENOENT 表示非虚拟路径。
 */
int pseudo_stat_path(const char *abspath, struct kernel_stat *st);

/*
 * pseudo_fill_stat: 对已知节点 nid 填充 *st。
 */
void pseudo_fill_stat(int nid, struct kernel_stat *st);

/*
 * pseudo_readlink: 解析 abspath 的符号链接目标，写入 buf（最多 bufsz 字节）。
 * 返回写入字节数，或负 errno（非链接路径返回 -EINVAL）。
 */
int pseudo_readlink(const char *abspath, char *buf, size_t bufsz);

/*
 * pseudo_getdents: 枚举伪目录节点 nid 下的条目。
 * *off 为已跳过条目计数，写入 buf，返回写入字节数。
 */
int pseudo_getdents(int nid, uint64_t *off, void *buf, size_t bufsz);

#endif /* PSEUDOFS_H */
