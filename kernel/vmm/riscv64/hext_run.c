/*
 * kernel/vmm/riscv64/hext_run.c — RISC-V H-extension 架构钩子实现
 *
 * 实现 vmm.h 声明的四个架构钩子：
 *   vmm_arch_restore_guest_ctx  — 进入 guest 循环前初始化 VS-CSRs
 *   vmm_arch_enter_guest        — 调用 hext_enter_guest（sret → VS-mode）
 *   vmm_arch_exit_handler       — 处理 VS-mode → HS-mode 陷阱
 *   vmm_arch_save_guest_ctx     — guest 退出后清理（当前无操作）
 *
 * 以及 RISC-V VMM 初始化：
 *   hext_vm_init(vm_t *)        — 检查 H-ext 支持，配置 hedeleg/hideleg/hstatus
 *   hext_vcpu_setup(vcpu_t *, entry) — 初始化 vcpu 软件 VMCS
 *
 * 特权级关系：
 *   HS-mode (本内核) → VS-mode (guest) → VU-mode (guest user，本例不用)
 *   VS-mode ecall → scause=10（区别于 U-mode ecall = 8）
 *   WFI 在 hstatus.VTW=1 时：VS-mode 执行 WFI 触发 scause=2（虚拟指令异常）
 *     注：部分 QEMU 版本报 scause=2，另一些直接报 illegal instruction；
 *         本 exit handler 两种都处理。
 */

#include "vmm.h"
#include "klog.h"
#include "string.h"
#include "task/task.h"
#include "riscv64/hext.h"
#include "riscv64/sysreg.h"
#include "riscv64/gstage.h"
#include "vmm_mmio.h"
#include "vmm_uart16550.h"
#include "vmm_vplic.h"
#include "mm_vm.h"      /* phys_to_virt */

/* ── 汇编入口声明 ─────────────────────────────────────────── */
extern int hext_enter_guest(vcpu_t *vcpu);  /* hext_vcpu.S */

/* ── H-extension 支持检测 ─────────────────────────────────── */
static int hext_check_support(void)
{
    /*
     * 读取 misa 检测 H 位（bit 7）。
     * 在 S-mode 可以 csrr misa，但若内核运行在 HS-mode，
     * misa 通常可读（只读）。部分实现可能 trap，此处简单尝试。
     * QEMU 8.2 rv64imafdch 默认含 H-ext，检测必过。
     *
     * 注：RISC-V 没有统一的"虚拟化支持位"，
     *     通过尝试读取 hstatus 来验证 H-ext 存在。
     *     若不支持则触发 illegal instruction，内核会 panic。
     */
    uint64_t hstat = READ_HSTATUS();
    (void)hstat;
    return 1;   /* 若执行到此说明 H-ext 可用 */
}

/* ── 全局 guest 栈（无 EPT，guest 用内核地址空间）─────────── */
#define GUEST_STACK_SIZE  4096
static uint8_t g_guest_stack[MAX_VCPUS][GUEST_STACK_SIZE]
    __attribute__((aligned(16)));

/* ── 全局 MMIO 总线与虚拟设备（静态存储，单 VM）───────────── */
static mmio_bus_t    g_rv_mmio_bus;
static mmio_device_t g_rv_uart_dev;
static mmio_device_t g_rv_plic_dev;

