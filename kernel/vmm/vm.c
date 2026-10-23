/*
   kernel/vmm/vm.c — VM 对象：池, 生命周期, 创建

   从 vmm.c 拆出来的。这里只管「一个 guest VM 这个对象本身」：
     - 静态 VM 池与槽位分配(vm_alloc / vm_get / vm_free / vm_count_used)
     - 生命周期状态机(VM_FREE → LOADING → RUNNING → DYING → FREE)
     - 停止请求与"有没有 VM 在跑"的查询
     - vm_create：按架构转发到各后端的初始化

   不在这里的：
     - vCPU 内核任务(vcpu.c)—— 它只是执行这个 VM 的载体
     - 主循环 vmm_run_vcpu(vcpu.c)
     - 各架构的初始化实现(aarch64/vm_init.c, x86_64/vmx.c, riscv64/hext_run.c)

   注意： 本文件不得依赖任何具体的中断控制器 —— 架构状态由 vm_t 里的守卫块承载,
      控制器注册集中在各架构的 vm_init 里。(第二轮做中断抽象时的落点。)
*/

#include "vmm/vmm.h"
#include "klog.h"
#include "spinlock.h" /* g_vm_pool_lock */
#include "string.h"   /* memset(vm_alloc / vm_free) */

/* ── 公开 API ─────────────────────────────────────────────── */

/*
   vm_create — 建一个 VM

   真正的实现在各架构的 vmm_arch_vm_init()(见 include/vmm/vmm.h 的架构钩子节)：
     aarch64 → kernel/vmm/aarch64/vm_init.c
     x86_64  → kernel/vmm/x86_64/vmx.c
     riscv64 → kernel/vmm/riscv64/hext_run.c

   这里只是一层转发 —— 编译期就解析成一条直接调用, 没有运行期分派。
   从前这里是 `#if/#elif` + 两处就地 extern(其中 aarch64 那个函数还内联在
   vmm.c 里), 三个架构三种放法；钩子化之后三边对称。
*/
int vm_create(vm_t *vm)
{
    return vmm_arch_vm_init(vm);
}

/* ── 宿主侧 guest 生命周期：VM 池 ─────────────────────────────

   从前这里只有一个 VM —— guest_loader.c 里一个 `static vm_t vm`, 加上下面
   两个全局标志(running / stop)。那套从数据结构上就假定了"内核里最多一个
   VM"：第二个 VM 会直接覆盖第一个的对象, 而 stop 请求也没法说清是给谁的。

   现在改成静态池。为什么不用动态分配：vm_t 里嵌着 vGIC(GICv3 时约 70 KB)
   与 stage-2 静态表, sizeof 有几十 KB, 动态分配要几十页连续物理内存,
   还会丢掉表所需的页对齐保证；静态池一共几百 KB BSS, 相对内核窗口可忽略。

   并发规则(重要)：
     - 槽位与 state 由 g_vm_pool_lock 保护(IRQ-safe：vmm_dev 的任务上下文
       和 vCPU 任务上下文都会碰)；
     - 跨任务只传 vmid, 不传 vm_t *, 也不做引用计数 —— 谁要操作某个 VM
       就现场 vm_get(vmid) 取一次, 用完即放；
     - 销毁由该 VM 的 vCPU 任务自己完成(vmm_run_vcpu 返回后调 vm_free),
       杜绝"任务还在跑, 槽位已被回收"。外部想销毁就置 stop_req 等它收拾。
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
            found->slot = i;
            found->vmid = g_next_vmid;
            g_next_vmid = (g_next_vmid >= 255) ? 1 : (g_next_vmid + 1);
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

/* ── 每 vCPU 的生命周期记账 ─────────────────────────────────── */

void vm_state_set(vm_t *vm, int state)
{
    uint64_t flags;

    if (!vm)
        return;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    if (vm->state != VM_FREE && vm->state != VM_DYING)
        vm->state = state;
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);
}

/*
   vm_vcpu_started — vcpu_task_create() 在 task_create 之前调用

   必须先加计数再建任务：新任务可能立刻在另一颗核上跑起来并退出,
   若先建任务再加计数, 那次 vm_vcpu_stopped() 会把计数减成负数, 并误判
   「我可以销毁 VM」。
*/
void vm_vcpu_started(vm_t *vm)
{
    uint64_t flags;

    if (!vm)
        return;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    if (vm->state != VM_FREE && vm->state != VM_DYING)
        vm->nr_vcpu_alive++;
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);
}

/* 任务创建失败时回滚 vm_vcpu_started()。不销毁 VM —— 调用方还在用。 */
void vm_vcpu_start_abort(vm_t *vm)
{
    uint64_t flags;

    if (!vm)
        return;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    if (vm->nr_vcpu_alive > 0)
        vm->nr_vcpu_alive--;
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);
}

