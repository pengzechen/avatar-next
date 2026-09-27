/*
 * kernel/vmm/vm.c — VM 对象：池、生命周期、创建
 *
 * 从 vmm.c 拆出来的。这里只管「一个 guest VM 这个对象本身」：
 *   - 静态 VM 池与槽位分配（vm_alloc / vm_get / vm_free / vm_count_used）
 *   - 生命周期状态机（VM_FREE → LOADING → RUNNING → DYING → FREE）
 *   - 停止请求与"有没有 VM 在跑"的查询
 *   - vm_create：按架构转发到各后端的初始化
 *
 * 不在这里的：
 *   - vCPU 内核任务（vcpu.c）—— 它只是**执行**这个 VM 的载体
 *   - 主循环 vmm_run_vcpu（vmm.c）
 *   - 各架构的初始化实现（aarch64/vm_init.c、x86_64/vmx.c、riscv64/hext_run.c）
 *
 * ⚠️ 本文件不得依赖任何具体的中断控制器 —— 架构状态由 vm_t 里的守卫块承载，
 *    控制器注册集中在各架构的 vm_init 里。（第二轮做中断抽象时的落点。）
 */

#include "vmm/vmm.h"
#include "klog.h"
#include "spinlock.h"     /* g_vm_pool_lock */
#include "string.h"       /* memset（vm_alloc / vm_free）*/

#if ARCH_AARCH64
/* 实现在 kernel/vmm/aarch64/vm_init.c（Step 4b 会统一成 vmm_arch_vm_init）。*/
extern int aarch64_vm_init(vm_t *vm);
#endif

/* ── 公开 API ─────────────────────────────────────────────── */

int vm_create(vm_t *vm)
{
#if ARCH_AARCH64
    return aarch64_vm_init(vm);
#elif ARCH_X86_64
    extern int vmx_vm_init(vm_t *vm);
    return vmx_vm_init(vm);
#elif ARCH_RISCV64
    extern int hext_vm_init(vm_t *vm);
    return hext_vm_init(vm);
#endif
}

/* ── 宿主侧 guest 生命周期：VM 池 ─────────────────────────────
 *
 * 从前这里只有一个 VM —— guest_loader.c 里一个 `static vm_t vm`，加上下面
 * 两个全局标志（running / stop）。那套从数据结构上就假定了"内核里最多一个
 * VM"：第二个 VM 会直接覆盖第一个的对象，而 stop 请求也没法说清是给谁的。
 *
 * 现在改成静态池。为什么不用动态分配：vm_t 里嵌着 vGIC（GICv3 时约 70 KB）
 * 与 stage-2 静态表，sizeof 有几十 KB，动态分配要几十页**连续**物理内存、
 * 还会丢掉表所需的页对齐保证；静态池一共几百 KB BSS，相对内核窗口可忽略。
 *
 * 并发规则（重要）：
 *   - 槽位与 state 由 g_vm_pool_lock 保护（IRQ-safe：vmm_dev 的任务上下文
 *     和 vCPU 任务上下文都会碰）；
 *   - **跨任务只传 vmid，不传 vm_t *，也不做引用计数** —— 谁要操作某个 VM
 *     就现场 vm_get(vmid) 取一次、用完即放；
 *   - **销毁由该 VM 的 vCPU 任务自己完成**（vmm_run_vcpu 返回后调 vm_free），
 *     杜绝"任务还在跑、槽位已被回收"。外部想销毁就置 stop_req 等它收拾。
 */
static vm_t g_vm_pool[MAX_VMS];
static spinlock_noirq_t g_vm_pool_lock = SPINLOCK_NOIRQ_INIT;
static uint32_t g_next_vmid = 1;

vm_t *vm_alloc(void)
{
    uint64_t flags;
    vm_t *found = NULL;
    int i;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    for (i = 0; i < MAX_VMS; i++) {
        if (g_vm_pool[i].state == VM_FREE) {
            found = &g_vm_pool[i];
            memset(found, 0, sizeof(*found));
            found->slot  = i;
            found->vmid  = g_next_vmid;
            g_next_vmid  = (g_next_vmid >= 255) ? 1 : (g_next_vmid + 1);
            found->state = VM_LOADING;
            break;
        }
    }
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);

    if (found)
        KLOG_INFO("[vmm] vm%u allocated (slot %d)\n", found->vmid, found->slot);
    else
        KLOG_WARN("[vmm] vm_alloc: no free slot (MAX_VMS=%d)\n", MAX_VMS);
    return found;
}

vm_t *vm_get(uint32_t vmid)
{
    uint64_t flags;
    vm_t *found = NULL;
    int i;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    for (i = 0; i < MAX_VMS; i++) {
        if (g_vm_pool[i].state != VM_FREE && g_vm_pool[i].vmid == vmid) {
            found = &g_vm_pool[i];
            break;
        }
    }
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);
    return found;
}

/*
 * vm_free — 由该 VM 的 vCPU 任务在退出前调用（见上面的并发规则）
 *
 * 目前只回收 stage-2（连同它按需分配的那些物理页）。vGIC / vpl011 的状态
 * 都是 vm_t 的一部分，跟着槽位一起被 vm_alloc 的 memset 清掉。
 */
void vm_free(vm_t *vm)
{
    uint64_t flags;

    if (!vm || vm->state == VM_FREE)
        return;

#if ARCH_AARCH64
    stage2_vm_destroy(&vm->s2);     /* 释放按需页 + L3 表 */
#elif ARCH_RISCV64
    rv_gstage_vm_destroy(&vm->gstage);  /* 释放按需页 + L0 表 */
#elif ARCH_X86_64
    x86_ept_vm_destroy(&vm->ept);   /* 释放按需页 + PT 表 */
#endif

    KLOG_INFO("[vmm] vm%u freed (slot %d)\n", vm->vmid, vm->slot);

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    memset(vm, 0, sizeof(*vm));
    vm->state = VM_FREE;
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);
}

int vm_count_used(void)
{
    uint64_t flags;
    int n = 0;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    for (int i = 0; i < MAX_VMS; i++)
        if (g_vm_pool[i].state != VM_FREE)
            n++;
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);
    return n;
}

void vm_request_stop(vm_t *vm)
{
    if (vm)
        vm->stop_req = 1;
}

/*
 * vmm_guest_running — 是否有**任意** VM 正在跑
 *
 * 保留这个"全局"语义是因为 /dev/vmm 的若干判断（还能不能往 guest 写、
 * bootlinux 要不要 -EBUSY）在单 VM 视角下就是这么用的。多 VM 的精细判断
 * 走 vm_get(vmid)->state。
 */
int vmm_guest_running(void)
{
    return vm_count_used() > 0;
}