/* ── hext_vcpu_setup：初始化 vcpu 软件 VMCS ──────────────── */
int hext_vcpu_setup(vcpu_t *vcpu, void (*entry)(void))
{
    memset(vcpu->r, 0, sizeof(vcpu->r));

    /* guest PC = entry（内核虚拟地址，与 host 共享地址空间，无 hgatp）*/
    vcpu->vsepc     = (uint64_t)entry;

    /* vsstatus：SPP=0（guest 运行在 VS-mode），SIE=0（中断关闭）
     * VS-mode 的 sstatus.SPP 在 vsstatus.SPP 处，bit 8 */
    vcpu->vsstatus  = 0;

    /* guest trap vector：暂用 entry 作为 guest stvec */
    vcpu->vstvec    = (uint64_t)entry;

    /* vsscratch = 0（guest 初始值）*/
    vcpu->vsscratch = 0;

    /* vsatp：与 host 共享内核页表，guest 可执行 kernel VA 地址的代码
     * vsatp=0（bare）时 guest 会把 kernel VA 当物理地址使用，立即 page fault */
    vcpu->vsatp     = CSR_READ(satp);

    /* vsie = 0：guest 中不使能任何中断 */
    vcpu->vsie      = 0;

    /* guest 栈：r[2] = sp */
    vcpu->r[2]      = (uint64_t)(g_guest_stack[vcpu->vcpu_id] + GUEST_STACK_SIZE);

    KLOG_INFO("[HEXT] vcpu%d setup: entry=0x%llx sp=0x%llx\n",
              vcpu->vcpu_id,
              vcpu->vsepc,
              vcpu->r[2]);
    return 0;
}

/* ── hext_vm_init：VM 初始化（检查 H-ext，配置全局 hstatus）── */
int hext_vm_init(vm_t *vm)
{
    int nr = vm->cfg.nr_vcpus;
    int i;

    if (nr < 1 || nr > MAX_VCPUS) {
        KLOG_ERROR("[HEXT] vm_init: invalid nr_vcpus=%d\n", nr);
        return -1;
    }

    /* 1. 检查 H-ext 支持 */
    if (!hext_check_support()) {
        KLOG_ERROR("[HEXT] Hypervisor Extension not supported!\n");
        return -1;
    }
    KLOG_INFO("[HEXT] H-extension available\n");

    /* 2. hedeleg/hideleg: 保留默认值 0（不委托）
     *
     * 默认行为：所有 VS-mode 异常和中断直接陷入 HS-mode，
     * 由 hext_trap_vector 处理。对 WFI(VTW=1) + ecall(scause=10) 测试足够。
     *
     * 注：在 QEMU 8.2 上，csrw hedeleg/hideleg 会触发 illegal instruction
     *     异常（推测 OpenSBI/QEMU 不允许 HS-mode 写这两个 CSR），暂时跳过。
     * TODO: 调查 hedeleg/hideleg 写入失败原因（QEMU virt 配置或 OpenSBI 限制）
     */
    KLOG_INFO("[HEXT] hedeleg/hideleg left at default (0) — all VS traps -> HS\n");

    /* 3. 配置 hstatus.VTW=1：WFI 在 VS-mode 陷入 HS-mode
     *    （等价 AArch64 的 HCR_EL2.TWI=1）
     *    VTW = bit 21 */
    KLOG_INFO("[HEXT] setting hstatus.VTW...\n");
    uint64_t hs = READ_HSTATUS();
    hs |= HSTATUS_VTW;
    WRITE_HSTATUS(hs);
    KLOG_INFO("[HEXT] hstatus.VTW set, init vcpus...\n");

    /* 4. 初始化 vCPU 数组 */
    for (i = 0; i < nr; i++) {
        vcpu_t *vcpu = &vm->vcpus[i];
        memset(vcpu, 0, sizeof(*vcpu));
        vcpu->vcpu_id  = i;
        vcpu->launched = 0;
        vcpu->vm       = vm;
    }
    vm->nr_vcpus = nr;

    /* 5. G-stage（hgatp Stage-2）内存隔离：仅当 VM 请求了独立 guest RAM
     *    （cfg.mem_size != 0）时启用。移植自 x-kernel gstage.rs。
     *
     *    默认（mem_size==0）保持原「共享 host 地址空间、无 hgatp」路径不变，
     *    即当前 vmm_test 使用的已验证行为，避免回归。
     *
     *    注：hpa_base 暂用 identity（= mem_base）；后续 Phase 引入 guest RAM
     *    预留（reserve_guest_ram）后改为分配得到的 HPA。 */
    if (vm->cfg.mem_size != 0) {
        rv_gstage_init(vm->cfg.mem_base, vm->cfg.mem_size,
                       vm->cfg.mem_base /* hpa_base = identity */, 1u);
        /* 只映射 guest RAM，其余置无效 → 设备访问陷入 HS-mode 模拟 */
        rv_gstage_enable_mmio_trap();
        rv_gstage_activate();

        /* 建立 MMIO 总线并注册虚拟 16550A 控制台 */
        mmio_bus_init(&g_rv_mmio_bus);
        if (uart16550_init(&g_rv_uart_dev, &g_rv_mmio_bus) != 0)
            KLOG_WARN("[HEXT] uart16550 registration failed\n");
        if (vplic_init(&g_rv_plic_dev, &g_rv_mmio_bus, (uint32_t)nr) != 0)
            KLOG_WARN("[HEXT] vplic registration failed\n");
        vm->mmio_bus = &g_rv_mmio_bus;

        KLOG_INFO("[HEXT] G-stage isolation enabled (mem=0x%llx+0x%llx)\n",
                  (unsigned long long)vm->cfg.mem_base,
                  (unsigned long long)vm->cfg.mem_size);
        KLOG_INFO("[HEXT] MMIO bus ready: virtual 16550A @0x%llx\n",
                  (unsigned long long)UART16550_BASE);
    } else {
        KLOG_INFO("[HEXT] G-stage disabled: guest shares host address space\n");
    }

    KLOG_INFO("[HEXT] vm_init: %d vCPU(s) ready\n", nr);
    return 0;
}

