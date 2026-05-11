/*
 * kernel/vmm/vmm.c — VM 管理与 vCPU 任务入口
 *
 * 架构无关代码（仅在 ARCH_AARCH64 下真正运行 VMM）。
 * 依赖：
 *   - kernel/vmm/aarch64/el2_run.c: vmm_run_vcpu()
 *   - kernel/mm/aarch64/stage2.c:   stage2_init()
 *   - kernel/task/task.h:           task_create() / task_exit()
 */

#include "vmm.h"
#include "klog.h"
#include "string.h"
#include "task/task.h"

#if ARCH_AARCH64
#include "aarch64/stage2.h"

/* 声明在 el2_run.c 中定义 */
extern int vmm_run_vcpu(vcpu_t *vcpu);

/* ── AArch64 VM 初始化 ────────────────────────────────────── */
static int aarch64_vm_init(vm_t *vm)
{
    int i;
    int nr = vm->cfg.nr_vcpus;

    if (nr < 1 || nr > MAX_VCPUS) {
        KLOG_ERROR("[vmm] vm_create: invalid nr_vcpus=%d\n", nr);
        return -1;
    }

    /* 初始化 Stage-2 页表（identity map）*/
    stage2_init(vm->cfg.mem_base, vm->cfg.mem_size);

    /* 初始化每个 vCPU 的状态 */
    for (i = 0; i < nr; i++) {
        vcpu_t *vcpu = &vm->vcpus[i];
        memset(vcpu, 0, sizeof(*vcpu));
        vcpu->vcpu_id  = i;
        vcpu->launched = 0;
        vcpu->vm       = vm;
        /* sysregs 全零：EL1 MMU 关、所有功能默认 off（对简单 bare-metal guest 安全）*/
    }
    vm->nr_vcpus = nr;

    KLOG_INFO("[vmm] vm_create: %d vCPU(s) initialized, mem=0x%llx+0x%llx\n",
              nr, vm->cfg.mem_base, vm->cfg.mem_size);
    return 0;
}
#endif /* ARCH_AARCH64 */

/* ── 公开 API ─────────────────────────────────────────────── */

int vm_create(vm_t *vm)
{
#if ARCH_AARCH64
    return aarch64_vm_init(vm);
#else
    (void)vm;
    KLOG_WARN("[vmm] vm_create: VMM not supported on this arch\n");
    return -1;
#endif
}

/* ── vCPU 任务入口（内核任务函数）────────────────────────── */
/*
 * vcpu_task_fn — 内核任务入口：运行 VMM 主循环
 *
 * 被 task_create() 创建的内核任务调用，arg = vcpu_t*。
 * 调用 vmm_run_vcpu() 进入/退出 guest 循环，完成后 task_exit()。
 */
static void vcpu_task_fn(void *arg)
{
    vcpu_t *vcpu = (vcpu_t *)arg;

    KLOG_INFO("[vmm] vcpu%d task started\n", vcpu->vcpu_id);

#if ARCH_AARCH64
    int rc = vmm_run_vcpu(vcpu);
    if (rc == 0)
        KLOG_INFO("[vmm] vcpu%d exited normally\n", vcpu->vcpu_id);
    else
        KLOG_ERROR("[vmm] vcpu%d exited with error %d\n", vcpu->vcpu_id, rc);
#else
    KLOG_WARN("[vmm] VMM not supported on this arch\n");
#endif

    task_exit();
}

struct task *vcpu_task_create(vcpu_t *vcpu, uint8_t priority)
{
    char name[TASK_NAME_LEN];
    /* 格式化任务名 "vcpu0" "vcpu1" ... */
    name[0] = 'v'; name[1] = 'c'; name[2] = 'p'; name[3] = 'u';
    name[4] = '0' + (char)(vcpu->vcpu_id & 0xF);
    name[5] = '\0';

    struct task *t = task_create(name, vcpu_task_fn, vcpu, priority);
    if (t)
        KLOG_INFO("[vmm] vcpu%d task created (id=%u)\n", vcpu->vcpu_id, t->id);
    else
        KLOG_ERROR("[vmm] failed to create vcpu%d task\n", vcpu->vcpu_id);
    return t;
}