/*
   vm_vcpu_stopped — vCPU 任务退出时调用(由 vcpu_task_fn 收尾)

   只有最后一个退出的 vCPU 才触发 vm_free()。多 vCPU 时第一个退出
   不能拆 stage-2 —— 兄弟 vCPU 还在用。
*/
void vm_vcpu_stopped(vm_t *vm)
{
    uint64_t flags;
    int last = 0;

    if (!vm)
        return;

    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    if (vm->state != VM_FREE) {
        if (vm->nr_vcpu_alive > 0)
            vm->nr_vcpu_alive--;
        last = (vm->nr_vcpu_alive == 0);
    }
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);

    if (last)
        vm_free(vm);
}

/*
   vm_free — 拆一个已经没有任何存活 vCPU 的 VM

   正常路径由 vm_vcpu_stopped() 在最后一颗 vCPU 退出时调用；错误路径
   (vm_create/装载失败, 一个任务都没建)由调用方直接调用。

   目前只回收 stage-2(连同它按需分配的那些物理页)。vGIC / vpl011 的状态
   都是 vm_t 的一部分, 跟着槽位一起被 vm_alloc 的 memset 清掉。
*/
void vm_free(vm_t *vm)
{
    uint64_t flags;

    if (!vm)
        return;

    /*
       state = DYING 同时充当「销毁进行中」的排他标记：
         - nr_vcpu_alive != 0：还有 vCPU 在跑, 绝不能拆；
         - 已经是 DYING/FREE：另一个调用者已经/正在拆, 重复调用直接返回。
       拆完之前不把槽位置成 FREE —— 否则 vm_alloc 会立刻把它分给别人。
    */
    spin_lock_irqsave(&g_vm_pool_lock, &flags);
    if (vm->state == VM_FREE || vm->state == VM_DYING || vm->nr_vcpu_alive != 0) {
        spin_unlock_irqrestore(&g_vm_pool_lock, flags);
        return;
    }
    vm->state = VM_DYING;
    spin_unlock_irqrestore(&g_vm_pool_lock, flags);

#if ARCH_AARCH64
    stage2_vm_destroy(&vm->s2); /* 释放按需页 + L3 表 */
#elif ARCH_RISCV64
    rv_gstage_vm_destroy(&vm->gstage); /* 释放按需页 + L0 表 */
#elif ARCH_X86_64
    x86_ept_vm_destroy(&vm->ept); /* 释放按需页 + PT 表 */
#endif

    /*
       归还设备槽位。

       注意： 必须在 memset 之前, 且在池锁外面：
         - 之前：下面那句 memset 会把 vm->slot 清成 0, 之后就没法索引池了；
         - 外面：各设备的 destroy 靠自己的锁串行化(池锁里不该做设备操作)。
       从前设备状态就嵌在 vm_t 里, 那句 memset 顺手就把它们清了 —— 搬到
       静态池之后 memset 碰不到池子, 必须显式归还。
    */
    vmm_arch_vm_destroy(vm);

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
    if (!vm)
        return;

    vm->stop_req = 1;

    /*
       把「等下一次自然 exit」缩短到「现在退出」。

       单 vCPU 时这只是省一次等待；多 vCPU 时它是必需的：停在 guest 里的
       兄弟 vCPU 可能正跑一个长时间不陷入的循环, 只置标志会让整个 VM
       迟迟收不齐, 槽位也回收不了。kick 走 resched IPI, 目标在 guest 里
       会被硬件转成一次 VM-exit, 主循环顶部看到 stop_req 后返回。
    */
    vmm_vm_for_each_vcpu(vm, vcpu)
    {
        /*
           还没被 CPU_ON 放行的 AP 任务睡在 start completion 上, kick 对
           它无效(state 还是 OFFLINE)。必须完成闸门把它叫醒, 让它在
           vmm_run_vcpu 顶部看到 stop_req 后退出；否则 VM 永远等不齐所有
           vCPU, 槽位也回收不了。
        */
        if (vcpu->state == VCPU_STATE_OFFLINE && vcpu->task)
            completion_complete_all(&vcpu->start);
        vmm_vcpu_kick(vcpu);
    }
}

/*
   vmm_guest_running — 是否有任意 VM 正在跑

   保留这个"全局"语义是因为 /dev/vmm 的若干判断(还能不能往 guest 写,
   bootlinux 要不要 -EBUSY)在单 VM 视角下就是这么用的。多 VM 的精细判断
   走 vm_get(vmid)->state。
*/
int vmm_guest_running(void)
{
    return vm_count_used() > 0;
}