/* ================================================================
 * 架构钩子实现
 * ================================================================ */

/* restore_guest_ctx：进入 vmm_run_vcpu 主循环前的一次性初始化（当前无操作）*/
void vmm_arch_restore_guest_ctx(vcpu_t *vcpu)
{
    (void)vcpu;
    /* VS-CSRs 在每次 hext_enter_guest 中动态恢复，无需预置 */
}

/*
 * vcpu_timer_irq_on_entry — 进入 guest 前注入 VS 定时器中断
 *
 * 对标 x-kernel vdev/riscv64/timer.rs 的 RiscvTimerHook::on_entry：
 * guest 经 SBI 设置了截止时间且已到期 → 置 hvip.VSTIP，使 VS-mode 看到
 * 定时器中断挂起（无需 vPLIC）。
 */
static void vcpu_timer_irq_on_entry(vcpu_t *vcpu)
{
    uint64_t now = READ_TIME();
    int pending = (vcpu->timer_deadline != 0 && now >= vcpu->timer_deadline);

    hext_set_vs_timer_irq(pending);
}

/* enter_guest：执行一次 sret → VS-mode，等到 guest 陷入后返回 */
int vmm_arch_enter_guest(vcpu_t *vcpu)
{
    /*
     * 每次 task_yield 后需要重设 hstatus.SPV+SPVP 和 sstatus.SPP。
     * 这些已在 hext_enter_guest 汇编内完成，此处只需调用。
     */
    vcpu_timer_irq_on_entry(vcpu);

    int ret = hext_enter_guest(vcpu);
    if (ret) {
        vcpu->launched = 1;
    }
    return ret;
}

/* ── WFI 陷阱处理（scause=2/22: virtual instruction，hstatus.VTW=1）──
 *
 * 移植自 x-kernel arch/riscv64/mod.rs handle_wfi：若 guest 通过 SBI 设置了
 * 定时器截止且已到期，直接 resume；否则让出 CPU。avatar 无
 * interruptible_sleep_until，用 task_yield 近似（定时器中断会周期性唤醒）。
 */
static int handle_wfi(vcpu_t *vcpu)
{
    /* 步进 guest PC 越过 WFI 指令（4 字节）*/
    vcpu->vsepc += 4;

    uint64_t now = READ_TIME();
    if (vcpu->timer_deadline != 0 && now >= vcpu->timer_deadline)
        return EL2_RESUME;   /* 定时器已到期，立即继续 guest */

    /* 让出 CPU，等价 AArch64 WFI→yield */
    task_yield();
    return EL2_RESUME;
}

/* ── guest 控制台输出（SBI console_putchar）───────────────── */
static void sbi_console_putchar(uint8_t byte)
{
    klog_putchar((char)byte);
}

/* ================================================================
 * MMIO 陷入模拟（移植自 x-kernel arch/riscv64/mod.rs）
 *
 * G-stage 把设备 IPA 置为无效后，guest 访问设备 → G-stage page fault
 * （cause 20/21/23），htval 给出 guest 物理地址。VMM 需：
 *   1. 从 guest PC 取指（经 guest 页表 vsatp 翻译 VA→GPA，再经 G-stage）
 *   2. 解码出访存指令的 方向/宽度/寄存器
 *   3. 交给 MMIO 总线分发，读结果写回 guest 寄存器，步进 PC
 * ================================================================ */

/* guest 物理地址读取（GPA → HPA → 内核直接映射）*/
static int guest_read_u64(uint64_t gpa, uint64_t *out)
{
    uint64_t hpa;
    if (!rv_gstage_gpa_to_hpa(gpa, &hpa))
        return 0;
    *out = *(volatile uint64_t *)phys_to_virt(hpa);
    return 1;
}

static int guest_read_u32(uint64_t gpa, uint32_t *out)
{
    uint64_t hpa;
    if (!rv_gstage_gpa_to_hpa(gpa, &hpa))
        return 0;
    *out = *(volatile uint32_t *)phys_to_virt(hpa);
    return 1;
}

static int guest_read_u16(uint64_t gpa, uint16_t *out)
{
    uint64_t hpa;
    if (!rv_gstage_gpa_to_hpa(gpa, &hpa))
        return 0;
    *out = *(volatile uint16_t *)phys_to_virt(hpa);
    return 1;
}

/* guest 虚拟地址 → guest 物理地址（软件遍历 vsatp 指向的 Sv39/Sv48 页表）*/
static int guest_va_to_gpa(vcpu_t *vcpu, uint64_t va, uint64_t *gpa_out)
{
    const uint64_t PTE_V = 1u << 0, PTE_R = 1u << 1, PTE_W = 1u << 2, PTE_X = 1u << 3;

    uint64_t satp = vcpu->vsatp;
    uint64_t mode = satp >> 60;
    int levels;

    switch (mode) {
    case 8:  levels = 3; break;   /* Sv39 */
    case 9:  levels = 4; break;   /* Sv48 */
    case 10: levels = 5; break;   /* Sv57 */
    default:
        *gpa_out = va;   /* 无页表：VA 即 GPA（bare 模式）*/
        return 1;
    }

    if (satp == 0) {
        *gpa_out = va;
        return 1;
    }

    uint64_t table_gpa = (satp & ((1ULL << 44) - 1)) << 12;

    for (int level = levels - 1; level >= 0; level--) {
        uint64_t vpn = (va >> (12 + level * 9)) & 0x1FF;
        uint64_t pte;

        if (!guest_read_u64(table_gpa + vpn * 8, &pte))
            return 0;
        if ((pte & PTE_V) == 0)
            return 0;
        if ((pte & PTE_W) && !(pte & PTE_R))
            return 0;   /* 保留组合 */

        if (pte & (PTE_R | PTE_X)) {
            /* 叶项：4KiB(level0) / 2MiB / 1GiB */
            uint64_t ppn = (pte >> 10) & ((1ULL << 44) - 1);
            uint64_t page_mask = (1ULL << (12 + level * 9)) - 1;
            *gpa_out = (ppn << 12) | (va & page_mask);
            return 1;
        }
        table_gpa = ((pte >> 10) & ((1ULL << 44) - 1)) << 12;
    }
    return 0;
}

/* 一条 MMIO 访存指令的解码结果 */
typedef struct {
    int      is_write;
    uint8_t  size;      /* 字节数 */
    uint32_t reg;       /* 目标/源寄存器号 */
    uint64_t inst_len;  /* 指令长度（2=压缩, 4=普通）*/
} mmio_access_t;

static int decode_compressed_mmio(uint16_t inst, int is_store,
                                  mmio_access_t *acc)
{
    uint32_t opcode = inst & 0x3;
    uint32_t funct3 = (inst >> 13) & 0x7;

    if (!is_store) {
        if (opcode == 0 && (funct3 == 2 || funct3 == 3)) {
            acc->is_write = 0;
            acc->size = (funct3 == 2) ? 4 : 8;
            acc->reg = ((inst >> 2) & 0x7) + 8;
            acc->inst_len = 2;
            return 1;
        }
        if (opcode == 2 && (funct3 == 2 || funct3 == 3)) {
            acc->is_write = 0;
            acc->size = (funct3 == 2) ? 4 : 8;
            acc->reg = (inst >> 7) & 0x1F;
            acc->inst_len = 2;
            return 1;
        }
    } else {
        if (opcode == 0 && (funct3 == 6 || funct3 == 7)) {
            acc->is_write = 1;
            acc->size = (funct3 == 6) ? 4 : 8;
            acc->reg = ((inst >> 2) & 0x7) + 8;
            acc->inst_len = 2;
            return 1;
        }
        if (opcode == 2 && (funct3 == 6 || funct3 == 7)) {
            acc->is_write = 1;
            acc->size = (funct3 == 6) ? 4 : 8;
            acc->reg = (inst >> 2) & 0x1F;
            acc->inst_len = 2;
            return 1;
        }
    }
    return 0;
}

static int decode_mmio_access(uint64_t inst, int is_store, mmio_access_t *acc)
{
    /* 压缩指令（低 2 位 != 11）先处理 */
    if ((inst & 0x3) != 0x3)
        return decode_compressed_mmio((uint16_t)inst, is_store, acc);

    uint32_t w = (uint32_t)inst;
    uint32_t opcode = w & 0x7f;
    uint32_t funct3 = (w >> 12) & 0x7;

    if (opcode == 0x03 && !is_store) {          /* LOAD */
        acc->is_write = 0;
        switch (funct3) {
        case 0: case 4: acc->size = 1; break;
        case 1: case 5: acc->size = 2; break;
        case 2: case 6: acc->size = 4; break;
        case 3:         acc->size = 8; break;
        default: return 0;
        }
        acc->reg = (w >> 7) & 0x1F;
        acc->inst_len = 4;
        return 1;
    }
    if (opcode == 0x23 && is_store) {           /* STORE */
        acc->is_write = 1;
        switch (funct3) {
        case 0: acc->size = 1; break;
        case 1: acc->size = 2; break;
        case 2: acc->size = 4; break;
        case 3: acc->size = 8; break;
        default: return 0;
        }
        acc->reg = (w >> 20) & 0x1F;
        acc->inst_len = 4;
        return 1;
    }
    return 0;
}

/* 取 guest PC 处的指令；取指失败时回落到 htinst */
static int mmio_instruction(vcpu_t *vcpu, uint64_t *inst_out)
{
    uint64_t pc_gpa;

    if (guest_va_to_gpa(vcpu, vcpu->vsepc, &pc_gpa)) {
        uint16_t lo;
        if (guest_read_u16(pc_gpa, &lo)) {
            if ((lo & 0x3) != 0x3) {
                *inst_out = lo;      /* 压缩指令 */
                return 1;
            }
            uint32_t full;
            if (guest_read_u32(pc_gpa, &full)) {
                *inst_out = full;
                return 1;
            }
        }
    }

    if (vcpu->htval_save != 0) {
        *inst_out = vcpu->htval_save;   /* 部分实现经 htinst 提供指令 */
        return 1;
    }
    return 0;
}

/* ── G-stage MMIO fault（cause 20/21/23）──────────────────── */
static int handle_gstage_fault(vcpu_t *vcpu, uint64_t code)
{
    /* 出错 GPA：htval 保存的是 GPA>>2，stval 低位给出页内偏移 */
    uint64_t gpa = (vcpu->htval_save << 2) | (vcpu->stval_save & 0xFFF);
    int is_store = (code == 23);
    uint64_t inst;
    mmio_access_t acc;

    if (!mmio_instruction(vcpu, &inst)) {
        KLOG_ERROR("[HEXT] vcpu%d: cannot fetch MMIO inst, pc=0x%llx htinst=0x%llx\n",
                   vcpu->vcpu_id,
                   (unsigned long long)vcpu->vsepc,
                   (unsigned long long)vcpu->htval_save);
        return EL2_EXIT;
    }
    if (!decode_mmio_access(inst, is_store, &acc)) {
        KLOG_ERROR("[HEXT] vcpu%d: undecodable MMIO inst=0x%llx cause=%llu\n",
                   vcpu->vcpu_id, (unsigned long long)inst,
                   (unsigned long long)code);
        return EL2_EXIT;
    }

    uint64_t value = acc.is_write ? vcpu->r[acc.reg] : 0;
    uint64_t out = 0;

    if (vcpu->vm && vcpu->vm->mmio_bus &&
        mmio_bus_handle(vcpu->vm->mmio_bus, gpa, acc.is_write, acc.size,
                        value, (uint32_t)vcpu->vcpu_id, &out)) {
        if (!acc.is_write && acc.reg != 0)
            vcpu->r[acc.reg] = out;
        vcpu->vsepc += acc.inst_len;
        return EL2_RESUME;
    }

    KLOG_ERROR("[HEXT] vcpu%d: unhandled G-stage %s fault gpa=0x%llx pc=0x%llx\n",
               vcpu->vcpu_id, acc.is_write ? "write" : "read",
               (unsigned long long)gpa, (unsigned long long)vcpu->vsepc);
    return EL2_EXIT;
}

/* ── VS-mode ecall 处理（scause=10）─────────────────────────
 *
 * 移植自 x-kernel arch/riscv64/mod.rs handle_vs_ecall：
 *   - guest 测试魔数 PRINT/DONE
 *   - SBI legacy：set_timer / console_putchar / console_getchar
 *   - SBI base：spec version / impl id / probe extension
 *   - SBI TIME.set_timer / RFENCE（单 vCPU 下为 no-op 成功）
 */
static int handle_vs_ecall(vcpu_t *vcpu)
{
    uint64_t ext  = vcpu->r[17];  /* a7 = SBI EID / hypercall 号 */
    uint64_t func = vcpu->r[16];  /* a6 = SBI FID                */
    uint64_t arg0 = vcpu->r[10];  /* a0 = 第一个参数             */

    /* 步进 guest PC 越过 ecall（4 字节） */
    vcpu->vsepc += 4;

    switch (ext) {
    case GUEST_ECALL_PRINT:
        if (arg0 % 20 == 0)
            KLOG_INFO("[HEXT] ECALL_PRINT: iter=%llu (vcpu%d)\n",
                      (unsigned long long)arg0, vcpu->vcpu_id);
        return EL2_RESUME;

    case GUEST_ECALL_DONE:
        KLOG_INFO("[HEXT] ECALL_DONE: vcpu%d exiting\n", vcpu->vcpu_id);
        return EL2_VMEXIT;

    case SBI_LEGACY_SET_TIMER:
        vcpu->timer_deadline = arg0;
        return EL2_RESUME;

    case SBI_LEGACY_CONSOLE_PUTCHAR:
        sbi_console_putchar((uint8_t)arg0);
        return EL2_RESUME;

    case SBI_LEGACY_CONSOLE_GETCHAR:
        vcpu->r[10] = (uint64_t)-1;   /* 无输入 */
        return EL2_RESUME;

    case SBI_EXT_BASE: {
        uint64_t value;
        switch (func) {
        case SBI_BASE_GET_SPEC_VERSION: value = 0x00000002; break; /* v0.2 */
        case SBI_BASE_GET_IMPL_ID:      value = 0x584b564d; break; /* "XKVM" */
        case SBI_BASE_GET_IMPL_VERSION: value = 1;          break;
        case SBI_BASE_PROBE_EXTENSION:
            switch (arg0) {
            case SBI_EXT_BASE:
            case SBI_EXT_TIME:
            case SBI_EXT_RFENCE:
            case SBI_LEGACY_CONSOLE_PUTCHAR:
            case SBI_LEGACY_CONSOLE_GETCHAR:
                value = 1; break;
            default:
                value = 0; break;
            }
            break;
        default:
            vcpu->r[10] = SBI_ERR_NOT_SUPPORTED;
            vcpu->r[11] = 0;
            return EL2_RESUME;
        }
        vcpu->r[10] = SBI_SUCCESS;
        vcpu->r[11] = value;
        return EL2_RESUME;
    }

    case SBI_EXT_TIME:
        if (func == SBI_TIME_SET_TIMER) {
            vcpu->timer_deadline = arg0;
            vcpu->r[10] = SBI_SUCCESS;
            vcpu->r[11] = 0;
            return EL2_RESUME;
        }
        vcpu->r[10] = SBI_ERR_NOT_SUPPORTED;
        vcpu->r[11] = 0;
        return EL2_RESUME;

    case SBI_EXT_RFENCE:
        /* 单 vCPU 无远端 hart 需同步；报告成功以兼容会探测 RFENCE 的 guest。*/
        vcpu->r[10] = SBI_SUCCESS;
        vcpu->r[11] = 0;
        return EL2_RESUME;

    default:
        KLOG_WARN("[HEXT] Unknown SBI ecall ext=0x%llx func=0x%llx (vcpu%d)\n",
                  (unsigned long long)ext, (unsigned long long)func,
                  vcpu->vcpu_id);
        vcpu->r[10] = SBI_ERR_NOT_SUPPORTED;
        vcpu->r[11] = 0;
        return EL2_RESUME;
    }
}

/* exit_handler：处理一次 guest → HS-mode 陷阱 */
int vmm_arch_exit_handler(vcpu_t *vcpu)
{
    uint64_t cause = vcpu->scause_save;
    int      is_int = (int)(cause >> 63);
    uint64_t code  = cause & ~(1ULL << 63);

    /* 对标 x-kernel vdev/riscv64/timer.rs 的 RiscvTimerHook::on_exit：
     * 已退出 guest，清除注入的 VS 定时器挂起位，避免残留导致下次进入
     * guest 立即重复触发。*/
    hext_set_vs_timer_irq(0);

    if (is_int) {
        /* 中断（定时器/外部）陷入 HS-mode：短暂开中断让宿主内核处理，
         * 然后 resume。对标 kvmm exit_handler 的 is_interrupt 分支。 */
        KLOG_DEBUG("[HEXT] vcpu%d interrupt: code=%llu\n",
                   vcpu->vcpu_id, (unsigned long long)code);
        /* 直接 yield，让 kernel 的 timer handler 运行 */
        task_yield();
        return EL2_RESUME;
    }

    switch (code) {
    case 2:
    case 22:
        /*
         * scause=2 (Illegal instruction) 或 22 (Virtual instruction)：
         * hstatus.VTW=1 时 VS-mode 的 WFI 陷入此处。不同 QEMU 版本报 2 或 22，
         * 两者都当 WFI 处理（步进 + 按定时器截止 yield/resume）。
         */
        return handle_wfi(vcpu);

    case CAUSE_VS_ECALL:   /* 10 */
        return handle_vs_ecall(vcpu);

    case 20:   /* Instruction G-stage Page Fault */
    case 21:   /* Load G-stage Page Fault      */
    case 23:   /* Store/AMO G-stage Page Fault */
        /* 设备 MMIO：G-stage 未映射 → 陷入模拟（移植自 kvmm mod.rs）*/
        return handle_gstage_fault(vcpu, code);

    case 12:   /* Instruction Page Fault（不应发生：vsatp=0 无页表）*/
    case 13:   /* Load Page Fault */
    case 15:   /* Store Page Fault */
        KLOG_ERROR("[HEXT] vcpu%d PF: cause=%llu vsepc=0x%llx stval=0x%llx\n",
                   vcpu->vcpu_id,
                   (unsigned long long)code,
                   (unsigned long long)vcpu->vsepc,
                   (unsigned long long)vcpu->stval_save);
        return EL2_EXIT;

    default:
        KLOG_ERROR("[HEXT] vcpu%d unhandled exit: cause=%llu vsepc=0x%llx\n",
                   vcpu->vcpu_id,
                   (unsigned long long)code,
                   (unsigned long long)vcpu->vsepc);
        return EL2_EXIT;
    }
}

/* save_guest_ctx：guest 退出后保存上下文（当前无额外操作）*/
void vmm_arch_save_guest_ctx(vcpu_t *vcpu)
{
    (void)vcpu;
}
